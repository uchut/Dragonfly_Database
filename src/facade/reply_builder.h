// Copyright 2022, DragonflyDB authors.  All rights reserved.
// See LICENSE for licensing terms.
//
#pragma once

#include <absl/container/flat_hash_map.h>

#include <optional>
#include <string_view>

#include "facade/facade_types.h"
#include "facade/op_status.h"
#include "io/io.h"

namespace facade {

// Reply mode allows filtering replies.
enum class ReplyMode {
  NONE,      // No replies are recorded
  ONLY_ERR,  // Only errors are recorded
  FULL       // All replies are recorded
};

class SinkReplyBuilder {
 public:
  struct MGetStorage {
    MGetStorage* next = nullptr;
    char data[1];
  };

  struct GetResp {
    std::string key;  // TODO: to use backing storage to optimize this as well.
    std::string_view value;

    uint64_t mc_ver = 0;  // 0 means we do not output it (i.e has not been requested).
    uint32_t mc_flag = 0;

    GetResp() = default;
    GetResp(std::string_view val) : value(val) {
    }
  };

  struct MGetResponse {
    MGetStorage* storage_list = nullptr;  // backing storage of resp_arr values.
    std::vector<std::optional<GetResp>> resp_arr;

    MGetResponse() = default;

    MGetResponse(size_t size) : resp_arr(size) {
    }

    ~MGetResponse();

    MGetResponse(MGetResponse&& other) noexcept
        : storage_list(other.storage_list), resp_arr(std::move(other.resp_arr)) {
      other.storage_list = nullptr;
    }

    MGetResponse& operator=(MGetResponse&& other) noexcept {
      resp_arr = std::move(other.resp_arr);
      storage_list = other.storage_list;
      other.storage_list = nullptr;
      return *this;
    }
  };

  SinkReplyBuilder(const SinkReplyBuilder&) = delete;
  void operator=(const SinkReplyBuilder&) = delete;

  SinkReplyBuilder(::io::Sink* sink);

  virtual ~SinkReplyBuilder() {
  }

  static MGetStorage* AllocMGetStorage(size_t size) {
    static_assert(alignof(MGetStorage) == 8);  // if this breaks we should fix the code below.
    char* buf = new char[size + sizeof(MGetStorage)];
    return new (buf) MGetStorage();
  }

  virtual void SendError(std::string_view str, std::string_view type = {}) = 0;  // MC and Redis
  virtual void SendError(ErrorReply error);
  virtual void SendError(OpStatus status);

  virtual void SendStored() = 0;  // Reply for set commands.
  virtual void SendSetSkipped() = 0;

  virtual void SendMGetResponse(MGetResponse resp) = 0;

  virtual void SendLong(long val) = 0;
  virtual void SendSimpleString(std::string_view str) = 0;

  void SendOk() {
    SendSimpleString("OK");
  }

  virtual void SendProtocolError(std::string_view str) = 0;

  // In order to reduce interrupt rate we allow coalescing responses together using
  // Batch mode. It is controlled by Connection state machine because it makes sense only
  // when pipelined requests are arriving.
  void SetBatchMode(bool batch);

  void FlushBatch();

  // Used for QUIT - > should move to conn_context?
  void CloseConnection();

  std::error_code GetError() const {
    return ec_;
  }

  size_t io_write_cnt() const {
    return io_write_cnt_;
  }

  size_t io_write_bytes() const {
    return io_write_bytes_;
  }

  void reset_io_stats() {
    io_write_cnt_ = 0;
    io_write_bytes_ = 0;
    err_count_.clear();
  }

  const absl::flat_hash_map<std::string, uint64_t>& err_count() const {
    return err_count_;
  }

  struct ReplyAggregator {
    explicit ReplyAggregator(SinkReplyBuilder* builder) : builder_(builder) {
      // If the builder is already aggregating then don't aggregate again as
      // this will cause redundant sink writes (such as in a MULTI/EXEC).
      if (builder->should_aggregate_) {
        return;
      }
      builder_->StartAggregate();
      is_nested_ = false;
    }

    ~ReplyAggregator() {
      if (!is_nested_) {
        builder_->StopAggregate();
      }
    }

   private:
    SinkReplyBuilder* builder_;
    bool is_nested_ = true;
  };

