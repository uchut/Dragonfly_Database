// Copyright 2022, DragonflyDB authors.  All rights reserved.
// See LICENSE for licensing terms.
//

#pragma once

#include <string>

#include "facade/facade_types.h"
#include "facade/memcache_parser.h"
#include "util/fiber_socket_base.h"

namespace util {
class HttpListenerBase;
}  // namespace util

namespace facade {

class ConnectionContext;
class Connection;
struct ConnectionStats;

class ServiceInterface {
 public:
  virtual ~ServiceInterface() {
  }

  virtual void DispatchCommand(CmdArgList args, ConnectionContext* cntx) = 0;

  // Returns number of processed commands
  virtual size_t DispatchManyCommands(absl::Span<CmdArgList> args_list,
                                      ConnectionContext* cntx) = 0;

  virtual void DispatchMC(const MemcacheParser::Command& cmd, std::string_view value,
                          ConnectionContext* cntx) = 0;

  virtual ConnectionContext* CreateContext(util::FiberSocketBase* peer, Connection* owner) = 0;

  virtual void ConfigureHttpHandlers(util::HttpListenerBase* base, bool is_privileged) {
  }

  virtual void OnClose(ConnectionContext* cntx) {
  }

  struct ContextInfo {
    std::string Format() const;

    unsigned db_index;
    bool async_dispatch, conn_closing, subscribers, blocked;
  };

  virtual ContextInfo GetContextInfo(ConnectionContext* cntx) const {
    return {};
  }
};

}  // namespace facade
