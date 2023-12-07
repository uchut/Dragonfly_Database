// Copyright 2022, DragonflyDB authors.  All rights reserved.
// See LICENSE for licensing terms.
//

#include "server/table.h"

#include "base/flags.h"
#include "base/logging.h"

ABSL_FLAG(bool, enable_top_keys_tracking, false,
          "Enables / disables tracking of hot keys debugging feature");

namespace dfly {

#define ADD(x) (x) += o.x

// It should be const, but we override this variable in our tests so that they run faster.
unsigned kInitSegmentLog = 3;

DbTableStats& DbTableStats::operator+=(const DbTableStats& o) {
  constexpr size_t kDbSz = sizeof(DbTableStats);
  static_assert(kDbSz == 64);

  ADD(inline_keys);
  ADD(obj_memory_usage);
  ADD(strval_memory_usage);
  ADD(update_value_amount);
  ADD(listpack_blob_cnt);
  ADD(listpack_bytes);
  ADD(tiered_entries);
  ADD(tiered_size);

  return *this;
}

SlotStats& SlotStats::operator+=(const SlotStats& o) {
  static_assert(sizeof(SlotStats) == 24);

  ADD(key_count);
  ADD(total_reads);
  ADD(total_writes);
  return *this;
}

DbTable::DbTable(PMR_NS::memory_resource* mr, DbIndex db_index)
    : prime(kInitSegmentLog, detail::PrimeTablePolicy{}, mr),
      expire(0, detail::ExpireTablePolicy{}, mr),
      mcflag(0, detail::ExpireTablePolicy{}, mr),
      top_keys({.enabled = absl::GetFlag(FLAGS_enable_top_keys_tracking)}),
      index(db_index) {
  if (ClusterConfig::IsEnabled()) {
    slots_stats.resize(ClusterConfig::kMaxSlotNum + 1);
  }
}

DbTable::~DbTable() {
}

void DbTable::Clear() {
  prime.size();
  prime.Clear();
  expire.Clear();
  mcflag.Clear();
  stats = DbTableStats{};
}

}  // namespace dfly
