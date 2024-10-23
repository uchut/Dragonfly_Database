// Copyright 2022, DragonflyDB authors.  All rights reserved.
// See LICENSE for licensing terms.
//

#include "base/init.h"
#include "facade/conn_context.h"
#include "facade/dragonfly_listener.h"
#include "facade/service_interface.h"
#include "util/accept_server.h"
#include "util/fibers/pool.h"

ABSL_FLAG(uint32_t, port, 6379, "server port");

using namespace util;
using namespace std;
using absl::GetFlag;

namespace facade {

namespace {

class OkService : public ServiceInterface {
 public:
  void DispatchCommand(ArgSlice args, ConnectionContext* cntx) final {
    cntx->SendOk();
  }

  size_t DispatchManyCommands(absl::Span<ArgSlice> args_lists, ConnectionContext* cntx) final {
    for (auto args : args_lists)
      DispatchCommand(args, cntx);
    return args_lists.size();
  }

  void DispatchMC(const MemcacheParser::Command& cmd, std::string_view value,
                  ConnectionContext* cntx) final {
    cntx->SendError("");
  }

  ConnectionContext* CreateContext(util::FiberSocketBase* peer, Connection* owner) final {
    return new ConnectionContext{peer, owner};
  }
};

void RunEngine(ProactorPool* pool, AcceptServer* acceptor) {
  OkService service;
  pool->Await([](auto*) { tl_facade_stats = new FacadeStats; });

  acceptor->AddListener(GetFlag(FLAGS_port), new Listener{Protocol::REDIS, &service});

  acceptor->Run();
  acceptor->Wait();
}

}  // namespace

}  // namespace facade

#ifdef __linux__
#define USE_URING 1
#else
#define USE_URING 0
#endif

int main(int argc, char* argv[]) {
  MainInitGuard guard(&argc, &argv);

  CHECK_GT(GetFlag(FLAGS_port), 0u);

#if USE_URING
  unique_ptr<util::ProactorPool> pp(fb2::Pool::IOUring(1024));
#else
  unique_ptr<util::ProactorPool> pp(fb2::Pool::Epoll());
#endif
  pp->Run();

  AcceptServer acceptor(pp.get());
  facade::RunEngine(pp.get(), &acceptor);

  pp->Stop();

  return 0;
}
