// Copyright 2022, DragonflyDB authors.  All rights reserved.
// See LICENSE for licensing terms.
//

#include "server/tiered_storage.h"

#include <mimalloc.h>

#include <memory>
#include <optional>
#include <variant>

#include "absl/cleanup/cleanup.h"
#include "absl/flags/internal/flag.h"
#include "base/flags.h"
#include "base/logging.h"
#include "server/common.h"
#include "server/db_slice.h"
#include "server/engine_shard_set.h"
#include "server/table.h"
#include "server/tiering/common.h"
#include "server/tiering/op_manager.h"
#include "server/tiering/small_bins.h"

ABSL_FLAG(bool, tiered_storage_v2_cache_fetched, true,
          "WIP: Load results of offloaded reads to memory");

namespace dfly {

using namespace std;
using namespace util;

using namespace tiering::literals;

using KeyRef = tiering::OpManager::KeyRef;

namespace {

bool OccupiesWholePages(size_t size) {
  return size >= TieredStorageV2::kMinOccupancySize;
}

}  // anonymous namespace

class TieredStorageV2::ShardOpManager : public tiering::OpManager {
  friend class TieredStorageV2;

 public:
  ShardOpManager(TieredStorageV2* ts, DbSlice* db_slice) : ts_{ts}, db_slice_{db_slice} {
    cache_fetched_ = absl::GetFlag(FLAGS_tiered_storage_v2_cache_fetched);
  }

  // Find entry by key in db_slice and store external segment in place of original value
  void SetExternal(OpManager::KeyRef key, tiering::DiskSegment segment) {
    if (auto pv = Find(key); pv) {
      pv->SetIoPending(false);
      pv->SetExternal(segment.offset, segment.length);  // TODO: Handle memory stats

      stats_.total_stashes++;
    }
  }

  // Find bin by id and call SetExternal for all contained entries
  void SetExternal(tiering::SmallBins::BinId id, tiering::DiskSegment segment) {
    for (const auto& [sub_dbid, sub_key, sub_segment] : ts_->bins_->ReportStashed(id, segment))
      SetExternal({sub_dbid, sub_key}, sub_segment);
  }

  // Clear IO pending flag for entry
  void ClearIoPending(OpManager::KeyRef key) {
    if (auto pv = Find(key); pv) {
      pv->SetIoPending(false);
      stats_.total_cancels++;
    }
  }

  // Clear IO pending flag for all contained entries of bin
  void ClearIoPending(tiering::SmallBins::BinId id) {
    for (const auto& key : ts_->bins_->ReportStashAborted(id))
      ClearIoPending(key);
  }

  // Find entry by key and store it's up-to-date value in place of external segment.
  // Returns false if the value is outdated, true otherwise
  bool SetInMemory(OpManager::KeyRef key, string_view value, tiering::DiskSegment segment) {
    if (auto pv = Find(key); pv && pv->IsExternal() && segment == pv->GetExternalSlice()) {
      pv->Reset();  // TODO: account for memory
      pv->SetString(value);

      stats_.total_fetches++;
      return true;
    }
    return false;
  }

  void ReportStashed(EntryId id, tiering::DiskSegment segment, error_code ec) override {
    if (ec) {
      VLOG(1) << "Stash failed " << ec.message();
      visit([this](auto id) { ClearIoPending(id); }, id);
    } else {
      visit([this, segment](auto id) { SetExternal(id, segment); }, id);
    }
  }

  void ReportFetched(EntryId id, string_view value, tiering::DiskSegment segment,
                     bool modified) override {
    DCHECK(holds_alternative<OpManager::KeyRef>(id));  // we never issue reads for bins

    // Modified values are always cached and deleted from disk
    if (!modified && !cache_fetched_)
      return;

    SetInMemory(get<OpManager::KeyRef>(id), value, segment);

    // Delete value
    if (OccupiesWholePages(segment.length)) {
      Delete(segment);
    } else {
      if (auto bin_segment = ts_->bins_->Delete(segment); bin_segment)
        Delete(*bin_segment);
    }
  }

 private:
  friend class TieredStorageV2;

  PrimeValue* Find(OpManager::KeyRef key) {
    // TODO: Get DbContext for transaction for correct dbid and time
    // Bypass all update and stat mechanisms
    auto it = db_slice_->GetDBTable(key.first)->prime.Find(key.second);
    return IsValid(it) ? &it->second : nullptr;
  }

  bool cache_fetched_ = false;

  struct {
    size_t total_stashes = 0, total_fetches = 0, total_cancels = 0;
  } stats_;

