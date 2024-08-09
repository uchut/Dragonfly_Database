// Copyright 2022, DragonflyDB authors.  All rights reserved.
// See LICENSE for licensing terms.
//
#include "facade/reply_builder.h"

#include <absl/cleanup/cleanup.h>
#include <absl/container/fixed_array.h>
#include <absl/strings/numbers.h>
#include <absl/strings/str_cat.h>
#include <double-conversion/double-to-string.h>

#include "absl/strings/escaping.h"
#include "base/logging.h"
#include "core/heap_size.h"
#include "facade/error.h"
#include "util/fibers/proactor_base.h"

using namespace std;
using absl::StrAppend;
using namespace double_conversion;

namespace facade {

namespace {

inline iovec constexpr IoVec(std::string_view s) {
  iovec r{const_cast<char*>(s.data()), s.size()};
  return r;
}

constexpr char kCRLF[] = "\r\n";
constexpr char kErrPref[] = "-ERR ";
constexpr char kSimplePref[] = "+";
constexpr char kNullStringR2[] = "$-1\r\n";
constexpr char kNullStringR3[] = "_\r\n";

constexpr unsigned kConvFlags =
    DoubleToStringConverter::UNIQUE_ZERO | DoubleToStringConverter::EMIT_POSITIVE_EXPONENT_SIGN;

DoubleToStringConverter dfly_conv(kConvFlags, "inf", "nan", 'e', -6, 21, 6, 0);

const char* NullString(bool resp3) {
  return resp3 ? "_\r\n" : "$-1\r\n";
}

}  // namespace

SinkReplyBuilder::MGetResponse::~MGetResponse() {
  while (storage_list) {
    auto* next = storage_list->next;
    delete[] reinterpret_cast<char*>(storage_list);
    storage_list = next;
  }
}

SinkReplyBuilder::SinkReplyBuilder(::io::Sink* sink)
    : sink_(sink),
      should_batch_(false),
      should_aggregate_(false),
      has_replied_(true),
      send_active_(false) {
}

void SinkReplyBuilder::CloseConnection() {
  if (!ec_)
    ec_ = std::make_error_code(std::errc::connection_aborted);
}

void SinkReplyBuilder::ResetThreadLocalStats() {
  tl_facade_stats->reply_stats = {};
}

void SinkReplyBuilder::Send(const iovec* v, uint32_t len) {
  has_replied_ = true;
  DCHECK(sink_);
  constexpr size_t kMaxBatchSize = 1024;

  size_t bsize = 0;
  for (unsigned i = 0; i < len; ++i) {
    bsize += v[i].iov_len;
  }

  // Allow batching with up to kMaxBatchSize of data.
  if ((should_batch_ || should_aggregate_) && (batch_.size() + bsize < kMaxBatchSize)) {
    batch_.reserve(batch_.size() + bsize);
    for (unsigned i = 0; i < len; ++i) {
      std::string_view src((char*)v[i].iov_base, v[i].iov_len);
      DVLOG(3) << "Appending to stream " << absl::CHexEscape(src);
      batch_.append(src.data(), src.size());
    }
    DVLOG(2) << "Batched " << bsize << " bytes";
    return;
  }

  int64_t before_ns = util::fb2::ProactorBase::GetMonotonicTimeNs();
  error_code ec;
  send_active_ = true;
  tl_facade_stats->reply_stats.io_write_cnt++;
  tl_facade_stats->reply_stats.io_write_bytes += bsize;
  DVLOG(2) << "Writing " << bsize << " bytes of len " << len;

  if (batch_.empty()) {
    ec = sink_->Write(v, len);
  } else {
    DVLOG(3) << "Sending batch to stream :" << absl::CHexEscape(batch_);

    tl_facade_stats->reply_stats.io_write_bytes += batch_.size();
    if (len == UIO_MAXIOV) {
      ec = sink_->Write(io::Buffer(batch_));
      if (!ec) {
        ec = sink_->Write(v, len);
      }
    } else {
      iovec tmp[len + 1];
      tmp[0].iov_base = batch_.data();
      tmp[0].iov_len = batch_.size();
      copy(v, v + len, tmp + 1);
      ec = sink_->Write(tmp, len + 1);
    }
    batch_.clear();
  }
  send_active_ = false;
  int64_t after_ns = util::fb2::ProactorBase::GetMonotonicTimeNs();
  tl_facade_stats->reply_stats.send_stats.count++;
  tl_facade_stats->reply_stats.send_stats.total_duration += (after_ns - before_ns) / 1'000;

  if (ec) {
    DVLOG(1) << "Error writing to stream: " << ec.message();
    ec_ = ec;
  }
}

void SinkReplyBuilder::SendRaw(std::string_view raw) {
  iovec v = {IoVec(raw)};

  Send(&v, 1);
}

void SinkReplyBuilder::ExpectReply() {
  has_replied_ = false;
}

bool SinkReplyBuilder::HasReplied() const {
  return has_replied_;
}

void SinkReplyBuilder::SendError(ErrorReply error) {
  if (error.status)
    return SendError(*error.status);

  SendError(error.ToSv(), error.kind);
}

void SinkReplyBuilder::SendError(OpStatus status) {
  if (status == OpStatus::OK) {
    SendOk();
  } else {
    SendError(StatusToMsg(status));
  }
}

void SinkReplyBuilder::StartAggregate() {
  DVLOG(1) << "StartAggregate";
  should_aggregate_ = true;
}

void SinkReplyBuilder::StopAggregate() {
  DVLOG(1) << "StopAggregate";
  should_aggregate_ = false;

  if (should_batch_)
    return;

  FlushBatch();
}

void SinkReplyBuilder::SetBatchMode(bool batch) {
  DVLOG(1) << "SetBatchMode(" << (batch ? "true" : "false") << ")";
  should_batch_ = batch;
}

void SinkReplyBuilder::FlushBatch() {
  if (batch_.empty())
    return;

  error_code ec = sink_->Write(io::Buffer(batch_));
  batch_.clear();
  if (ec) {
    DVLOG(1) << "Error flushing to stream: " << ec.message();
    ec_ = ec;
  }
}

size_t SinkReplyBuilder::UsedMemory() const {
  return dfly::HeapSize(batch_);
}

SinkReplyBuilder2::ReplyAggregator::~ReplyAggregator() {
  rb->batched_ = prev;
  if (!prev)
    rb->Flush();
}

SinkReplyBuilder2::ReplyScope::~ReplyScope() {
  rb->scoped_ = prev;
  if (!prev)
    rb->FinishScope();
}

void SinkReplyBuilder2::SendError(ErrorReply error) {
  if (error.status)
    return SendError(*error.status);
  SendError(error.ToSv(), error.kind);
}

void SinkReplyBuilder2::SendError(OpStatus status) {
  if (status == OpStatus::OK)
    return SendOk();
  SendError(StatusToMsg(status));
}

void SinkReplyBuilder2::CloseConnection() {
  if (!ec_)
    ec_ = std::make_error_code(std::errc::connection_aborted);
}

char* SinkReplyBuilder2::ReservePiece(size_t size) {
  if (buffer_.AppendLen() <= size)
    Flush();

  char* dest = reinterpret_cast<char*>(buffer_.AppendBuffer().data());

  // Start new vec for piece if last one dones't point at buffer tail
  if (vecs_.empty() || ((char*)vecs_.back().iov_base) + vecs_.back().iov_len != dest)
    NextVec({dest, 0});

  return dest;
}

void SinkReplyBuilder2::CommitPiece(size_t size) {
  buffer_.CommitWrite(size);
  vecs_.back().iov_len += size;
  total_size_ += size;
}

void SinkReplyBuilder2::WritePiece(std::string_view str) {
  char* dest = ReservePiece(str.size());
  memcpy(dest, str.data(), str.size());
  CommitPiece(str.size());
}

void SinkReplyBuilder2::WriteRef(std::string_view str) {
  NextVec(str);
  total_size_ += str.size();
}

void SinkReplyBuilder2::Flush() {
  auto& reply_stats = tl_facade_stats->reply_stats;

  send_active_ = true;
  uint64_t before_ns = util::fb2::ProactorBase::GetMonotonicTimeNs();
  reply_stats.io_write_cnt++;
  reply_stats.io_write_bytes += total_size_;

  if (auto ec = sink_->Write(vecs_.data(), vecs_.size()); ec)
    ec_ = ec;

  uint64_t after_ns = util::fb2::ProactorBase::GetMonotonicTimeNs();
  reply_stats.send_stats.count++;
  reply_stats.send_stats.total_duration += (after_ns - before_ns) / 1'000;
  send_active_ = false;

  if (buffer_.InputLen() * 2 > buffer_.Capacity())  // If needed, grow backing buffer
    buffer_.Reserve(std::min(kMaxBufferSize, buffer_.Capacity() * 2));

  total_size_ = 0;
  buffer_.Clear();
  vecs_.clear();
  guaranteed_pieces_ = 0;
}

void SinkReplyBuilder2::FinishScope() {
  if (!batched_ || total_size_ * 2 >= kMaxBufferSize)
    return Flush();

  // Check if we have enough space to copy all refs to buffer
  size_t ref_bytes = total_size_ - buffer_.InputLen();
  if (ref_bytes > buffer_.AppendLen())
    return Flush();

  // Copy all extenral references to buffer to safely keep batching
  for (size_t i = guaranteed_pieces_; i < vecs_.size(); i++) {
    auto ib = buffer_.InputBuffer();
    if (vecs_[i].iov_base >= ib.data() && vecs_[i].iov_base <= ib.data() + ib.size())
      continue;  // this is a piece

    DCHECK_LE(buffer_.AppendLen(), vecs_[i].iov_len);
    void* dest = buffer_.AppendBuffer().data();
    memcpy(dest, vecs_[i].iov_base, vecs_[i].iov_len);
    buffer_.CommitWrite(vecs_[i].iov_len);
    vecs_[i].iov_base = dest;
  }
  guaranteed_pieces_ = vecs_.size();  // all vecs are pieces
}

void SinkReplyBuilder2::NextVec(std::string_view str) {
  if (vecs_.size() >= IOV_MAX - 2)
    Flush();
  vecs_.push_back(iovec{const_cast<char*>(str.data()), str.size()});
}

MCReplyBuilder::MCReplyBuilder(::io::Sink* sink) : SinkReplyBuilder(sink), noreply_(false) {
}

void MCReplyBuilder::SendSimpleString(std::string_view str) {
  if (noreply_)
    return;

  iovec v[2] = {IoVec(str), IoVec(kCRLF)};

  Send(v, ABSL_ARRAYSIZE(v));
}

void MCReplyBuilder::SendStored() {
  SendSimpleString("STORED");
}

void MCReplyBuilder::SendLong(long val) {
  char buf[32];
  char* next = absl::numbers_internal::FastIntToBuffer(val, buf);
  SendSimpleString(string_view(buf, next - buf));
}

void MCReplyBuilder::SendMGetResponse(MGetResponse resp) {
  string header;
  for (unsigned i = 0; i < resp.resp_arr.size(); ++i) {
    if (resp.resp_arr[i]) {
      const auto& src = *resp.resp_arr[i];
      absl::StrAppend(&header, "VALUE ", src.key, " ", src.mc_flag, " ", src.value.size());
      if (src.mc_ver) {
        absl::StrAppend(&header, " ", src.mc_ver);
      }

      absl::StrAppend(&header, "\r\n");
      iovec v[] = {IoVec(header), IoVec(src.value), IoVec(kCRLF)};
      Send(v, ABSL_ARRAYSIZE(v));
      header.clear();
    }
  }
  SendSimpleString("END");
}

void MCReplyBuilder::SendError(string_view str, std::string_view type) {
  SendSimpleString(absl::StrCat("SERVER_ERROR ", str));
}

void MCReplyBuilder::SendProtocolError(std::string_view str) {
  SendSimpleString(absl::StrCat("CLIENT_ERROR ", str));
}

bool MCReplyBuilder::NoReply() const {
  return noreply_;
}

void MCReplyBuilder::SendClientError(string_view str) {
  iovec v[] = {IoVec("CLIENT_ERROR "), IoVec(str), IoVec(kCRLF)};
  Send(v, ABSL_ARRAYSIZE(v));
}

void MCReplyBuilder::SendSetSkipped() {
  SendSimpleString("NOT_STORED");
}

void MCReplyBuilder::SendNotFound() {
  SendSimpleString("NOT_FOUND");
}

char* RedisReplyBuilder::FormatDouble(double val, char* dest, unsigned dest_len) {
  StringBuilder sb(dest, dest_len);
  CHECK(dfly_conv.ToShortest(val, &sb));
  return sb.Finalize();
}

RedisReplyBuilder::RedisReplyBuilder(::io::Sink* sink) : SinkReplyBuilder(sink) {
}

void RedisReplyBuilder::SetResp3(bool is_resp3) {
  is_resp3_ = is_resp3;
}

void RedisReplyBuilder::SendError(string_view str, string_view err_type) {
  VLOG(1) << "Error: " << str;

  if (err_type.empty()) {
    err_type = str;
    if (err_type == kSyntaxErr)
      err_type = kSyntaxErrType;
  }

  tl_facade_stats->reply_stats.err_count[err_type]++;

  if (str[0] == '-') {
    iovec v[] = {IoVec(str), IoVec(kCRLF)};
    Send(v, ABSL_ARRAYSIZE(v));
    return;
  }

  iovec v[] = {IoVec(kErrPref), IoVec(str), IoVec(kCRLF)};
  Send(v, ABSL_ARRAYSIZE(v));
}

void RedisReplyBuilder::SendProtocolError(std::string_view str) {
  SendError(absl::StrCat("-ERR Protocol error: ", str), "protocol_error");
}

void RedisReplyBuilder::SendSimpleString(std::string_view str) {
  iovec v[3] = {IoVec(kSimplePref), IoVec(str), IoVec(kCRLF)};

  Send(v, ABSL_ARRAYSIZE(v));
}

void RedisReplyBuilder::SendStored() {
  SendSimpleString("OK");
}

void RedisReplyBuilder::SendSetSkipped() {
  SendNull();
}

void RedisReplyBuilder::SendNull() {
  iovec v[] = {IoVec(NullString(is_resp3_))};

  Send(v, ABSL_ARRAYSIZE(v));
}

void RedisReplyBuilder::SendBulkString(std::string_view str) {
  char tmp[absl::numbers_internal::kFastToBufferSize + 3];
  tmp[0] = '$';  // Format length
  char* next = absl::numbers_internal::FastIntToBuffer(uint32_t(str.size()), tmp + 1);
  *next++ = '\r';
  *next++ = '\n';

  std::string_view lenpref{tmp, size_t(next - tmp)};

  // 3 parts: length, string and CRLF.
  iovec v[3] = {IoVec(lenpref), IoVec(str), IoVec(kCRLF)};

  return Send(v, ABSL_ARRAYSIZE(v));
}

void RedisReplyBuilder::SendVerbatimString(std::string_view str, VerbatimFormat format) {
  if (!is_resp3_)
    return SendBulkString(str);

  char tmp[absl::numbers_internal::kFastToBufferSize + 7];
  tmp[0] = '=';
  // + 4 because format is three byte, and need to be followed by a ":"
  char* next = absl::numbers_internal::FastIntToBuffer(uint32_t(str.size() + 4), tmp + 1);
  *next++ = '\r';
  *next++ = '\n';

  DCHECK(format <= VerbatimFormat::MARKDOWN);
  if (format == VerbatimFormat::TXT)
    strcpy(next, "txt:");
  else if (format == VerbatimFormat::MARKDOWN)
    strcpy(next, "mkd:");
  next += 4;
  std::string_view lenpref{tmp, size_t(next - tmp)};
  iovec v[3] = {IoVec(lenpref), IoVec(str), IoVec(kCRLF)};
  return Send(v, ABSL_ARRAYSIZE(v));
}

void RedisReplyBuilder::SendLong(long num) {
  string str = absl::StrCat(":", num, kCRLF);
  SendRaw(str);
}

void RedisReplyBuilder::SendScoredArray(const std::vector<std::pair<std::string, double>>& arr,
                                        bool with_scores) {
  ReplyAggregator agg(this);
  if (!with_scores) {
    auto cb = [&](size_t indx) -> string_view { return arr[indx].first; };

    SendStringArrInternal(arr.size(), std::move(cb), CollectionType::ARRAY);
    return;
  }

  char buf[DoubleToStringConverter::kBase10MaximalLength * 3];  // to be on the safe side.

  if (!is_resp3_) {  // RESP2 formats withscores as a flat array.
    auto cb = [&](size_t indx) -> string_view {
      if (indx % 2 == 0)
        return arr[indx / 2].first;

      // NOTE: we reuse the same buffer, assuming that SendStringArrInternal does not reference
      // previous string_views. The assumption holds for small strings like
      // doubles because SendStringArrInternal employs small string optimization.
      // It's a bit hacky but saves allocations.
      return FormatDouble(arr[indx / 2].second, buf, sizeof(buf));
    };

    SendStringArrInternal(arr.size() * 2, std::move(cb), CollectionType::ARRAY);
    return;
  }

  // Resp3 formats withscores as array of (key, score) pairs.
  // TODO: to implement efficient serializing by extending SendStringArrInternal to support
  // 2-level arrays.
  StartArray(arr.size());
  for (const auto& p : arr) {
    StartArray(2);
    SendBulkString(p.first);
    SendDouble(p.second);
  }
}

void RedisReplyBuilder::SendDouble(double val) {
  char buf[64];

  char* start = FormatDouble(val, buf, sizeof(buf));

  if (!is_resp3_) {
    SendBulkString(start);
  } else {
    // RESP3
    SendRaw(absl::StrCat(",", start, kCRLF));
  }
}

void RedisReplyBuilder::SendMGetResponse(MGetResponse resp) {
  DCHECK(!resp.resp_arr.empty());

  size_t size = resp.resp_arr.size();

  size_t vec_len = std::min<size_t>(32, size);

  constexpr size_t kBatchLen = 32 * 2 + 2;  // (blob_size, blob) * 32 + 2 spares
  iovec vec_batch[kBatchLen];

  // for all the meta data to fill the vec batch. 10 digits for the blob size and 6 for
  // $, \r, \n, \r, \n
  absl::FixedArray<char, 64> meta((vec_len + 2) * 16);  // 2 for header and next item meta data.

  char* next = meta.data();
  char* cur_meta = next;
  *next++ = '*';
  next = absl::numbers_internal::FastIntToBuffer(size, next);
  *next++ = '\r';
  *next++ = '\n';

  unsigned vec_indx = 0;
  const char* nullstr = NullString(is_resp3_);
  size_t nulllen = strlen(nullstr);
  auto get_pending_metabuf = [&] { return string_view{cur_meta, size_t(next - cur_meta)}; };

  for (unsigned i = 0; i < size; ++i) {
    DCHECK_GE(meta.end() - next, 16);  // We have at least 16 bytes for the meta data.
    if (resp.resp_arr[i]) {
      string_view blob = resp.resp_arr[i]->value;

      *next++ = '$';
      next = absl::numbers_internal::FastIntToBuffer(blob.size(), next);
      *next++ = '\r';
      *next++ = '\n';
      DCHECK_GT(next - cur_meta, 0);

      vec_batch[vec_indx++] = IoVec(get_pending_metabuf());
      vec_batch[vec_indx++] = IoVec(blob);
      cur_meta = next;  // we combine the CRLF with the next item meta data.
      *next++ = '\r';
      *next++ = '\n';
    } else {
      memcpy(next, nullstr, nulllen);
      next += nulllen;
    }

    if (vec_indx >= (kBatchLen - 2) || (meta.end() - next < 16)) {
      // we have space for at least one iovec because in the worst case we reached (kBatchLen - 3)
      // and then filled 2 vectors in the previous iteration.
      DCHECK_LE(vec_indx, kBatchLen - 1);

      // if we do not have enough space in the meta buffer, we add the meta data to the
      // vector batch and reset it.
      if (meta.end() - next < 16) {
        vec_batch[vec_indx++] = IoVec(get_pending_metabuf());
        next = meta.data();
        cur_meta = next;
      }

      Send(vec_batch, vec_indx);
      if (ec_)
        return;

      vec_indx = 0;
      size_t meta_len = next - cur_meta;
      memcpy(meta.data(), cur_meta, meta_len);
      cur_meta = meta.data();
      next = cur_meta + meta_len;
    }
  }

  if (next - cur_meta > 0) {
    vec_batch[vec_indx++] = IoVec(get_pending_metabuf());
  }
  if (vec_indx > 0)
    Send(vec_batch, vec_indx);
}

void RedisReplyBuilder::SendSimpleStrArr(StrSpan arr) {
  string res = absl::StrCat("*", arr.Size(), kCRLF);
  for (string_view str : arr)
    StrAppend(&res, "+", str, kCRLF);

  SendRaw(res);
}

void RedisReplyBuilder::SendNullArray() {
  SendRaw("*-1\r\n");
}

void RedisReplyBuilder::SendEmptyArray() {
  StartArray(0);
}

void RedisReplyBuilder::SendStringArr(StrSpan arr, CollectionType type) {
  if (type == ARRAY && arr.Size() == 0) {
    SendRaw("*0\r\n");
    return;
  }

  auto cb = [&](size_t i) {
    return visit([i](auto& span) { return facade::ToSV(span[i]); }, arr.span);
  };
  SendStringArrInternal(arr.Size(), std::move(cb), type);
}

void RedisReplyBuilder::StartArray(unsigned len) {
  StartCollection(len, ARRAY);
}

constexpr static string_view START_SYMBOLS[] = {"*", "~", "%", ">"};
static_assert(START_SYMBOLS[RedisReplyBuilder::MAP] == "%" &&
              START_SYMBOLS[RedisReplyBuilder::SET] == "~");

void RedisReplyBuilder::StartCollection(unsigned len, CollectionType type) {
  if (!is_resp3_) {  // Flatten for Resp2
    if (type == MAP)
      len *= 2;
    type = ARRAY;
  }

  DVLOG(2) << "StartCollection(" << len << ", " << type << ")";

  // We do not want to send multiple packets for small responses because these
  // trigger TCP-related artifacts (e.g. Nagle's algorithm) that slow down the delivery of the whole
  // response.
  bool prev = should_aggregate_;
  should_aggregate_ |= (len > 0);
  SendRaw(absl::StrCat(START_SYMBOLS[type], len, kCRLF));
  should_aggregate_ = prev;
}

// This implementation a bit complicated because it uses vectorized
// send to send an array. The problem with that is the OS limits vector length to UIO_MAXIOV.
// Therefore, to make it robust we send the array in batches.
// We limit the vector length, and when it fills up we flush it to the socket and continue
// iterating.
void RedisReplyBuilder::SendStringArrInternal(
    size_t size, absl::FunctionRef<std::string_view(unsigned)> producer, CollectionType type) {
  size_t header_len = size;
  string_view type_char = "*";
  if (is_resp3_) {
    type_char = START_SYMBOLS[type];
    if (type == MAP)
      header_len /= 2;  // Each key value pair counts as one.
  }

  if (header_len == 0) {
    SendRaw(absl::StrCat(type_char, "0\r\n"));
    return;
  }

  // We limit iovec capacity, vectorized length is limited upto UIO_MAXIOV (Send returns EMSGSIZE).
  size_t vec_cap = std::min<size_t>(UIO_MAXIOV, size * 2);
  absl::FixedArray<iovec, 16> vec(vec_cap);
  absl::FixedArray<char, 128> meta(std::max<size_t>(vec_cap * 64, 128u));

  char* start = meta.data();
  char* next = start;

  // at most 35 chars.
  auto serialize_len = [&](char prefix, size_t len) {
    *next++ = prefix;
    next = absl::numbers_internal::FastIntToBuffer(len, next);  // at most 32 chars
    *next++ = '\r';
    *next++ = '\n';
  };

  serialize_len(type_char[0], header_len);
  unsigned vec_indx = 0;
  string_view src;

#define FLUSH_IOVEC()           \
  do {                          \
    Send(vec.data(), vec_indx); \
    if (ec_)                    \
      return;                   \
    vec_indx = 0;               \
    next = meta.data();         \
  } while (false)

  for (unsigned i = 0; i < size; ++i) {
    DCHECK_LT(vec_indx, vec_cap);

    src = producer(i);
    serialize_len('$', src.size());

    // copy data either by referencing via an iovec or copying inline into meta buf.
    constexpr size_t kSSOLen = 32;
    if (src.size() > kSSOLen) {
      // reference metadata blob before referencing another vector.
      DCHECK_GT(next - start, 0);
      vec[vec_indx++] = IoVec(string_view{start, size_t(next - start)});
      if (vec_indx >= vec_cap) {
        FLUSH_IOVEC();
      }

      DCHECK_LT(vec_indx, vec.size());
      vec[vec_indx++] = IoVec(src);
      if (vec_indx >= vec_cap) {
        FLUSH_IOVEC();
      }
      start = next;
    } else if (src.size() > 0) {
      // NOTE!: this is not just optimization. producer may returns a string_piece that will
      // be overriden for the next call, so we must do this for correctness.
      memcpy(next, src.data(), src.size());
      next += src.size();
    }

    // how much buffer we need to perform the next iteration.
    constexpr ptrdiff_t kMargin = kSSOLen + 3 /* $\r\n */ + 2 /*length*/ + 2 /* \r\n */;

    // Keep at least kMargin bytes for a small string as well as its length.
    if (kMargin >= meta.end() - next) {
      // Flush the iovec array.
      vec[vec_indx++] = IoVec(string_view{start, size_t(next - start)});
      FLUSH_IOVEC();
      start = next;
    }
    *next++ = '\r';
    *next++ = '\n';
  }

  vec[vec_indx].iov_base = start;
  vec[vec_indx].iov_len = next - start;
  Send(vec.data(), vec_indx + 1);
}

void ReqSerializer::SendCommand(std::string_view str) {
  VLOG(2) << "SendCommand: " << str;

  iovec v[] = {IoVec(str), IoVec(kCRLF)};
  ec_ = sink_->Write(v, ABSL_ARRAYSIZE(v));
}

void RedisReplyBuilder2Base::SendNull() {
  ReplyScope scope(this);
  resp3_ ? Write(kNullStringR3) : Write(kNullStringR2);
}

void RedisReplyBuilder2Base::SendSimpleString(std::string_view str) {
  ReplyScope scope(this);
  Write(kSimplePref, str, kCRLF);
}

void RedisReplyBuilder2Base::SendBulkString(std::string_view str) {
  ReplyScope scope(this);
  WriteIntWithPrefix('$', str.size());
  Write(kCRLF, str, kCRLF);
}

void RedisReplyBuilder2Base::SendLong(long val) {
  ReplyScope scope(this);
  WriteIntWithPrefix(':', val);
  Write(kCRLF);
}

void RedisReplyBuilder2Base::SendDouble(double val) {
  char buf[DoubleToStringConverter::kBase10MaximalLength + 1];
  static_assert(ABSL_ARRAYSIZE(buf) < kMaxInlineSize, "Write temporary string from buf inline");
  string_view val_str = FormatDouble(val, buf, ABSL_ARRAYSIZE(buf));

  if (!resp3_)
    return SendBulkString(val_str);

  ReplyScope scope(this);
  Write(",", val_str, kCRLF);
}

void RedisReplyBuilder2Base::SendNullArray() {
  ReplyScope scope(this);
  Write("*-1", kCRLF);
}

constexpr static const char START_SYMBOLS2[4] = {'*', '~', '%', '>'};
static_assert(START_SYMBOLS2[RedisReplyBuilder2Base::MAP] == '%' &&
              START_SYMBOLS2[RedisReplyBuilder2Base::SET] == '~');

void RedisReplyBuilder2Base::StartCollection(unsigned len, CollectionType ct) {
  if (!IsResp3()) {  // RESP2 supports only arrays
    if (ct == MAP)
      len *= 2;
    ct = ARRAY;
  }
  ReplyScope scope(this);
  WriteIntWithPrefix(START_SYMBOLS2[ct], len);
  WritePiece(kCRLF);
}

void RedisReplyBuilder2Base::WriteIntWithPrefix(char prefix, int64_t val) {
  char* dest = ReservePiece(absl::numbers_internal::kFastToBufferSize + 1);
  char* next = dest;
  *next++ = prefix;
  next = absl::numbers_internal::FastIntToBuffer(val, next);
  CommitPiece(next - dest);
}

void RedisReplyBuilder2Base::SendError(std::string_view str, std::string_view type) {
  ReplyScope scope(this);

  if (type.empty()) {
    type = str;
    if (type == kSyntaxErr)
      type = kSyntaxErrType;
  }
  tl_facade_stats->reply_stats.err_count[type]++;

  if (str[0] != '-')
    WritePiece(kErrPref);
  WritePiece(str);
  WritePiece(kCRLF);
}

void RedisReplyBuilder2Base::SendProtocolError(std::string_view str) {
  SendError(absl::StrCat("-ERR Protocol error: ", str), "protocol_error");
}

char* RedisReplyBuilder2Base::FormatDouble(double d, char* dest, unsigned len) {
  StringBuilder sb(dest, len);
  CHECK(dfly_conv.ToShortest(d, &sb));
  return sb.Finalize();
}

}  // namespace facade
