// Copyright 2022, DragonflyDB authors.  All rights reserved.
// See LICENSE for licensing terms.
//

#pragma once

#include <optional>

#include "facade/op_status.h"
#include "server/common.h"
#include "server/table.h"

namespace dfly {

class ConnectionContext;
class CommandRegistry;
class StringMap;

using facade::OpResult;
using facade::OpStatus;

class HSetFamily {
 public:
  static void Register(CommandRegistry* registry);

  // Does not free lp.
  static StringMap* ConvertToStrMap(uint8_t* lp);

  static int32_t FieldExpireTime(const DbContext& db_context, const PrimeValue& pv,
                                 std::string_view field);

  static std::vector<long> SetFieldsExpireTime(const OpArgs& op_args, uint32_t ttl_sec,
                                               std::string_view key, CmdArgList values,
                                               PrimeValue* pv);

 private:
  // TODO: to move it to anonymous namespace in cc file.

  static void HExpire(CmdArgList args, ConnectionContext* cntx);
  static void HDel(CmdArgList args, ConnectionContext* cntx);
  static void HLen(CmdArgList args, ConnectionContext* cntx);
  static void HExists(CmdArgList args, ConnectionContext* cntx);
  static void HGet(CmdArgList args, ConnectionContext* cntx);
  static void HMGet(CmdArgList args, ConnectionContext* cntx);
  static void HIncrBy(CmdArgList args, ConnectionContext* cntx);
  static void HKeys(CmdArgList args, ConnectionContext* cntx);
  static void HVals(CmdArgList args, ConnectionContext* cntx);
  static void HGetAll(CmdArgList args, ConnectionContext* cntx);
  static void HIncrByFloat(CmdArgList args, ConnectionContext* cntx);
  static void HScan(CmdArgList args, ConnectionContext* cntx);
  static void HSet(CmdArgList args, ConnectionContext* cntx);
  static void HSetNx(CmdArgList args, ConnectionContext* cntx);
  static void HStrLen(CmdArgList args, ConnectionContext* cntx);
  static void HRandField(CmdArgList args, ConnectionContext* cntx);
};

}  // namespace dfly
