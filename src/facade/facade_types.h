// Copyright 2022, DragonflyDB authors.  All rights reserved.
// See LICENSE for licensing terms.
//

#pragma once

#include <absl/container/flat_hash_map.h>
#include <absl/types/span.h>

#include <optional>
#include <string>
#include <string_view>
#include <variant>

#include "facade/op_status.h"

namespace facade {

#if defined(__clang__)
#if defined(__has_feature)
#if __has_feature(address_sanitizer)
constexpr size_t kSanitizerOverhead = 24u;
#else
constexpr size_t kSanitizerOverhead = 0u;
#endif
#endif
#else
#ifdef __SANITIZE_ADDRESS__
constexpr size_t kSanitizerOverhead = 24u;
#else
constexpr size_t kSanitizerOverhead = 0u;
#endif
#endif

enum class Protocol : uint8_t { MEMCACHE = 1, REDIS = 2 };

using MutableSlice = absl::Span<char>;
using CmdArgList = absl::Span<MutableSlice>;
using CmdArgVec = std::vector<MutableSlice>;

inline std::string_view ToSV(MutableSlice slice) {
  return std::string_view{slice.data(), slice.size()};
}

inline std::string_view ToSV(std::string_view slice) {
  return slice;
}

struct ConnectionStats {
  size_t read_buf_capacity = 0;                // total capacity of input buffers
  uint64_t dispatch_queue_entries = 0;         // total number of dispatch queue entries
  size_t dispatch_queue_bytes = 0;             // total size of all dispatch queue entries
  size_t dispatch_queue_subscriber_bytes = 0;  // total size of all publish messages

  size_t pipeline_cmd_cache_bytes = 0;

  uint64_t io_read_cnt = 0;
  size_t io_read_bytes = 0;

  uint64_t command_cnt = 0;
  uint64_t pipelined_cmd_cnt = 0;
  uint64_t pipelined_cmd_latency = 0;  // in microseconds
  uint64_t conn_received_cnt = 0;

  uint32_t num_conns = 0;
  uint32_t num_replicas = 0;
  uint32_t num_blocked_clients = 0;
  uint64_t num_migrations = 0;
  ConnectionStats& operator+=(const ConnectionStats& o);
};

struct ReplyStats {
  struct SendStats {
    int64_t count = 0;
    int64_t total_duration = 0;

    SendStats& operator+=(const SendStats& other) {
      static_assert(sizeof(SendStats) == 16u);

      count += other.count;
      total_duration += other.total_duration;
      return *this;
    }
  };

  // Send() operations that are written to sockets
  SendStats send_stats;

  size_t io_write_cnt = 0;
  size_t io_write_bytes = 0;
  absl::flat_hash_map<std::string, uint64_t> err_count;
  size_t script_error_count = 0;

  ReplyStats& operator+=(const ReplyStats& other);
};

struct FacadeStats {
  ConnectionStats conn_stats;
  ReplyStats reply_stats;

  FacadeStats& operator+=(const FacadeStats& other) {
    conn_stats += other.conn_stats;
    reply_stats += other.reply_stats;
    return *this;
  }
};

struct ErrorReply {
  explicit ErrorReply(std::string&& msg, std::string_view kind = {})
      : message{std::move(msg)}, kind{kind} {
  }
  explicit ErrorReply(std::string_view msg, std::string_view kind = {}) : message{msg}, kind{kind} {
  }
  explicit ErrorReply(const char* msg,
                      std::string_view kind = {})  // to resolve ambiguity of constructors above
      : message{std::string_view{msg}}, kind{kind} {
  }
  explicit ErrorReply(OpStatus status) : message{}, kind{}, status{status} {
  }

  std::string_view ToSv() const {
    return std::visit([](auto& str) { return std::string_view(str); }, message);
  }

  std::variant<std::string, std::string_view> message;
  std::string_view kind;
  std::optional<OpStatus> status{std::nullopt};
};

inline MutableSlice ToMSS(absl::Span<uint8_t> span) {
  return MutableSlice{reinterpret_cast<char*>(span.data()), span.size()};
}

inline std::string_view ArgS(CmdArgList args, size_t i) {
  auto arg = args[i];
  return std::string_view(arg.data(), arg.size());
}

constexpr inline unsigned long long operator""_MB(unsigned long long x) {
  return 1024L * 1024L * x;
}

constexpr inline unsigned long long operator""_KB(unsigned long long x) {
  return 1024L * x;
}

extern __thread FacadeStats* tl_facade_stats;

}  // namespace facade

namespace std {
ostream& operator<<(ostream& os, facade::CmdArgList args);

}  // namespace std
