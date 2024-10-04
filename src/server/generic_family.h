// Copyright 2022, DragonflyDB authors.  All rights reserved.
// See LICENSE for licensing terms.
//

#pragma once

#include "base/flags.h"
#include "facade/facade_types.h"
#include "server/common.h"
#include "server/table.h"

ABSL_DECLARE_FLAG(uint32_t, dbnum);

namespace util {
class ProactorPool;
}  // namespace util

namespace dfly {

using facade::ErrorReply;
using facade::OpResult;
using facade::OpStatus;

class ConnectionContext;
class CommandRegistry;
class EngineShard;

enum ExpireFlags {
  EXPIRE_ALWAYS = 0,
  EXPIRE_NX = 1 << 0,  // Set expiry only when key has no expiry
  EXPIRE_XX = 1 << 2,  // Set expiry only when the key has expiry
  EXPIRE_GT = 1 << 3,  // GT: Set expiry only when the new expiry is greater than current one
  EXPIRE_LT = 1 << 4,  // LT: Set expiry only when the new expiry is less than current one
};

class GenericFamily {
 public:
  static void Register(CommandRegistry* registry);

  // Accessed by Service::Exec and Service::Watch as an utility.
  static OpResult<uint32_t> OpExists(const OpArgs& op_args, const ShardArgs& keys);
  static OpResult<uint32_t> OpDel(const OpArgs& op_args, const ShardArgs& keys);

 private:
  static void Del(CmdArgList args, ConnectionContext* cntx);
  static void Ping(CmdArgList args, ConnectionContext* cntx);
  static void Exists(CmdArgList args, ConnectionContext* cntx);
  static void Expire(CmdArgList args, ConnectionContext* cntx);
  static void ExpireAt(CmdArgList args, ConnectionContext* cntx);
  static void Persist(CmdArgList args, ConnectionContext* cntx);
  static void Keys(CmdArgList args, ConnectionContext* cntx);
  static void PexpireAt(CmdArgList args, ConnectionContext* cntx);
  static void Pexpire(CmdArgList args, ConnectionContext* cntx);
  static void Stick(CmdArgList args, ConnectionContext* cntx);
  static void Sort(CmdArgList args, ConnectionContext* cntx);
  static void Move(CmdArgList args, ConnectionContext* cntx);

  static void Rename(CmdArgList args, ConnectionContext* cntx);
  static void RenameNx(CmdArgList args, ConnectionContext* cntx);
  static void ExpireTime(CmdArgList args, ConnectionContext* cntx);
  static void PExpireTime(CmdArgList args, ConnectionContext* cntx);
  static void Ttl(CmdArgList args, ConnectionContext* cntx);
  static void Pttl(CmdArgList args, ConnectionContext* cntx);

  static void Echo(CmdArgList args, ConnectionContext* cntx);
  static void Select(CmdArgList args, ConnectionContext* cntx);
  static void Scan(CmdArgList args, ConnectionContext* cntx);
  static void Time(CmdArgList args, ConnectionContext* cntx);
  static void Type(CmdArgList args, ConnectionContext* cntx);
  static void Dump(CmdArgList args, ConnectionContext* cntx);
  static void Restore(CmdArgList args, ConnectionContext* cntx);
  static void RandomKey(CmdArgList args, ConnectionContext* cntx);
  static void FieldTtl(CmdArgList args, ConnectionContext* cntx);
  static void FieldExpire(CmdArgList args, ConnectionContext* cntx);

  static ErrorReply RenameGeneric(CmdArgList args, bool destination_should_not_exist,
                                  ConnectionContext* cntx);

  static void ExpireTimeGeneric(CmdArgList args, ConnectionContext* cntx, TimeUnit unit);
  static void TtlGeneric(CmdArgList args, ConnectionContext* cntx, TimeUnit unit);

  static OpResult<uint64_t> OpExpireTime(Transaction* t, EngineShard* shard, std::string_view key);
  static OpResult<uint64_t> OpTtl(Transaction* t, EngineShard* shard, std::string_view key);
  static OpResult<void> OpRen(const OpArgs& op_args, std::string_view from, std::string_view to,
                              bool destination_should_not_exist);
  static OpStatus OpMove(const OpArgs& op_args, std::string_view key, DbIndex target_db);
};

}  // namespace dfly
