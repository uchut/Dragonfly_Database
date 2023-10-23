// Copyright 2022, DragonflyDB authors.  All rights reserved.
// See LICENSE for licensing terms.
//

#include "server/memory_cmd.h"

#include <absl/strings/str_cat.h>
#include <mimalloc.h>

#include "facade/error.h"
#include "server/engine_shard_set.h"
#include "server/server_state.h"

using namespace std;
using namespace facade;

namespace dfly {

namespace {

void MiStatsCallback(const char* msg, void* arg) {
  string* str = (string*)arg;
  absl::StrAppend(str, msg);
}

// blocksize, reserved, committed, used.
using BlockKey = std::tuple<size_t, size_t, size_t, size_t>;
using BlockMap = absl::flat_hash_map<BlockKey, uint64_t>;

bool MiArenaVisit(const mi_heap_t* heap, const mi_heap_area_t* area, void* block, size_t block_size,
                  void* arg) {
  BlockMap* bmap = (BlockMap*)arg;
  BlockKey bkey{block_size, area->reserved, area->committed, area->used * block_size};
  (*bmap)[bkey]++;

  return true;
};

std::string MallocStats(bool backing, unsigned tid) {
  string str;

  uint64_t start = absl::GetCurrentTimeNanos();
  absl::StrAppend(&str, "___ Begin mimalloc statistics ___\n");
  mi_stats_print_out(MiStatsCallback, &str);

  absl::StrAppend(&str, "\nArena statistics from thread:", tid, "\n");
  absl::StrAppend(&str, "Count BlockSize Reserved Committed Used\n");

  mi_heap_t* data_heap = backing ? mi_heap_get_backing() : ServerState::tlocal()->data_heap();
  BlockMap block_map;

  mi_heap_visit_blocks(data_heap, false /* visit all blocks*/, MiArenaVisit, &block_map);
  uint64_t reserved = 0, committed = 0, used = 0;
  for (const auto& k_v : block_map) {
    uint64_t count = k_v.second;
    absl::StrAppend(&str, count, " ", get<0>(k_v.first), " ", get<1>(k_v.first), " ",
                    get<2>(k_v.first), " ", get<3>(k_v.first), "\n");
    reserved += count * get<1>(k_v.first);
    committed += count * get<2>(k_v.first);
    used += count * get<3>(k_v.first);
  }

  uint64_t delta = (absl::GetCurrentTimeNanos() - start) / 1000;
  absl::StrAppend(&str, "--- End mimalloc statistics, took ", delta, "us ---\n");
  absl::StrAppend(&str, "total reserved: ", reserved, ", comitted: ", committed, ", used: ", used,
                  " fragmentation waste: ",
                  (100.0 * (committed - used)) / std::max<size_t>(1UL, committed), "%\n");

  return str;
}

size_t MemoryUsage(PrimeIterator it) {
  return it->first.MallocUsed() + it->second.MallocUsed();
}

}  // namespace

MemoryCmd::MemoryCmd(ServerFamily* owner, ConnectionContext* cntx) : cntx_(cntx) {
}

void MemoryCmd::Run(CmdArgList args) {
  string_view sub_cmd = ArgS(args, 0);

  if (sub_cmd == "HELP") {
    string_view help_arr[] = {
        "MEMORY <subcommand> [<arg> ...]. Subcommands are:",
        "MALLOC-STATS [BACKING] [thread-id]",
        "    Show malloc stats for a heap residing in specified thread-id. 0 by default.",
        "    If BACKING is specified, show stats for the backing heap.",
        "USAGE <key>",
        "    Show memory usage of a key.",
        "DECOMMIT",
        "    Force decommit the memory freed by the server back to OS.",
    };
    return (*cntx_)->SendSimpleStrArr(help_arr);
  };

  if (sub_cmd == "USAGE" && args.size() > 1) {
    string_view key = ArgS(args, 1);
    return Usage(key);
  }

  if (sub_cmd == "DECOMMIT") {
    shard_set->pool()->Await([](auto* pb) {
      mi_heap_collect(ServerState::tlocal()->data_heap(), true);
      mi_heap_collect(mi_heap_get_backing(), true);
    });
    return (*cntx_)->SendSimpleString("OK");
  }

  if (sub_cmd == "MALLOC-STATS") {
    uint32_t tid = 0;
    bool backing = false;
    if (args.size() >= 2) {
      ToUpper(&args[1]);

      unsigned tid_indx = 1;
      if (ArgS(args, tid_indx) == "BACKING") {
        ++tid_indx;
        backing = true;
      }

      if (args.size() > tid_indx && !absl::SimpleAtoi(ArgS(args, tid_indx), &tid)) {
        return (*cntx_)->SendError(kInvalidIntErr);
      }
    }

    if (backing && tid >= shard_set->pool()->size()) {
      return cntx_->SendError(
          absl::StrCat("Thread id must be less than ", shard_set->pool()->size()));
    }

    if (!backing && tid >= shard_set->size()) {
      return cntx_->SendError(absl::StrCat("Thread id must be less than ", shard_set->size()));
    }

    string res = shard_set->pool()->at(tid)->AwaitBrief([=] { return MallocStats(backing, tid); });

    return (*cntx_)->SendBulkString(res);
  }

  string err = UnknownSubCmd(sub_cmd, "MEMORY");
  return (*cntx_)->SendError(err, kSyntaxErrType);
}

void MemoryCmd::Usage(std::string_view key) {
  ShardId sid = Shard(key, shard_set->size());
  ssize_t memory_usage = shard_set->pool()->at(sid)->AwaitBrief([key, this]() -> ssize_t {
    auto& db_slice = EngineShard::tlocal()->db_slice();
    auto [pt, exp_t] = db_slice.GetTables(cntx_->db_index());
    PrimeIterator it = pt->Find(key);
    if (IsValid(it)) {
      return MemoryUsage(it);
    } else {
      return -1;
    }
  });

  if (memory_usage < 0)
    return cntx_->SendError(kKeyNotFoundErr);
  (*cntx_)->SendLong(memory_usage);
}

}  // namespace dfly
