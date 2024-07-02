// Copyright 2024, DragonflyDB authors.  All rights reserved.
// See LICENSE for licensing terms.
//

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace dfly::cluster {

using SlotId = uint16_t;

constexpr SlotId kMaxSlotNum = 0x3FFF;
constexpr SlotId kInvalidSlotId = kMaxSlotNum + 1;

struct SlotRange {
  static constexpr SlotId kMaxSlotId = 0x3FFF;
  SlotId start = 0;
  SlotId end = 0;

  bool operator==(const SlotRange& r) const noexcept {
    return start == r.start && end == r.end;
  }

  bool operator<(const SlotRange& r) const noexcept {
    return start < r.start || (start == r.start && end < r.end);
  }

  bool IsValid() const noexcept {
    return start <= end && start <= kMaxSlotId && end <= kMaxSlotId;
  }

  bool Contains(SlotId id) const noexcept {
    return id >= start && id <= end;
  }

  std::string ToString() const;
};

class SlotRanges {
 public:
  SlotRanges() = default;
  explicit SlotRanges(std::vector<SlotRange> ranges);

  bool Contains(SlotId id) const noexcept {
    for (const auto& sr : ranges_) {
      if (sr.Contains(id))
        return true;
    }
    return false;
  }

  size_t Size() const noexcept {
    return ranges_.size();
  }

  bool Empty() const noexcept {
    return ranges_.empty();
  }

  void Merge(const SlotRanges& sr);

  bool operator==(const SlotRanges& r) const noexcept {
    return ranges_ == r.ranges_;
  }

  std::string ToString() const;

  auto begin() const noexcept {
    return ranges_.cbegin();
  }

  auto end() const noexcept {
    return ranges_.cend();
  }

 private:
  std::vector<SlotRange> ranges_;
};

struct ClusterNodeInfo {
  std::string id;
  std::string ip;
  uint16_t port = 0;

  bool operator==(const ClusterNodeInfo& r) const noexcept {
    return port == r.port && ip == r.ip && id == r.id;
  }
};

struct MigrationInfo {
  SlotRanges slot_ranges;
  ClusterNodeInfo node_info;

  bool operator==(const MigrationInfo& r) const noexcept {
    return node_info == r.node_info && slot_ranges == r.slot_ranges;
  }

  std::string ToString() const;
};

struct ClusterShardInfo {
  SlotRanges slot_ranges;
  ClusterNodeInfo master;
  std::vector<ClusterNodeInfo> replicas;
  std::vector<MigrationInfo> migrations;
};

using ClusterShardInfos = std::vector<ClusterShardInfo>;

// MigrationState constants are ordered in state changing order
enum class MigrationState : uint8_t {
  C_NO_STATE,
  C_CONNECTING,
  C_SYNC,
  C_ERROR,
  C_FINISHED,
};

SlotId KeySlot(std::string_view key);

void InitializeCluster();
bool IsClusterEnabled();
bool IsClusterEmulated();
bool IsClusterEnabledOrEmulated();
bool IsClusterShardedByTag();

}  // namespace dfly::cluster
