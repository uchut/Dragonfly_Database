// Copyright 2022, DragonflyDB authors.  All rights reserved.
// See LICENSE for licensing terms.
//

#pragma once

#include <absl/base/internal/spinlock.h>

#include <memory>
#include <system_error>
#include <utility>
#include <vector>

#include "facade/facade_types.h"
#include "util/fiber_socket_base.h"
#include "util/fibers/proactor_base.h"
#include "util/http/http_handler.h"
#include "util/listener_interface.h"

typedef struct ssl_ctx_st SSL_CTX;

namespace facade {

class ServiceInterface;

class Listener : public util::ListenerInterface {
 public:
  // The Role PRIVILEGED is for admin port/listener
  // The Role MAIN is for the main listener on main port
  // The Role OTHER is for all the other listeners
  enum class Role { PRIVILEGED, MAIN, OTHER };
  Listener(Protocol protocol, ServiceInterface*, Role role = Role::OTHER);
  ~Listener();

  std::error_code ConfigureServerSocket(int fd) final;

  // Wait until all command dispatches that are currently in progress finish,
  // ignore commands from issuer connection.
  bool AwaitCurrentDispatches(absl::Duration timeout, util::Connection* issuer);

  // ReconfigureTLS MUST be called from the same proactor as the listener.
  bool ReconfigureTLS();

  bool IsPrivilegedInterface() const;
  bool IsMainInterface() const;

 private:
  util::Connection* NewConnection(ProactorBase* proactor) final;
  ProactorBase* PickConnectionProactor(util::FiberSocketBase* sock) final;

  void OnConnectionStart(util::Connection* conn) final;
  void OnConnectionClose(util::Connection* conn) final;
  void OnMaxConnectionsReached(util::FiberSocketBase* sock) final;
  void PreAcceptLoop(ProactorBase* pb) final;

  void PreShutdown() final;
  void PostShutdown() final;

  std::unique_ptr<util::HttpListenerBase> http_base_;

  ServiceInterface* service_;

  struct PerThread {
    int32_t num_connections{0};
    unsigned napi_id = 0;
  };
  std::vector<PerThread> per_thread_;

  std::atomic_uint32_t next_id_{0};

  Role role_;

  uint32_t conn_cnt_{0};
  uint32_t min_cnt_thread_id_{0};
  int32_t min_cnt_{0};
  absl::base_internal::SpinLock mutex_;

  Protocol protocol_;
  SSL_CTX* ctx_ = nullptr;
};

}  // namespace facade
