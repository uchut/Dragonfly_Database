// Copyright 2022, DragonflyDB authors.  All rights reserved.
// See LICENSE for licensing terms.
//

#pragma once

#include <utility>

#include "base/varz_value.h"
#include "core/interpreter.h"
#include "facade/service_interface.h"
#include "server/acl/acl_commands_def.h"
#include "server/acl/acl_family.h"
#include "server/acl/user_registry.h"
#include "server/cluster/cluster_family.h"
#include "server/command_registry.h"
#include "server/config_registry.h"
#include "server/engine_shard_set.h"
#include "server/server_family.h"

namespace util {
class AcceptServer;
}  // namespace util

namespace dfly {

class Interpreter;
using facade::MemcacheParser;

class Service : public facade::ServiceInterface {
 public:
  struct InitOpts {
    bool disable_time_update;

    InitOpts() : disable_time_update{false} {
    }
  };

  explicit Service(util::ProactorPool* pp);
  ~Service();

  void Init(util::AcceptServer* acceptor, std::vector<facade::Listener*> listeners,
            const InitOpts& opts = InitOpts{});

  void Shutdown();

  // Prepare command execution, verify and execute, reply to context
  void DispatchCommand(CmdArgList args, facade::ConnectionContext* cntx) final;

  // Execute multiple consecutive commands, possibly in parallel by squashing
  size_t DispatchManyCommands(absl::Span<CmdArgList> args_list,
                              facade::ConnectionContext* cntx) final;

  // Check VerifyCommandExecution and invoke command with args
  bool InvokeCmd(const CommandId* cid, CmdArgList tail_args, ConnectionContext* reply_cntx);

  // Verify command can be executed now (check out of memory), always called immediately before
  // execution
  std::optional<facade::ErrorReply> VerifyCommandExecution(const CommandId* cid,
                                                           const ConnectionContext* cntx,
                                                           CmdArgList tail_args);

  // Verify command prepares excution in correct state.
  // It's usually called before command execution. Only for multi/exec transactions it's checked
  // when the command is queued for execution, not before the execution itself.
  std::optional<facade::ErrorReply> VerifyCommandState(const CommandId* cid, CmdArgList tail_args,
                                                       const ConnectionContext& cntx);

  void DispatchMC(const MemcacheParser::Command& cmd, std::string_view value,
                  facade::ConnectionContext* cntx) final;

  facade::ConnectionContext* CreateContext(util::FiberSocketBase* peer,
                                           facade::Connection* owner) final;

  std::pair<const CommandId*, CmdArgList> FindCmd(CmdArgList args) const;
  const CommandId* FindCmd(std::string_view) const;

  CommandRegistry* mutable_registry() {
    return &registry_;
  }

  facade::ErrorReply ReportUnknownCmd(std::string_view cmd_name);

  // Returns: the new state.
  // if from equals the old state then the switch is performed "to" is returned.
  // Otherwise, does not switch and returns the current state in the system.
  // Upon switch, updates cached global state in threadlocal ServerState struct.
  GlobalState SwitchState(GlobalState from, GlobalState to);

  void RequestLoadingState();
  void RemoveLoadingState();

  GlobalState GetGlobalState() const;

  void ConfigureHttpHandlers(util::HttpListenerBase* base, bool is_privileged) final;
  void OnClose(facade::ConnectionContext* cntx) final;

  Service::ContextInfo GetContextInfo(facade::ConnectionContext* cntx) const final;

  uint32_t shard_count() const {
    return shard_set->size();
  }

  // Used by tests.
  bool IsLocked(DbIndex db_index, std::string_view key) const;
  bool IsShardSetLocked() const;

  util::ProactorPool& proactor_pool() {
    return pp_;
  }

  absl::flat_hash_map<std::string, unsigned> UknownCmdMap() const;

  ScriptMgr* script_mgr() {
    return server_family_.script_mgr();
  }

  const ScriptMgr* script_mgr() const {
    return server_family_.script_mgr();
  }

  ServerFamily& server_family() {
    return server_family_;
  }

  // Utility function used in unit tests
  // Do not use in production, only meant to be used by unit tests
  void TestInit();

 private:
  static void Quit(CmdArgList args, ConnectionContext* cntx);
  static void Multi(CmdArgList args, ConnectionContext* cntx);

  static void Watch(CmdArgList args, ConnectionContext* cntx);
  static void Unwatch(CmdArgList args, ConnectionContext* cntx);

  void Discard(CmdArgList args, ConnectionContext* cntx);
  void Eval(CmdArgList args, ConnectionContext* cntx);
  void EvalSha(CmdArgList args, ConnectionContext* cntx);
  void Exec(CmdArgList args, ConnectionContext* cntx);
  void Publish(CmdArgList args, ConnectionContext* cntx);
  void Subscribe(CmdArgList args, ConnectionContext* cntx);
  void Unsubscribe(CmdArgList args, ConnectionContext* cntx);
  void PSubscribe(CmdArgList args, ConnectionContext* cntx);
  void PUnsubscribe(CmdArgList args, ConnectionContext* cntx);
  void Function(CmdArgList args, ConnectionContext* cntx);
  void Monitor(CmdArgList args, ConnectionContext* cntx);
  void Pubsub(CmdArgList args, ConnectionContext* cntx);
  void Command(CmdArgList args, ConnectionContext* cntx);

  void PubsubChannels(std::string_view pattern, ConnectionContext* cntx);
  void PubsubPatterns(ConnectionContext* cntx);
  void PubsubNumSub(CmdArgList channels, ConnectionContext* cntx);

  struct EvalArgs {
    std::string_view sha;  // only one of them is defined.
    CmdArgList keys, args;
  };

  // Return error if not all keys are owned by the server when running in cluster mode
  std::optional<facade::ErrorReply> CheckKeysOwnership(const CommandId* cid, CmdArgList args,
                                                       const ConnectionContext& dfly_cntx);

  void EvalInternal(CmdArgList args, const EvalArgs& eval_args, Interpreter* interpreter,
                    ConnectionContext* cntx);
  void CallSHA(CmdArgList args, std::string_view sha, Interpreter* interpreter,
               ConnectionContext* cntx);

  // Return optional payload - first received error that occured when executing commands.
  std::optional<facade::CapturingReplyBuilder::Payload> FlushEvalAsyncCmds(ConnectionContext* cntx,
                                                                           bool force = false);

  void CallFromScript(ConnectionContext* cntx, Interpreter::CallArgs& args);

  void RegisterCommands();
  void Register(CommandRegistry* registry);

  base::VarzValue::Map GetVarzStats();

  util::ProactorPool& pp_;

  acl::UserRegistry user_registry_;
  acl::AclFamily acl_family_;
  ServerFamily server_family_;
  cluster::ClusterFamily cluster_family_;
  CommandRegistry registry_;
  absl::flat_hash_map<std::string, unsigned> unknown_cmds_;

  const CommandId* exec_cid_;  // command id of EXEC command for pipeline squashing

  mutable util::fb2::Mutex mu_;
  GlobalState global_state_ ABSL_GUARDED_BY(mu_) = GlobalState::ACTIVE;
  uint32_t loading_state_counter_ ABSL_GUARDED_BY(mu_) = 0;
};

uint64_t GetMaxMemoryFlag();
void SetMaxMemoryFlag(uint64_t value);

}  // namespace dfly
