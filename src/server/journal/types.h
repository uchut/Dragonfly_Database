// Copyright 2022, Roman Gershman.  All rights reserved.
// See LICENSE for licensing terms.
//
#pragma once

#include <optional>
#include <string>
#include <variant>

#include "server/cluster/cluster_config.h"
#include "server/common.h"
#include "server/table.h"

namespace dfly {
namespace journal {

enum class Op : uint8_t {
  NOOP = 0,
  SELECT = 6,
  EXPIRED = 9,
  COMMAND = 10,
  MULTI_COMMAND = 11,
  EXEC = 12,
  PING = 13,
  FIN = 14
};

struct EntryBase {
  TxId txid;
  Op opcode;
  DbIndex dbid;
  uint32_t shard_cnt;
  std::optional<SlotId> slot;
};

// This struct represents a single journal entry.
// Those are either control instructions or commands.
struct Entry : public EntryBase {
  // Payload represents a non-owning view into a command executed on the shard.
  using Payload =
      std::variant<std::monostate,                           // No payload.
                   std::pair<std::string_view, CmdArgList>,  // Parts of a full command.
                   std::pair<std::string_view, ArgSlice>     // Command and its shard parts.
                   >;

  Entry(TxId txid, Op opcode, DbIndex dbid, uint32_t shard_cnt, std::optional<SlotId> slot_id,
        Payload pl)
      : EntryBase{txid, opcode, dbid, shard_cnt, slot_id}, payload{pl} {
  }

  Entry(journal::Op opcode, DbIndex dbid, std::optional<SlotId> slot_id)
      : EntryBase{0, opcode, dbid, 0, slot_id}, payload{} {
  }

  Entry(TxId txid, journal::Op opcode, DbIndex dbid, uint32_t shard_cnt,
        std::optional<SlotId> slot_id)
      : EntryBase{txid, opcode, dbid, shard_cnt, slot_id}, payload{} {
  }

  bool HasPayload() const {
    return !std::holds_alternative<std::monostate>(payload);
  }

  std::string ToString() const;

  Payload payload;
};

struct ParsedEntry : public EntryBase {
  struct CmdData {
    std::unique_ptr<char[]> command_buf;
    CmdArgVec cmd_args;  // represents the parsed command.
  };
  CmdData cmd;

  std::string ToString() const;
};

struct JournalItem {
  LSN lsn;
  Op opcode;
  std::string data;
  std::optional<SlotId> slot;
};

using ChangeCallback = std::function<void(const JournalItem&, bool await)>;

}  // namespace journal
}  // namespace dfly
