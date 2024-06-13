// Copyright 2022, DragonflyDB authors.  All rights reserved.
// See LICENSE for licensing terms.
//

#include "server/journal/streamer.h"

#include <absl/functional/bind_front.h>

#include "base/flags.h"
#include "base/logging.h"
#include "server/cluster/cluster_defs.h"

using namespace facade;

ABSL_FLAG(uint32_t, replication_stream_timeout, 500,
          "Time in milliseconds to wait for the replication output buffer go below "
          "the throttle limit.");
ABSL_FLAG(uint32_t, replication_stream_output_limit, 64_KB,
          "Time to wait for the replication output buffer go below the throttle limit");

namespace dfly {
using namespace util;
using namespace journal;

namespace {

iovec IoVec(io::Bytes src) {
  return iovec{const_cast<uint8_t*>(src.data()), src.size()};
}

constexpr size_t kFlushThreshold = 2_KB;
uint32_t replication_stream_output_limit_cached = 64_KB;

}  // namespace

JournalStreamer::JournalStreamer(journal::Journal* journal, Context* cntx)
    : journal_(journal), cntx_(cntx) {
  // cache the flag to avoid accessing it later.
  replication_stream_output_limit_cached = absl::GetFlag(FLAGS_replication_stream_output_limit);
}

JournalStreamer::~JournalStreamer() {
  DCHECK_EQ(in_flight_bytes_, 0u);
  VLOG(1) << "~JournalStreamer";
}

void JournalStreamer::Start(io::AsyncSink* dest, bool send_lsn) {
  CHECK(dest_ == nullptr && dest != nullptr);
  dest_ = dest;
  journal_cb_id_ =
      journal_->RegisterOnChange([this, send_lsn](const JournalItem& item, bool allow_await) {
        if (allow_await) {
          ThrottleIfNeeded();
          // No record to write, just await if data was written so consumer will read the data.
          if (item.opcode == Op::NOOP)
            return;
        }

        if (!ShouldWrite(item)) {
          return;
        }

        Write(item.data);
        time_t now = time(nullptr);

        // TODO: to chain it to the previous Write call.
        if (send_lsn && now - last_lsn_time_ > 3) {
          last_lsn_time_ = now;
          io::StringSink sink;
          JournalWriter writer(&sink);
          writer.Write(Entry{journal::Op::LSN, item.lsn});
          Write(sink.str());
        }
      });
}

void JournalStreamer::Cancel() {
  VLOG(1) << "JournalStreamer::Cancel";
  waker_.notifyAll();
  journal_->UnregisterOnChange(journal_cb_id_);
  WaitForInflightToComplete();
}

size_t JournalStreamer::GetTotalBufferCapacities() const {
  return in_flight_bytes_ + pending_buf_.capacity();
}

void JournalStreamer::Write(std::string_view str) {
  DCHECK(!str.empty());
  DVLOG(2) << "Writing " << str.size() << " bytes";

  // If we do not have any in flight requests we send the string right a way.
  // We can not aggregate it since we do not know when the next update will follow.
  size_t total_pending = pending_buf_.size() + str.size();
  if (in_flight_bytes_ == 0 || total_pending > kFlushThreshold) {
    // because of potential SOO with strings we allocate explicitly on heap
    uint8_t* buf(new uint8_t[str.size()]);

    // TODO: it is possible to remove these redundant copies if we adjust high level
    // interfaces to pass reference-counted buffers.
    memcpy(buf, str.data(), str.size());
    in_flight_bytes_ += total_pending;

    iovec v[2];
    unsigned next_buf_id = 0;

    if (!pending_buf_.empty()) {
      v[0] = IoVec(pending_buf_);
      ++next_buf_id;
    }
    v[next_buf_id++] = IoVec(io::Bytes(buf, str.size()));

    dest_->AsyncWrite(
        v, next_buf_id,
        [buf0 = std::move(pending_buf_), buf, this, len = total_pending](std::error_code ec) {
          delete[] buf;
          OnCompletion(ec, len);
        });

    return;
  }

  DCHECK_GT(in_flight_bytes_, 0u);
  DCHECK_LE(pending_buf_.size() + str.size(), kFlushThreshold);

  // Aggregate
  size_t tail = pending_buf_.size();
  pending_buf_.resize(pending_buf_.size() + str.size());
  memcpy(pending_buf_.data() + tail, str.data(), str.size());
}

void JournalStreamer::OnCompletion(std::error_code ec, size_t len) {
  DCHECK_GE(in_flight_bytes_, len);

  DVLOG(2) << "Completing from " << in_flight_bytes_ << " to " << in_flight_bytes_ - len;
  in_flight_bytes_ -= len;
  if (ec && !IsStopped()) {
    cntx_->ReportError(ec);
  } else if (in_flight_bytes_ == 0 && !pending_buf_.empty() && !IsStopped()) {
    // If everything was sent but we have a pending buf, flush it.
    io::Bytes src(pending_buf_);
    in_flight_bytes_ += src.size();
    dest_->AsyncWrite(src, [buf = std::move(pending_buf_), this](std::error_code ec) {
      OnCompletion(ec, buf.size());
    });
  }

  // notify ThrottleIfNeeded or WaitForInflightToComplete that waits
  // for all the completions to finish.
  // ThrottleIfNeeded can run from multiple fibers in the journal thread.
  // For example, from Heartbeat calling TriggerJournalWriteToSink to flush potential
  // expiration deletions and there are other cases as well.
  waker_.notifyAll();
}

void JournalStreamer::ThrottleIfNeeded() {
  if (IsStopped() || !IsStalled())
    return;

  auto next = chrono::steady_clock::now() +
              chrono::milliseconds(absl::GetFlag(FLAGS_replication_stream_timeout));
  auto inflight_start = in_flight_bytes_;

  std::cv_status status =
      waker_.await_until([this]() { return !IsStalled() || IsStopped(); }, next);
  if (status == std::cv_status::timeout) {
    LOG(WARNING) << "Stream timed out, inflight bytes start: " << inflight_start
                 << ", end: " << in_flight_bytes_;
    cntx_->ReportError(make_error_code(errc::stream_timeout));
  }
}

void JournalStreamer::WaitForInflightToComplete() {
  while (in_flight_bytes_) {
    auto next = chrono::steady_clock::now() + 1s;
    std::cv_status status =
        waker_.await_until([this] { return this->in_flight_bytes_ == 0; }, next);
    LOG_IF(WARNING, status == std::cv_status::timeout)
        << "Waiting for inflight bytes " << in_flight_bytes_;
  }
}

bool JournalStreamer::IsStalled() const {
  return in_flight_bytes_ >= replication_stream_output_limit_cached;
}

RestoreStreamer::RestoreStreamer(DbSlice* slice, cluster::SlotSet slots, journal::Journal* journal,
                                 Context* cntx)
    : JournalStreamer(journal, cntx), db_slice_(slice), my_slots_(std::move(slots)) {
  DCHECK(slice != nullptr);
}

void RestoreStreamer::Start(io::AsyncSink* dest, bool send_lsn) {
  VLOG(1) << "RestoreStreamer start";
  auto db_cb = absl::bind_front(&RestoreStreamer::OnDbChange, this);
  snapshot_version_ = db_slice_->RegisterOnChange(std::move(db_cb));

  JournalStreamer::Start(dest, send_lsn);

  PrimeTable::Cursor cursor;
  uint64_t last_yield = 0;
  PrimeTable* pt = &db_slice_->databases()[0]->prime;

  do {
    if (fiber_cancelled_)
      return;

    bool written = false;
    cursor = pt->Traverse(cursor, [&](PrimeTable::bucket_iterator it) {
      db_slice_->FlushChangeToEarlierCallbacks(0 /*db_id always 0 for cluster*/,
                                               DbSlice::Iterator::FromPrime(it), snapshot_version_);
      if (WriteBucket(it)) {
        written = true;
      }
    });
    if (written) {
      ThrottleIfNeeded();
    }

    if (++last_yield >= 100) {
      ThisFiber::Yield();
      last_yield = 0;
    }
  } while (cursor);
}

void RestoreStreamer::SendFinalize() {
  VLOG(1) << "RestoreStreamer FIN opcode for : " << db_slice_->shard_id();
  journal::Entry entry(journal::Op::FIN, 0 /*db_id*/, 0 /*slot_id*/);

  io::StringSink sink;
  JournalWriter writer{&sink};
  writer.Write(entry);
  Write(sink.str());

  // TODO: is the intent here to flush everything?
  //
  ThrottleIfNeeded();
}

RestoreStreamer::~RestoreStreamer() {
}

void RestoreStreamer::Cancel() {
  auto sver = snapshot_version_;
  snapshot_version_ = 0;  // to prevent double cancel in another fiber
  if (sver != 0) {
    fiber_cancelled_ = true;
    db_slice_->UnregisterOnChange(sver);
    JournalStreamer::Cancel();
  }
}

bool RestoreStreamer::ShouldWrite(const journal::JournalItem& item) const {
  if (!item.slot.has_value()) {
    return false;
  }

  return ShouldWrite(*item.slot);
}

bool RestoreStreamer::ShouldWrite(std::string_view key) const {
  return ShouldWrite(cluster::KeySlot(key));
}

bool RestoreStreamer::ShouldWrite(cluster::SlotId slot_id) const {
  return my_slots_.Contains(slot_id);
}

bool RestoreStreamer::WriteBucket(PrimeTable::bucket_iterator it) {
  // Can't switch fibers because that could invalidate iterator or cause bucket splits which may
  // move keys between buckets.
  FiberAtomicGuard fg;

  bool written = false;

  if (it.GetVersion() < snapshot_version_) {
    it.SetVersion(snapshot_version_);
    string key_buffer;  // we can reuse it
    for (; !it.is_done(); ++it) {
      const auto& pv = it->second;
      string_view key = it->first.GetSlice(&key_buffer);
      if (ShouldWrite(key)) {
        written = true;

        uint64_t expire = 0;
        if (pv.HasExpire()) {
          auto eit = db_slice_->databases()[0]->expire.Find(it->first);
          expire = db_slice_->ExpireTime(eit);
        }

        WriteEntry(key, pv, expire);
      }
    }
  }

  return written;
}

void RestoreStreamer::OnDbChange(DbIndex db_index, const DbSlice::ChangeReq& req) {
  DCHECK_EQ(db_index, 0) << "Restore migration only allowed in cluster mode in db0";

  FiberAtomicGuard fg;
  PrimeTable* table = db_slice_->GetTables(0).first;

  if (const PrimeTable::bucket_iterator* bit = req.update()) {
    WriteBucket(*bit);
  } else {
    string_view key = get<string_view>(req.change);
    table->CVCUponInsert(snapshot_version_, key, [this](PrimeTable::bucket_iterator it) {
      DCHECK_LT(it.GetVersion(), snapshot_version_);
      WriteBucket(it);
    });
  }
}

void RestoreStreamer::WriteEntry(string_view key, const PrimeValue& pv, uint64_t expire_ms) {
  absl::InlinedVector<string_view, 4> args;
  args.push_back(key);

  string expire_str = absl::StrCat(expire_ms);
  args.push_back(expire_str);

  io::StringSink value_dump_sink;
  SerializerBase::DumpObject(pv, &value_dump_sink);
  args.push_back(value_dump_sink.str());

  args.push_back("ABSTTL");  // Means expire string is since epoch

  WriteCommand(journal::Entry::Payload("RESTORE", ArgSlice(args)));
}

void RestoreStreamer::WriteCommand(journal::Entry::Payload cmd_payload) {
  journal::Entry entry(0,                     // txid
                       journal::Op::COMMAND,  // single command
                       0,                     // db index
                       1,                     // shard count
                       0,                     // slot-id, but it is ignored at this level
                       cmd_payload);

  // TODO: From WriteEntry to till Write we tripple copy the PrimeValue. It's ver in-efficient and
  // will burn CPU for large values.
  io::StringSink sink;
  JournalWriter writer{&sink};
  writer.Write(entry);
  Write(sink.str());
}

}  // namespace dfly
