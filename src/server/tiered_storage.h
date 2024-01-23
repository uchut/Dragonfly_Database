// Copyright 2023, DragonflyDB authors.  All rights reserved.
// See LICENSE for licensing terms.
//
#pragma once

#ifdef __linux__

#include <absl/container/flat_hash_map.h>

#include "core/external_alloc.h"
#include "core/fibers.h"
#include "server/common.h"
#include "server/io_mgr.h"
#include "server/table.h"

namespace dfly {

class DbSlice;

class TieredStorage {
 public:
  enum : uint16_t { kMinBlobLen = 64 };

  explicit TieredStorage(DbSlice* db_slice, size_t max_file_size);
  ~TieredStorage();

  std::error_code Open(const std::string& path);

  PrimeIterator Load(DbIndex db_index, PrimeIterator it, std::string_view key);

  // Schedules unloading of the item, pointed by the iterator.
  std::error_code ScheduleOffload(DbIndex db_index, PrimeIterator it, std::string_view key);

  void CancelIo(DbIndex db_index, PrimeIterator it);

  static bool EligibleForOffload(std::string_view val) {
    return val.size() >= kMinBlobLen;
  }

  void Free(PrimeIterator it, DbTableStats* stats);

  void Shutdown();

  TieredStats GetStats() const;

  const IoMgrStats& GetDiskStats() const {
    return io_mgr_.GetStats();
  }

  void CancelAllIos(DbIndex db_index);

  std::error_code Read(size_t offset, size_t len, char* dest);

 private:
  class InflightWriteRequest;

  void WriteSingle(DbIndex db_index, PrimeIterator it, size_t blob_len);

  // Returns a pair consisting of an bool denoting whether we can write to disk, and updated
  // iterator as this function can yield. 'it' should not be used after the call to this function.
  std::pair<bool, PrimeIterator> CanScheduleOffload(DbIndex db_index, PrimeIterator it,
                                                    std::string_view key);

  bool FlushPending(DbIndex db_index, unsigned bin_index);

  void InitiateGrow(size_t size);

  void FinishIoRequest(int io_res, InflightWriteRequest* req);
  void SetExternal(DbIndex db_index, size_t item_offset, PrimeValue* dest);

  DbSlice& db_slice_;
  IoMgr io_mgr_;
  ExternalAllocator alloc_;

  uint32_t num_active_requests_ = 0;

  struct PerDb;
  std::vector<PerDb*> db_arr_;

  absl::flat_hash_map<uint32_t, uint8_t> page_refcnt_;
  util::fb2::EventCount throttle_ec_;
  TieredStats stats_;
  size_t max_file_size_;
  size_t allocated_size_ = 0;
};

}  // namespace dfly

#else

#include "server/common.h"

class DbSlice;

// This is a stub implementation for non-linux platforms.
namespace dfly {
class TieredStorage {
 public:
  static constexpr size_t kMinBlobLen = size_t(-1);  // infinity.

  explicit TieredStorage(DbSlice* db_slice) {
  }
  ~TieredStorage() {
  }

  std::error_code Open(const std::string& path) {
    return {};
  }

  std::error_code Read(size_t offset, size_t len, char* dest) {
    return {};
  }

  // Schedules unloading of the item, pointed by the iterator.
  std::error_code ScheduleOffload(DbIndex db_index, PrimeIterator it) {
    return {};
  }

  void CancelIo(DbIndex db_index, PrimeIterator it) {
  }

  static bool EligibleForOffload(std::string_view val) {
    return false;
  }

  void Free(size_t offset, size_t len) {
  }

  void Shutdown() {
  }

  TieredStats GetStats() const {
    return {};
  }
};
}  // namespace dfly

#endif  // __linux__
