// Copyright 2023, Roman Gershman.  All rights reserved.
// See LICENSE for licensing terms.
//

#pragma once

#include <absl/functional/function_ref.h>

#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

extern "C" {
#include "redis/dict.h"
#include "redis/zset.h"
}

#include "core/bptree_set.h"
#include "core/score_map.h"

namespace dfly {

namespace detail {

template <typename... Ts> struct Overload : Ts... { using Ts::operator()...; };

template <typename... Ts> Overload(Ts...) -> Overload<Ts...>;

/**
 * @brief SortedMap is a sorted map implementation based on zset.h. It holds unique strings that
 * are ordered by score and lexicographically. The score is a double value and has higher priority.
 * The map is implemented as a skip list and a hash table. For more details see
 * zset.h and t_zset.c files in Redis.
 */
class SortedMap {
 public:
  using ScoredMember = std::pair<std::string, double>;
  using ScoredArray = std::vector<ScoredMember>;

  SortedMap(PMR_NS::memory_resource* res);
  SortedMap(const SortedMap&) = delete;
  SortedMap& operator=(const SortedMap&) = delete;

  ~SortedMap();

  // The ownership for the returned SortedMap stays with the caller, and must be freed via
  // placement delete and then res->deallocate().
  static SortedMap* FromListPack(PMR_NS::memory_resource* res, const uint8_t* lp);

  size_t Size() const {
    return std::visit(Overload{[](const auto& impl) { return impl.Size(); }}, impl_);
  }

  bool Reserve(size_t sz) {
    return std::visit(Overload{[&](auto& impl) { return impl.Reserve(sz); }}, impl_);
  }

  // Interface equivalent to zsetAdd.
  // Does not take ownership over ele string.
  // Returns 1 if succeeded, false if the final score became invalid due to the update.
  // newscore is set to the new score of the element only if in_flags contains ZADD_IN_INCR.
  int Add(double score, sds ele, int in_flags, int* out_flags, double* newscore) {
    return std::visit(
        Overload{[&](auto& impl) { return impl.Add(score, ele, in_flags, out_flags, newscore); }},
        impl_);
  }

  // Takes ownership over member.
  bool Insert(double score, sds member) {
    return std::visit(Overload{[&](auto& impl) { return impl.Insert(score, member); }}, impl_);
  }

  uint8_t* ToListPack() const {
    return std::visit(Overload{[](const auto& impl) { return impl.ToListPack(); }}, impl_);
  }

  size_t MallocSize() const {
    return std::visit(Overload{[](const auto& impl) { return impl.MallocSize(); }}, impl_);
  }

  uint64_t Scan(uint64_t cursor, absl::FunctionRef<void(std::string_view, double)> cb) const {
    return std::visit([&](const auto& impl) { return impl.Scan(cursor, cb); }, impl_);
  }

  size_t DeleteRangeByRank(unsigned start, unsigned end) {
    return std::visit(Overload{[&](auto& impl) { return impl.DeleteRangeByRank(start, end); }},
                      impl_);
  }

  size_t DeleteRangeByScore(const zrangespec& range) {
    return std::visit(Overload{[&](auto& impl) { return impl.DeleteRangeByScore(range); }}, impl_);
  }

  size_t DeleteRangeByLex(const zlexrangespec& range) {
    return std::visit(Overload{[&](auto& impl) { return impl.DeleteRangeByLex(range); }}, impl_);
  }

  // returns true if the element was deleted.
  bool Delete(sds ele) {
    return std::visit(Overload{[&](auto& impl) { return impl.Delete(ele); }}, impl_);
  }

  std::optional<double> GetScore(sds ele) const {
    return std::visit(Overload{[&](const auto& impl) { return impl.GetScore(ele); }}, impl_);
  }

  std::optional<unsigned> GetRank(sds ele, bool reverse) const {
    return std::visit(Overload{[&](const auto& impl) { return impl.GetRank(ele, reverse); }},
                      impl_);
  }

  ScoredArray GetRange(const zrangespec& range, unsigned offset, unsigned limit,
                       bool reverse) const {
    return std::visit(
        Overload{[&](const auto& impl) { return impl.GetRange(range, offset, limit, reverse); }},
        impl_);
  }

  ScoredArray GetLexRange(const zlexrangespec& range, unsigned offset, unsigned limit,
                          bool reverse) const {
    return std::visit(
        Overload{[&](const auto& impl) { return impl.GetLexRange(range, offset, limit, reverse); }},
        impl_);
  }

  ScoredArray PopTopScores(unsigned count, bool reverse) {
    return std::visit(Overload{[&](auto& impl) { return impl.PopTopScores(count, reverse); }},
                      impl_);
  }

  size_t Count(const zrangespec& range) const {
    return std::visit(Overload{[&](const auto& impl) { return impl.Count(range); }}, impl_);
  }

