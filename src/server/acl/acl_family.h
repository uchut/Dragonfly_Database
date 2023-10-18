// Copyright 2022, DragonflyDB authors.  All rights reserved.
// See LICENSE for licensing terms.
//

#pragma once

#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

#include "facade/dragonfly_listener.h"
#include "facade/facade_types.h"
#include "helio/util/proactor_pool.h"
#include "server/acl/user_registry.h"
#include "server/command_registry.h"
#include "server/common.h"

namespace dfly {

class ConnectionContext;
namespace acl {

class AclFamily final {
 public:
  explicit AclFamily(UserRegistry* registry, util::ProactorPool* pool);

  void Register(CommandRegistry* registry);
  void Init(facade::Listener* listener, UserRegistry* registry);

 private:
  void Acl(CmdArgList args, ConnectionContext* cntx);
  void List(CmdArgList args, ConnectionContext* cntx);
  void SetUser(CmdArgList args, ConnectionContext* cntx);
  void DelUser(CmdArgList args, ConnectionContext* cntx);
  void WhoAmI(CmdArgList args, ConnectionContext* cntx);
  void Save(CmdArgList args, ConnectionContext* cntx);
  void Load(CmdArgList args, ConnectionContext* cntx);
  // Helper function for bootstrap
  bool Load();
  void Log(CmdArgList args, ConnectionContext* cntx);
  void Users(CmdArgList args, ConnectionContext* cntx);
  void Cat(CmdArgList args, ConnectionContext* cntx);
  void GetUser(CmdArgList args, ConnectionContext* cntx);
  void DryRun(CmdArgList args, ConnectionContext* cntx);

  // Helper function that updates all open connections and their
  // respective ACL fields on all the available proactor threads
  using NestedVector = std::vector<std::vector<uint64_t>>;
  void StreamUpdatesToAllProactorConnections(const std::vector<std::string>& user,
                                             const std::vector<uint32_t>& update_cat,
                                             const NestedVector& update_commands);

  // Helper function that closes all open connection from the deleted user
  void EvictOpenConnectionsOnAllProactors(std::string_view user);

  // Helper function that closes all open connections for users in the registry
  void EvictOpenConnectionsOnAllProactorsWithRegistry(const UserRegistry::RegistryType& registry);

  // Helper function that loads the acl state of an acl file into the user registry
  std::optional<facade::ErrorReply> LoadToRegistryFromFile(std::string_view full_path, bool init);

  std::string RegistryToString() const;

  facade::Listener* main_listener_{nullptr};
  UserRegistry* registry_;
  CommandRegistry* cmd_registry_;
  util::ProactorPool* pool_;
};

}  // namespace acl
}  // namespace dfly