  TieredStorageV2* ts_;
  DbSlice* db_slice_;
};

TieredStorageV2::TieredStorageV2(DbSlice* db_slice)
    : op_manager_{make_unique<ShardOpManager>(this, db_slice)},
      bins_{make_unique<tiering::SmallBins>()} {
}

TieredStorageV2::~TieredStorageV2() {
}

error_code TieredStorageV2::Open(string_view path) {
  return op_manager_->Open(absl::StrCat(path, ProactorBase::me()->GetPoolIndex()));
}

void TieredStorageV2::Close() {
  op_manager_->Close();
}

util::fb2::Future<string> TieredStorageV2::Read(DbIndex dbid, string_view key,
                                                const PrimeValue& value) {
  DCHECK(value.IsExternal());
  util::fb2::Future<string> future;
  auto cb = [future](string* value) mutable {
    future.Resolve(*value);
    return false;
  };
  op_manager_->Enqueue(KeyRef(dbid, key), value.GetExternalSlice(), std::move(cb));
  return future;
}

template <typename T>
util::fb2::Future<T> TieredStorageV2::Modify(DbIndex dbid, std::string_view key,
                                             const PrimeValue& value,
                                             std::function<T(std::string*)> modf) {
  DCHECK(value.IsExternal());
  util::fb2::Future<T> future;
  auto cb = [future, modf = std::move(modf)](std::string* value) mutable {
    future.Resolve(modf(value));
    return true;
  };
  op_manager_->Enqueue(KeyRef(dbid, key), value.GetExternalSlice(), std::move(cb));
  return future;
}

template util::fb2::Future<size_t> TieredStorageV2::Modify(
    DbIndex dbid, std::string_view key, const PrimeValue& value,
    std::function<size_t(std::string*)> modf);

void TieredStorageV2::Stash(DbIndex dbid, string_view key, PrimeValue* value) {
  DCHECK(!value->IsExternal() && !value->HasIoPending());

  string buf;
  string_view value_sv = value->GetSlice(&buf);
  value->SetIoPending(true);

  tiering::OpManager::EntryId id;
  error_code ec;
  if (OccupiesWholePages(value->Size())) {  // large enough for own page
    id = KeyRef(dbid, key);
    ec = op_manager_->Stash(id, value_sv);
  } else if (auto bin = bins_->Stash(dbid, key, value_sv); bin) {
    id = bin->first;
    ec = op_manager_->Stash(bin->first, bin->second);
  }

  if (ec) {
    VLOG(1) << "Stash failed immediately" << ec.message();
    visit([this](auto id) { op_manager_->ClearIoPending(id); }, id);
  }
}
void TieredStorageV2::Delete(PrimeValue* value) {
  DCHECK(value->IsExternal());
  tiering::DiskSegment segment = value->GetExternalSlice();
  if (OccupiesWholePages(segment.length)) {
    op_manager_->Delete(segment);
  } else if (auto bin = bins_->Delete(segment); bin) {
    op_manager_->Delete(*bin);
  }
  value->Reset();
}

void TieredStorageV2::CancelStash(DbIndex dbid, std::string_view key, PrimeValue* value) {
  DCHECK(value->HasIoPending());
  if (OccupiesWholePages(value->Size())) {
    op_manager_->Delete(KeyRef(dbid, key));
  } else if (auto bin = bins_->Delete(dbid, key); bin) {
    op_manager_->Delete(*bin);
  }
  value->SetIoPending(false);
}

bool TieredStorageV2::ShouldStash(const PrimeValue& pv) {
  return !pv.IsExternal() && pv.ObjType() == OBJ_STRING && pv.Size() >= kMinValueSize;
}

TieredStats TieredStorageV2::GetStats() const {
  TieredStats stats{};

  {  // ShardOpManager stats
    auto shard_stats = op_manager_->stats_;
    stats.total_fetches = shard_stats.total_fetches;
    stats.total_stashes = shard_stats.total_stashes;
    stats.total_cancels = shard_stats.total_cancels;
  }

  {  // OpManager stats
    tiering::OpManager::Stats op_stats = op_manager_->GetStats();
    stats.pending_read_cnt = op_stats.pending_read_cnt;
    stats.pending_stash_cnt = op_stats.pending_stash_cnt;
    stats.allocated_bytes = op_stats.disk_stats.allocated_bytes;
    stats.capacity_bytes = op_stats.disk_stats.capacity_bytes;
  }

  {  // SmallBins stats
    tiering::SmallBins::Stats bins_stats = bins_->GetStats();
    stats.small_bins_cnt = bins_stats.stashed_bins_cnt;
    stats.small_bins_entries_cnt = bins_stats.stashed_entries_cnt;
    stats.small_bins_filling_bytes = bins_stats.current_bin_bytes;
  }

  return stats;
}

}  // namespace dfly
