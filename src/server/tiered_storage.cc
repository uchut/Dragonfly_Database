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

class TieredStorageV2::ShardOpManager : public tiering::OpManager {
  friend class TieredStorageV2;

 public:
  ShardOpManager(TieredStorageV2* ts, DbSlice* db_slice) : ts_{ts}, db_slice_{db_slice} {
    cache_fetched_ = absl::GetFlag(FLAGS_tiered_storage_v2_cache_fetched);
  }

  // Find entry by key in db_slice and store external segment in place of original value
  void SetExternal(std::string_view key, tiering::DiskSegment segment) {
    if (auto pv = Find(key); pv) {
      pv->SetIoPending(false);
      pv->SetExternal(segment.offset, segment.length);  // TODO: Handle memory stats

      stats_.total_stashes++;
    }
  }

  void ClearIoPending(std::string_view key) {
    if (auto pv = Find(key); pv)
      pv->SetIoPending(false);
  }

  // Find entry by key and store it's up-to-date value in place of external segment.
  // Returns false if the value is outdated, true otherwise
  bool SetInMemory(std::string_view key, std::string_view value, tiering::DiskSegment segment) {
    if (auto pv = Find(key); pv && pv->IsExternal() && segment == pv->GetExternalSlice()) {
      pv->Reset();  // TODO: account for memory
      pv->SetString(value);

      stats_.total_fetches++;
      return true;
    }
    return false;
  }

  void ReportStashed(EntryId id, tiering::DiskSegment segment) override {
    if (holds_alternative<string_view>(id)) {
      SetExternal(get<string_view>(id), segment);
    } else {
      for (const auto& [sub_key, sub_segment] :
           ts_->bins_->ReportStashed(get<tiering::SmallBins::BinId>(id), segment))
        SetExternal(string_view{sub_key}, sub_segment);
    }
  }

  void ReportFetched(EntryId id, std::string_view value, tiering::DiskSegment segment,
                     bool modified) override {
    DCHECK(holds_alternative<string_view>(id));  // we never issue reads for bins

    // Modified values are always cached and deleted from disk
    if (!modified && !cache_fetched_)
      return;

    SetInMemory(get<string_view>(id), value, segment);

    // Delete value
    if (segment.length >= TieredStorageV2::kMinOccupancySize) {
      Delete(segment);
    } else {
      if (auto bin_segment = ts_->bins_->Delete(segment); bin_segment)
        Delete(*bin_segment);
    }
  }

  TieredStatsV2 GetStats() const {
    auto stats = stats_;
    stats.allocated_bytes = OpManager::storage_.GetStats().allocated_bytes;
    return stats;
  }

 private:
  PrimeValue* Find(std::string_view key) {
    // TODO: Get DbContext for transaction for correct dbid and time
    auto it = db_slice_->FindMutable(DbContext{}, key);
    return IsValid(it.it) ? &it.it->second : nullptr;
  }

  bool cache_fetched_ = false;

  TieredStatsV2 stats_;

  TieredStorageV2* ts_;
  DbSlice* db_slice_;
};

TieredStorageV2::TieredStorageV2(DbSlice* db_slice)
    : op_manager_{make_unique<ShardOpManager>(this, db_slice)},
      bins_{make_unique<tiering::SmallBins>()} {
}

TieredStorageV2::~TieredStorageV2() {
}

std::error_code TieredStorageV2::Open(string_view path) {
  return op_manager_->Open(path);
}

void TieredStorageV2::Close() {
  op_manager_->Close();
}

util::fb2::Future<std::string> TieredStorageV2::Read(string_view key, const PrimeValue& value) {
  DCHECK(value.IsExternal());
  util::fb2::Future<std::string> future;
  op_manager_->Enqueue(key, value.GetExternalSlice(), [future](std::string* value) mutable {
    future.Resolve(*value);
    return false;
  });
  return future;
}

template <typename T>
util::fb2::Future<T> TieredStorageV2::Modify(std::string_view key, const PrimeValue& value,
                                             std::function<T(std::string*)> modf) {
  DCHECK(value.IsExternal());
  util::fb2::Future<T> future;
  auto cb = [future, modf = std::move(modf)](std::string* value) mutable {
    future.Resolve(modf(value));
    return true;
  };
  op_manager_->Enqueue(key, value.GetExternalSlice(), std::move(cb));
  return future;
}

template util::fb2::Future<size_t> TieredStorageV2::Modify(
    std::string_view key, const PrimeValue& value, std::function<size_t(std::string*)> modf);

void TieredStorageV2::Stash(string_view key, PrimeValue* value) {
  DCHECK(!value->IsExternal() && !value->HasIoPending());

  string buf;
  string_view value_sv = value->GetSlice(&buf);
  value->SetIoPending(true);

  if (value->Size() >= kMinOccupancySize) {
    if (auto ec = op_manager_->Stash(key, value_sv); ec)
      value->SetIoPending(false);
  } else if (auto bin = bins_->Stash(key, value_sv); bin) {
    if (auto ec = op_manager_->Stash(bin->first, bin->second); ec) {
      for (const string& key : bins_->ReportStashAborted(bin->first))
        op_manager_->ClearIoPending(key);  // clear IO_PENDING flag
    }
  }
}
void TieredStorageV2::Delete(string_view key, PrimeValue* value) {
  if (value->IsExternal()) {
    tiering::DiskSegment segment = value->GetExternalSlice();
    if (segment.length >= kMinOccupancySize) {
      op_manager_->Delete(segment);
    } else if (auto bin = bins_->Delete(segment); bin) {
      op_manager_->Delete(*bin);
    }
    value->Reset();
  } else {
    DCHECK(value->HasIoPending());
    if (value->Size() >= kMinOccupancySize) {
      op_manager_->Delete(key);
    } else if (auto bin = bins_->Delete(key); bin) {
      op_manager_->Delete(*bin);
    }
    value->SetIoPending(false);
  }
}

bool TieredStorageV2::ShouldStash(const PrimeValue& pv) {
  return !pv.IsExternal() && pv.ObjType() == OBJ_STRING && pv.Size() >= kMinValueSize;
}

TieredStatsV2 TieredStorageV2::GetStats() const {
  return op_manager_->GetStats();
}

}  // namespace dfly
