// Copyright 2023, DragonflyDB authors.  All rights reserved.
// See LICENSE for licensing terms.
//

#pragma once

#include <absl/container/btree_map.h>

#include <string>

#include "facade/conn_context.h"
#include "server/cluster/cluster_config.h"
#include "server/cluster/incoming_slot_migration.h"
#include "server/cluster/outgoing_slot_migration.h"
#include "server/common.h"

namespace dfly {
class ServerFamily;
class CommandRegistry;
class ConnectionContext;
}  // namespace dfly

namespace dfly::cluster {

class ClusterFamily {
 public:
  explicit ClusterFamily(ServerFamily* server_family);

  void Register(CommandRegistry* registry);

  // Returns a thread-local pointer.
  ClusterConfig* cluster_config();

  void UpdateConfig(const std::vector<SlotRange>& slots, bool enable);

  const std::string& MyID() const {
    return id_;
  }

 private:
  // Cluster commands compatible with Redis
  void Cluster(CmdArgList args, ConnectionContext* cntx);
  void ClusterHelp(ConnectionContext* cntx);
  void ClusterShards(ConnectionContext* cntx);
  void ClusterSlots(ConnectionContext* cntx);
  void ClusterNodes(ConnectionContext* cntx);
  void ClusterInfo(ConnectionContext* cntx);

  void KeySlot(CmdArgList args, ConnectionContext* cntx);

  void ReadOnly(CmdArgList args, ConnectionContext* cntx);
  void ReadWrite(CmdArgList args, ConnectionContext* cntx);

  // Custom Dragonfly commands for cluster management
  void DflyCluster(CmdArgList args, ConnectionContext* cntx);
  void DflyClusterConfig(CmdArgList args, ConnectionContext* cntx);
  void DflyClusterGetSlotInfo(CmdArgList args, ConnectionContext* cntx);
  void DflyClusterMyId(CmdArgList args, ConnectionContext* cntx);
  void DflyClusterFlushSlots(CmdArgList args, ConnectionContext* cntx);

 private:  // Slots migration section
  void DflySlotMigrationStatus(CmdArgList args, ConnectionContext* cntx);

  // DFLYMIGRATE is internal command defines several steps in slots migrations process
  void DflyMigrate(CmdArgList args, ConnectionContext* cntx);

  // DFLYMIGRATE INIT is internal command to create incoming migration object
  void InitMigration(CmdArgList args, ConnectionContext* cntx);

  // DFLYMIGRATE FLOW initiate second step in slots migration procedure
  // this request should be done for every shard on the target node
  // this method assocciate connection and shard that will be the data
  // source for migration
  void DflyMigrateFlow(CmdArgList args, ConnectionContext* cntx);

  void DflyMigrateAck(CmdArgList args, ConnectionContext* cntx);

  std::shared_ptr<IncomingSlotMigration> GetIncomingMigration(std::string_view source_id);

  void StartSlotMigrations(std::vector<MigrationInfo> migrations);
  void RemoveOutgoingMigrations(const std::vector<MigrationInfo>& migrations);
  void RemoveIncomingMigrations(const std::vector<MigrationInfo>& migrations);

  // store info about migration and create unique session id
  std::shared_ptr<OutgoingMigration> CreateOutgoingMigration(MigrationInfo info);

  mutable util::fb2::Mutex migration_mu_;  // guard migrations operations
  // holds all incoming slots migrations that are currently in progress.
  std::vector<std::shared_ptr<IncomingSlotMigration>> incoming_migrations_jobs_
      ABSL_GUARDED_BY(migration_mu_);

  // holds all outgoing slots migrations that are currently in progress
  std::vector<std::shared_ptr<OutgoingMigration>> outgoing_migration_jobs_
      ABSL_GUARDED_BY(migration_mu_);

 private:
  ClusterShardInfo GetEmulatedShardInfo(ConnectionContext* cntx) const;

  mutable util::fb2::Mutex config_update_mu_;

  std::string id_;

  ServerFamily* server_family_ = nullptr;
};

}  // namespace dfly::cluster