  size_t LexCount(const zlexrangespec& range) const {
    return std::visit(Overload{[&](const auto& impl) { return impl.LexCount(range); }}, impl_);
  }

  // Runs cb for each element in the range [start_rank, start_rank + len).
  // Stops iteration if cb returns false. Returns false in this case.
  bool Iterate(unsigned start_rank, unsigned len, bool reverse,
               absl::FunctionRef<bool(sds, double)> cb) const {
    return std::visit([&](const auto& impl) { return impl.Iterate(start_rank, len, reverse, cb); },
                      impl_);
  }

 private:
  struct RdImpl {
    struct dict* dict = nullptr;
    zskiplist* zsl = nullptr;

    int Add(double score, sds ele, int in_flags, int* out_flags, double* newscore);

    void Init();

    void Free() {
      dictRelease(dict);
      zslFree(zsl);
    }

    bool Insert(double score, sds member);

    bool Delete(sds ele);

    size_t Size() const {
      return zsl->length;
    }

    size_t MallocSize() const;

    bool Reserve(size_t sz) {
      return dictExpand(dict, sz) == DICT_OK;
    }

    size_t DeleteRangeByRank(unsigned start, unsigned end) {
      return zslDeleteRangeByRank(zsl, start + 1, end + 1, dict);
    }

    size_t DeleteRangeByScore(const zrangespec& range) {
      return zslDeleteRangeByScore(zsl, &range, dict);
    }

    size_t DeleteRangeByLex(const zlexrangespec& range) {
      return zslDeleteRangeByLex(zsl, &range, dict);
    }

    ScoredArray PopTopScores(unsigned count, bool reverse);

    uint8_t* ToListPack() const;

    std::optional<double> GetScore(sds ele) const;
    std::optional<unsigned> GetRank(sds ele, bool reverse) const;

    ScoredArray GetRange(const zrangespec& r, unsigned offs, unsigned len, bool rev) const;
    ScoredArray GetLexRange(const zlexrangespec& r, unsigned o, unsigned l, bool rev) const;

    size_t Count(const zrangespec& range) const;
    size_t LexCount(const zlexrangespec& range) const;

    // Runs cb for each element in the range [start_rank, start_rank + len).
    // Stops iteration if cb returns false. Returns false in this case.
    bool Iterate(unsigned start_rank, unsigned len, bool reverse,
                 absl::FunctionRef<bool(sds, double)> cb) const;

    uint64_t Scan(uint64_t cursor, absl::FunctionRef<void(std::string_view, double)> cb) const;
  };

  struct DfImpl {
    ScoreMap* score_map = nullptr;
    using ScoreSds = void*;

    struct ScoreSdsPolicy {
      using KeyT = ScoreSds;

      struct KeyCompareTo {
        int operator()(KeyT a, KeyT b) const;
      };
    };

    using ScoreTree = BPTree<ScoreSds, ScoreSdsPolicy>;
    ScoreTree* score_tree = nullptr;  // just a stub for now.

    void Init(PMR_NS::memory_resource* mr);

    void Free();

    int Add(double score, sds ele, int in_flags, int* out_flags, double* newscore);

    bool Insert(double score, sds member);

    bool Delete(sds ele);

    size_t Size() const {
      return score_map->UpperBoundSize();
    }

    size_t MallocSize() const;

    bool Reserve(size_t sz);

    size_t DeleteRangeByRank(unsigned start, unsigned end);

    size_t DeleteRangeByScore(const zrangespec& range);

    size_t DeleteRangeByLex(const zlexrangespec& range);

    ScoredArray PopTopScores(unsigned count, bool reverse);

    uint8_t* ToListPack() const;

    std::optional<double> GetScore(sds ele) const;
    std::optional<unsigned> GetRank(sds ele, bool reverse) const;

    ScoredArray GetRange(const zrangespec& r, unsigned offs, unsigned len, bool rev) const;
    ScoredArray GetLexRange(const zlexrangespec& r, unsigned o, unsigned l, bool rev) const;

    size_t Count(const zrangespec& range) const;
    size_t LexCount(const zlexrangespec& range) const;

    // Runs cb for each element in the range [start_rank, start_rank + len).
    // Stops iteration if cb returns false. Returns false in this case.
    bool Iterate(unsigned start_rank, unsigned len, bool reverse,
                 absl::FunctionRef<bool(sds, double)> cb) const;

    uint64_t Scan(uint64_t cursor, absl::FunctionRef<void(std::string_view, double)> cb) const;
  };

  std::variant<RdImpl, DfImpl> impl_;
};

// Used by CompactObject.
unsigned char* ZzlInsert(unsigned char* zl, sds ele, double score);

}  // namespace detail
}  // namespace dfly
