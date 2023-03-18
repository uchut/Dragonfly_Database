// Copyright 2022, DragonflyDB authors.  All rights reserved.
// See LICENSE for licensing terms.
//

#pragma once

#include "server/conn_context.h"

namespace dfly {

class ServerFamily;

class MemoryCmd {
 public:
  MemoryCmd(ServerFamily* owner, ConnectionContext* cntx);

  void Run(CmdArgList args);

 private:
  ConnectionContext* cntx_;
};

}  // namespace dfly
