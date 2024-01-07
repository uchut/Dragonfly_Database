// Copyright 2023, DragonflyDB authors.  All rights reserved.
// See LICENSE for licensing terms.
//

#pragma once

#include <absl/container/btree_map.h>

#include <string>

#include "facade/conn_context.h"
#include "server/cluster/cluster_config.h"
#include "server/cluster/cluster_slot_migration.h"
#include "server/common.h"

namespace dfly {
class CommandRegistry;
class ConnectionContext;
class ServerFamily;
class DflyCmd;

class ClusterFamily {
 public:
  explicit ClusterFamily(ServerFamily* server_family);

  void Register(CommandRegistry* registry);

  // Returns a thread-local pointer.
  ClusterConfig* cluster_config();

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
  void DflyClusterStartSlotMigration(CmdArgList args, ConnectionContext* cntx);
  void DflySlotMigrationStatus(CmdArgList args, ConnectionContext* cntx);

  // DFLYMIGRATE is internal command defines several steps in slots migrations process
  void DflyMigrate(CmdArgList args, ConnectionContext* cntx);

  // DFLYMIGRATE CONF initiate first step in slots migration procedure
  // MigrationConf process this request and saving slots range and
  // target node port in outgoing_migration_infos_.
  // return sync_id and shard number to the target node
  void MigrationConf(CmdArgList args, ConnectionContext* cntx);

  // DFLYMIGRATE FLOW initiate second step in slots migration procedure
  // this request should be done for every shard on the target node
  // this method assocciate connection and shard that will be the data
  // source for migration
  void Flow(CmdArgList args, ConnectionContext* cntx);

  // create a ClusterSlotMigration entity which will execute migration
  ClusterSlotMigration* AddMigration(std::string host_ip, uint16_t port,
                                     std::vector<ClusterConfig::SlotRange> slots);

  // store info about migration and create unique session id
  uint32_t CreateMigrationSession(ConnectionContext* cntx, uint16_t port,
                                  std::vector<ClusterConfig::SlotRange> slots);

  // FlowInfo is used to store state, connection, and all auxiliary data
  // that is needed for correct slots (per shard) data transfer
  struct FlowInfo {
    facade::Connection* conn = nullptr;
  };

  // Whole slots migration process information
  struct MigrationInfo {
    MigrationInfo() = default;
    MigrationInfo(std::uint32_t flows_num, std::string ip, uint32_t sync_id, uint16_t port,
                  std::vector<ClusterConfig::SlotRange> slots)
        : host_ip(ip),
          flows(flows_num),
          slots(slots),
          sync_id(sync_id),
          port(port),
          state(ClusterSlotMigration::State::C_CONNECTING) {
    }
    std::string host_ip;
    std::vector<FlowInfo> flows;
    std::vector<ClusterConfig::SlotRange> slots;
    uint32_t sync_id;
    uint16_t port;
    ClusterSlotMigration::State state = ClusterSlotMigration::State::C_NO_STATE;
  };

  std::shared_ptr<MigrationInfo> GetMigrationInfo(uint32_t sync_id);

  mutable Mutex migration_mu_;  // guard migrations operations
  // holds all incoming slots migrations that are currently in progress.
  std::vector<std::unique_ptr<ClusterSlotMigration>> incoming_migrations_jobs_
      ABSL_GUARDED_BY(migration_mu_);

  uint32_t next_sync_id_ = 1;
  // holds all outgoing slots migrations that are currently in progress
  using MigrationInfoMap = absl::btree_map<uint32_t, std::shared_ptr<MigrationInfo>>;
  MigrationInfoMap outgoing_migration_infos_;

 private:
  ClusterConfig::ClusterShard GetEmulatedShardInfo(ConnectionContext* cntx) const;

  ServerFamily* server_family_ = nullptr;
};

}  // namespace dfly