  void ExpectReply();
  bool HasReplied() const;

  virtual size_t UsedMemory() const;

  enum SendStatsType {
    kRegular,   // Send() operations that are written to sockets
    kBatch,     // Send() operations that are internally batched to a buffer
    kNumTypes,  // Number of types, do not use directly
  };

  struct SendStats {
    int64_t count = 0;
    int64_t total_duration = 0;

    SendStats& operator+=(const SendStats& other) {
      count += other.count;
      total_duration += other.total_duration;
      return *this;
    }
  };

  using StatsType = std::array<SendStats, SendStatsType::kNumTypes>;

  static StatsType GetThreadLocalStats();

 protected:
  void SendRaw(std::string_view str);  // Sends raw without any formatting.
  void SendRawVec(absl::Span<const std::string_view> msg_vec);

  void Send(const iovec* v, uint32_t len);

  void StartAggregate();
  void StopAggregate();

  std::string batch_;
  ::io::Sink* sink_;
  std::error_code ec_;

  size_t io_write_cnt_ = 0;
  size_t io_write_bytes_ = 0;
  absl::flat_hash_map<std::string, uint64_t> err_count_;

  bool should_batch_ : 1;

  // Similarly to batch mode but is controlled by at operation level.
  bool should_aggregate_ : 1;
  bool has_replied_ : 1;
};

class MCReplyBuilder : public SinkReplyBuilder {
  bool noreply_;

 public:
  MCReplyBuilder(::io::Sink* stream);

  using SinkReplyBuilder::SendRaw;

  void SendError(std::string_view str, std::string_view type = std::string_view{}) final;

  // void SendGetReply(std::string_view key, uint32_t flags, std::string_view value) final;
  void SendMGetResponse(MGetResponse resp) final;

  void SendStored() final;
  void SendLong(long val) final;
  void SendSetSkipped() final;

  void SendClientError(std::string_view str);
  void SendNotFound();
  void SendSimpleString(std::string_view str) final;
  void SendProtocolError(std::string_view str) final;

  void SetNoreply(bool noreply) {
    noreply_ = noreply;
  }

  bool NoReply() const;
};

class RedisReplyBuilder : public SinkReplyBuilder {
 public:
  enum CollectionType { ARRAY, SET, MAP, PUSH };

  enum VerbatimFormat { TXT, MARKDOWN };

  using StrSpan = std::variant<absl::Span<const std::string>, absl::Span<const std::string_view>>;

  RedisReplyBuilder(::io::Sink* stream);

  void SetResp3(bool is_resp3);
  bool IsResp3() const;

  void SendError(std::string_view str, std::string_view type = {}) override;
  using SinkReplyBuilder::SendError;

  void SendMGetResponse(MGetResponse resp) override;

  void SendStored() override;
  void SendSetSkipped() override;
  void SendProtocolError(std::string_view str) override;

  virtual void SendNullArray();   // Send *-1
  virtual void SendEmptyArray();  // Send *0
  virtual void SendSimpleStrArr(StrSpan arr);
  virtual void SendStringArr(StrSpan arr, CollectionType type = ARRAY);

  virtual void SendNull();
  void SendLong(long val) override;
  virtual void SendDouble(double val);
  void SendSimpleString(std::string_view str) override;

  virtual void SendBulkString(std::string_view str);
  virtual void SendVerbatimString(std::string_view str, VerbatimFormat format = TXT);
  virtual void SendScoredArray(const std::vector<std::pair<std::string, double>>& arr,
                               bool with_scores);

  void StartArray(unsigned len);  // StartCollection(len, ARRAY)

  virtual void StartCollection(unsigned len, CollectionType type);

  static char* FormatDouble(double val, char* dest, unsigned dest_len);

 protected:
  struct WrappedStrSpan : public StrSpan {
    size_t Size() const;
    std::string_view operator[](size_t index) const;
  };

 private:
  void SendStringArrInternal(WrappedStrSpan arr, CollectionType type);

  bool is_resp3_ = false;
};

class ReqSerializer {
 public:
  explicit ReqSerializer(::io::Sink* stream) : sink_(stream) {
  }

  void SendCommand(std::string_view str);

  std::error_code ec() const {
    return ec_;
  }

 private:
  ::io::Sink* sink_;
  std::error_code ec_;
};

}  // namespace facade
