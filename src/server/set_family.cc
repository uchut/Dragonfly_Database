// Copyright 2022, DragonflyDB authors.  All rights reserved.
// See LICENSE for licensing terms.
//

#include "server/set_family.h"

extern "C" {
#include "redis/intset.h"
#include "redis/redis_aux.h"
#include "redis/util.h"
}

#include "base/flags.h"
#include "base/logging.h"
#include "base/stl_util.h"
#include "core/string_set.h"
#include "facade/cmd_arg_parser.h"
#include "server/acl/acl_commands_def.h"
#include "server/command_registry.h"
#include "server/conn_context.h"
#include "server/container_utils.h"
#include "server/engine_shard_set.h"
#include "server/error.h"
#include "server/journal/journal.h"
#include "server/transaction.h"

ABSL_DECLARE_FLAG(bool, use_set2);

namespace dfly {

using namespace facade;

using namespace std;
using absl::GetFlag;

using ResultStringVec = vector<OpResult<StringVec>>;
using ResultSetView = OpResult<absl::flat_hash_set<std::string_view>>;
using SvArray = vector<std::string_view>;
using SetType = pair<void*, unsigned>;

namespace {

constexpr uint32_t kMaxIntSetEntries = 256;

bool IsDenseEncoding(const CompactObj& co) {
  return co.Encoding() == kEncodingStrMap2;
}

intset* IntsetAddSafe(string_view val, intset* is, bool* success, bool* added) {
  long long llval;
  *added = false;
  if (!string2ll(val.data(), val.size(), &llval)) {
    *success = false;
    return is;
  }

  uint8_t inserted = 0;
  is = intsetAdd(is, llval, &inserted);
  if (inserted) {
    *added = true;
    *success = intsetLen(is) <= kMaxIntSetEntries;
  } else {
    *added = false;
    *success = true;
  }

  return is;
}

pair<unsigned, bool> RemoveStrSet(uint32_t now_sec, ArgSlice vals, CompactObj* set) {
  unsigned removed = 0;
  bool isempty = false;
  DCHECK(IsDenseEncoding(*set));

  if (true) {
    StringSet* ss = ((StringSet*)set->RObjPtr());
    ss->set_time(now_sec);

    for (auto member : vals) {
      removed += ss->Erase(member);
    }

    isempty = ss->Empty();
  }

  return make_pair(removed, isempty);
}

unsigned AddStrSet(const DbContext& db_context, ArgSlice vals, uint32_t ttl_sec, CompactObj* dest) {
  unsigned res = 0;
  DCHECK(IsDenseEncoding(*dest));

  if (true) {
    StringSet* ss = (StringSet*)dest->RObjPtr();
    uint32_t time_now = MemberTimeSeconds(db_context.time_now_ms);

    ss->set_time(time_now);

    for (auto member : vals) {
      res += ss->Add(member, ttl_sec);
    }
  }

  return res;
}

void InitStrSet(CompactObj* set) {
  set->InitRobj(OBJ_SET, kEncodingStrMap2, CompactObj::AllocateMR<StringSet>());
}

// returns (removed, isempty)
pair<unsigned, bool> RemoveSet(const DbContext& db_context, ArgSlice vals, CompactObj* set) {
  bool isempty = false;
  unsigned removed = 0;

  if (set->Encoding() == kEncodingIntSet) {
    intset* is = (intset*)set->RObjPtr();
    long long llval;

    for (auto val : vals) {
      if (!string2ll(val.data(), val.size(), &llval)) {
        continue;
      }

      int is_removed = 0;
      is = intsetRemove(is, llval, &is_removed);
      removed += is_removed;
    }
    isempty = (intsetLen(is) == 0);
    set->SetRObjPtr(is);
  } else {
    return RemoveStrSet(MemberTimeSeconds(db_context.time_now_ms), vals, set);
  }
  return make_pair(removed, isempty);
}

void InitSet(ArgSlice vals, CompactObj* set) {
  bool int_set = true;
  long long intv;

  for (auto v : vals) {
    if (!string2ll(v.data(), v.size(), &intv)) {
      int_set = false;
      break;
    }
  }

  if (int_set) {
    intset* is = intsetNew();
    set->InitRobj(OBJ_SET, kEncodingIntSet, is);
  } else {
    InitStrSet(set);
  }
}

uint64_t ScanStrSet(const DbContext& db_context, const CompactObj& co, uint64_t curs,
                    const ScanOpts& scan_op, StringVec* res) {
  uint32_t count = scan_op.limit;
  long maxiterations = count * 10;
  DCHECK(IsDenseEncoding(co));

  if (true) {
    StringSet* set = (StringSet*)co.RObjPtr();
    set->set_time(MemberTimeSeconds(db_context.time_now_ms));

    do {
      auto scan_callback = [&](const sds ptr) {
        string_view str{ptr, sdslen(ptr)};
        if (scan_op.Matches(str)) {
          res->push_back(std::string(str));
        }
      };

      curs = set->Scan(curs, scan_callback);

    } while (curs && maxiterations-- && res->size() < count);
  }
  return curs;
}

uint32_t SetTypeLen(const DbContext& db_context, const SetType& set) {
  if (set.second == kEncodingIntSet) {
    return intsetLen((const intset*)set.first);
  }

  if (true) {
    StringSet* ss = (StringSet*)set.first;
    ss->set_time(MemberTimeSeconds(db_context.time_now_ms));
    return ss->UpperBoundSize();
  }
}

bool IsInSet(const DbContext& db_context, const SetType& st, int64_t val) {
  if (st.second == kEncodingIntSet)
    return intsetFind((intset*)st.first, val);

  char buf[32];
  char* next = absl::numbers_internal::FastIntToBuffer(val, buf);
  string_view str{buf, size_t(next - buf)};

  if (true) {
    StringSet* ss = (StringSet*)st.first;
    ss->set_time(MemberTimeSeconds(db_context.time_now_ms));
    return ss->Contains(str);
  }
}

bool IsInSet(const DbContext& db_context, const SetType& st, string_view member) {
  if (st.second == kEncodingIntSet) {
    long long llval;
    if (!string2ll(member.data(), member.size(), &llval))
      return false;

    return intsetFind((intset*)st.first, llval);
  }

  if (true) {
    StringSet* ss = (StringSet*)st.first;
    ss->set_time(MemberTimeSeconds(db_context.time_now_ms));

    return ss->Contains(member);
  }
}

// returns -3 if member is not found, -1 if no ttl is associated with this member.
int32_t GetExpiry(const DbContext& db_context, const SetType& st, string_view member) {
  if (st.second == kEncodingIntSet) {
    long long llval;
    if (!string2ll(member.data(), member.size(), &llval))
      return -3;

    return -1;
  }

  if (true) {
    StringSet* ss = (StringSet*)st.first;
    ss->set_time(MemberTimeSeconds(db_context.time_now_ms));

    auto it = ss->Find(member);
    if (it == ss->end())
      return -3;

    return it.HasExpiry() ? it.ExpiryTime() : -1;
  }
}

void FindInSet(StringVec& memberships, const DbContext& db_context, const SetType& st,
               const vector<string_view>& members) {
  for (const auto& member : members) {
    bool status = IsInSet(db_context, st, member);
    memberships.emplace_back(to_string(status));
  }
}

// Removes arg from result.
void DiffStrSet(const DbContext& db_context, const SetType& st,
                absl::flat_hash_set<string>* result) {
  if (true) {
    StringSet* ss = (StringSet*)st.first;
    ss->set_time(MemberTimeSeconds(db_context.time_now_ms));
    for (sds ptr : *ss) {
      result->erase(string_view{ptr, sdslen(ptr)});
    }
  }
}

void InterStrSet(const DbContext& db_context, const vector<SetType>& vec, StringVec* result) {
  if (true) {
    StringSet* ss = (StringSet*)vec.front().first;
    ss->set_time(MemberTimeSeconds(db_context.time_now_ms));
    for (const sds ptr : *ss) {
      std::string_view str{ptr, sdslen(ptr)};
      size_t j = 1;
      for (j = 1; j < vec.size(); ++j) {
        if (vec[j].first != ss && !IsInSet(db_context, vec[j], str)) {
          break;
        }
      }

      if (j == vec.size()) {
        result->push_back(std::string(str));
      }
    }
  }
}

StringVec PopStrSet(const DbContext& db_context, unsigned count, const SetType& st) {
  StringVec result;

  if (true) {
    StringSet* ss = (StringSet*)st.first;
    ss->set_time(MemberTimeSeconds(db_context.time_now_ms));

    // TODO: this loop is inefficient because Pop searches again and again an occupied bucket.
    for (unsigned i = 0; i < count && !ss->Empty(); ++i) {
      result.push_back(ss->Pop().value());
    }
  }

  return result;
}

vector<string> ToVec(absl::flat_hash_set<string>&& set) {
  vector<string> result(set.size());
  size_t i = 0;

  // extract invalidates current iterator. therefore, we increment it first before extracting.
  // hence the weird loop.
  for (auto it = set.begin(); it != set.end();) {
    result[i] = std::move(set.extract(it++).value());
    ++i;
  }

  return result;
}

ResultSetView UnionResultVec(const ResultStringVec& result_vec) {
  absl::flat_hash_set<std::string_view> uniques;

  for (const auto& val : result_vec) {
    if (val || val.status() == OpStatus::SKIPPED) {
      for (const string& s : val.value()) {
        uniques.emplace(s);
      }
      continue;
    }

    if (val.status() != OpStatus::KEY_NOTFOUND) {
      return val.status();
    }
  }

  return uniques;
}

ResultSetView DiffResultVec(const ResultStringVec& result_vec, ShardId src_shard) {
  for (const auto& res : result_vec) {
    if (res.status() == OpStatus::WRONG_TYPE)
      return res.status();
  }

  absl::flat_hash_set<std::string_view> uniques;

  for (const auto& val : result_vec[src_shard].value()) {
    uniques.emplace(val);
  }

  for (unsigned i = 0; i < result_vec.size(); ++i) {
    if (i == src_shard)
      continue;

    if (result_vec[i]) {
      for (const string& s : result_vec[i].value()) {
        uniques.erase(s);
      }
    }
  }
  return uniques;
}

OpResult<SvArray> InterResultVec(const ResultStringVec& result_vec, unsigned required_shard_cnt,
                                 unsigned limit = 0) {
  absl::flat_hash_map<std::string_view, unsigned> uniques;

  for (const auto& res : result_vec) {
    if (!res && !base::_in(res.status(), {OpStatus::SKIPPED, OpStatus::KEY_NOTFOUND}))
      return res.status();
  }

  for (const auto& res : result_vec) {
    if (res.status() == OpStatus::KEY_NOTFOUND)
      return OpStatus::OK;  // empty set.
  }

  bool first = true;
  for (const auto& res : result_vec) {
    if (res.status() == OpStatus::SKIPPED)
      continue;

    DCHECK(res);  // we handled it above.

    // I use this awkward 'first' condition instead of table[s]++ deliberately.
    // I do not want to add keys that I know will not stay in the set.
    if (first) {
      for (const string& s : res.value()) {
        uniques.emplace(s, 1);
      }
      first = false;
    } else {
      for (const string& s : res.value()) {
        auto it = uniques.find(s);
        if (it != uniques.end()) {
          ++it->second;
        }
      }
    }
  }

  SvArray result;
  result.reserve(uniques.size());

  for (const auto& k_v : uniques) {
    if (k_v.second == required_shard_cnt) {
      if (limit != 0 && result.size() >= limit)
        return result;
      result.push_back(k_v.first);
    }
  }

  return result;
}

SvArray ToSvArray(const absl::flat_hash_set<std::string_view>& set) {
  SvArray result;
  result.reserve(set.size());
  copy(set.begin(), set.end(), back_inserter(result));
  return result;
}

// if overwrite is true then OpAdd writes vals into the key and discards its previous value.
OpResult<uint32_t> OpAdd(const OpArgs& op_args, std::string_view key, ArgSlice vals, bool overwrite,
                         bool journal_update) {
  auto* es = op_args.shard;
  auto& db_slice = es->db_slice();

  VLOG(2) << "OpAdd(" << key << ")";

  // overwrite - meaning we run in the context of 2-hop operation and we want
  // to overwrite the key. However, if the set is empty it means we should delete the
  // key if it exists.
  if (overwrite && vals.empty()) {
    auto it = db_slice.FindMutable(op_args.db_cntx, key).it;  // post_updater will run immediately
    db_slice.Del(op_args.db_cntx.db_index, it);
    if (journal_update && op_args.shard->journal()) {
      RecordJournal(op_args, "DEL"sv, ArgSlice{key});
    }
    return 0;
  }

  auto op_res = db_slice.AddOrFind(op_args.db_cntx, key);
  RETURN_ON_BAD_STATUS(op_res);
  auto& add_res = *op_res;

  CompactObj& co = add_res.it->second;

  if (!add_res.is_new) {
    // for non-overwrite case it must be set.
    if (!overwrite && co.ObjType() != OBJ_SET)
      return OpStatus::WRONG_TYPE;
  }

  if (add_res.is_new || overwrite) {
    // does not store the values, merely sets the encoding.
    // TODO: why not store the values as well?
    InitSet(vals, &co);
  }

  void* inner_obj = co.RObjPtr();
  uint32_t res = 0;

  if (co.Encoding() == kEncodingIntSet) {
    intset* is = (intset*)inner_obj;
    bool success = true;

    for (auto val : vals) {
      bool added = false;
      is = IntsetAddSafe(val, is, &success, &added);
      res += added;

      if (!success) {
        co.SetRObjPtr(is);

        StringSet* ss = SetFamily::ConvertToStrSet(is, intsetLen(is));
        if (!ss) {
          return OpStatus::OUT_OF_MEMORY;
        }

        // frees 'is' on a way.
        co.InitRobj(OBJ_SET, kEncodingStrMap2, ss);
        inner_obj = co.RObjPtr();
        break;
      }
    }

    if (success)
      co.SetRObjPtr(is);
  }

  if (co.Encoding() != kEncodingIntSet) {
    res = AddStrSet(op_args.db_cntx, vals, UINT32_MAX, &co);
  }

  if (journal_update && op_args.shard->journal()) {
    if (overwrite) {
      RecordJournal(op_args, "DEL"sv, ArgSlice{key});
    }
    vector<string_view> mapped(vals.size() + 1);
    mapped[0] = key;
    std::copy(vals.begin(), vals.end(), mapped.begin() + 1);
    RecordJournal(op_args, "SADD"sv, mapped);
  }
  return res;
}

OpResult<uint32_t> OpAddEx(const OpArgs& op_args, string_view key, uint32_t ttl_sec,
                           ArgSlice vals) {
  auto* es = op_args.shard;
  auto& db_slice = es->db_slice();

  auto op_res = db_slice.AddOrFind(op_args.db_cntx, key);
  RETURN_ON_BAD_STATUS(op_res);
  auto& add_res = *op_res;

  CompactObj& co = add_res.it->second;

  if (add_res.is_new) {
    InitStrSet(&co);
  } else {
    // for non-overwrite case it must be set.
    if (co.ObjType() != OBJ_SET)
      return OpStatus::WRONG_TYPE;

    // Update stats and trigger any handle the old value if needed.
    if (co.Encoding() == kEncodingIntSet) {
      intset* is = (intset*)co.RObjPtr();
      StringSet* ss = SetFamily::ConvertToStrSet(is, intsetLen(is));
      if (!ss) {
        return OpStatus::OUT_OF_MEMORY;
      }
      co.InitRobj(OBJ_SET, kEncodingStrMap2, ss);
    }

    CHECK(IsDenseEncoding(co));
  }

  uint32_t res = AddStrSet(op_args.db_cntx, std::move(vals), ttl_sec, &co);

  return res;
}

OpResult<uint32_t> OpRem(const OpArgs& op_args, string_view key, const ArgSlice& vals,
                         bool journal_rewrite) {
  auto* es = op_args.shard;
  auto& db_slice = es->db_slice();
  auto find_res = db_slice.FindMutable(op_args.db_cntx, key, OBJ_SET);
  if (!find_res) {
    return find_res.status();
  }

  CompactObj& co = find_res->it->second;
  auto [removed, isempty] = RemoveSet(op_args.db_cntx, vals, &co);

  find_res->post_updater.Run();

  if (isempty) {
    CHECK(db_slice.Del(op_args.db_cntx.db_index, find_res->it));
  }
  if (journal_rewrite && op_args.shard->journal()) {
    vector<string_view> mapped(vals.size() + 1);
    mapped[0] = key;
    std::copy(vals.begin(), vals.end(), mapped.begin() + 1);
    RecordJournal(op_args, "SREM"sv, mapped);
  }

  return removed;
}

// For SMOVE. Comprised of 2 transactional steps: Find and Commit.
// After Find Mover decides on the outcome of the operation, applies it in commit
// and reports the result.
class Mover {
 public:
  Mover(string_view src, string_view dest, string_view member, bool journal_rewrite)
      : src_(src), dest_(dest), member_(member), journal_rewrite_(journal_rewrite) {
  }

  void Find(Transaction* t);
  OpResult<unsigned> Commit(Transaction* t);

 private:
  OpStatus OpFind(Transaction* t, EngineShard* es);
  OpStatus OpMutate(Transaction* t, EngineShard* es);

  string_view src_, dest_, member_;
  OpResult<bool> found_[2];
  bool journal_rewrite_;
};

OpStatus Mover::OpFind(Transaction* t, EngineShard* es) {
  ShardArgs largs = t->GetShardArgs(es->shard_id());

  // In case both src and dest are in the same shard, largs size will be 2.
  DCHECK_LE(largs.Size(), 2u);

  for (auto k : largs) {
    unsigned index = (k == src_) ? 0 : 1;
    auto res = es->db_slice().FindReadOnly(t->GetDbContext(), k, OBJ_SET);
    if (res && index == 0) {  // successful src find.
      DCHECK(!res->is_done());
      const CompactObj& val = res.value()->second;
      SetType st{val.RObjPtr(), val.Encoding()};
      found_[0] = IsInSet(t->GetDbContext(), st, member_);
    } else {
      found_[index] = res.status();
    }
  }

  return OpStatus::OK;
}

OpStatus Mover::OpMutate(Transaction* t, EngineShard* es) {
  ShardArgs largs = t->GetShardArgs(es->shard_id());
  DCHECK_LE(largs.Size(), 2u);

  OpArgs op_args = t->GetOpArgs(es);
  for (auto k : largs) {
    if (k == src_) {
      CHECK_EQ(1u, OpRem(op_args, k, {member_}, journal_rewrite_).value());  // must succeed.
    } else {
      DCHECK_EQ(k, dest_);
      OpAdd(op_args, k, {member_}, false, journal_rewrite_);
    }
  }

  return OpStatus::OK;
}

void Mover::Find(Transaction* t) {
  // non-concluding step.
  t->Execute([this](Transaction* t, EngineShard* es) { return this->OpFind(t, es); }, false);
}

OpResult<unsigned> Mover::Commit(Transaction* t) {
  OpResult<unsigned> res;
  bool noop = false;

  if (found_[0].status() == OpStatus::WRONG_TYPE || found_[1].status() == OpStatus::WRONG_TYPE) {
    res = OpStatus::WRONG_TYPE;
    noop = true;
  } else if (!found_[0].value_or(false)) {
    res = 0;
    noop = true;
  } else {
    res = 1;
    noop = (src_ == dest_);
  }

  if (noop) {
    t->Conclude();
  } else {
    t->Execute([this](Transaction* t, EngineShard* es) { return this->OpMutate(t, es); }, true);
  }

  return res;
}

// Read-only OpUnion op on sets.
OpResult<StringVec> OpUnion(const OpArgs& op_args, ShardArgs::Iterator start,
                            ShardArgs::Iterator end) {
  DCHECK(start != end);
  absl::flat_hash_set<string> uniques;

  for (; start != end; ++start) {
    auto find_res = op_args.shard->db_slice().FindReadOnly(op_args.db_cntx, *start, OBJ_SET);
    if (find_res) {
      const PrimeValue& pv = find_res.value()->second;
      if (IsDenseEncoding(pv)) {
        StringSet* ss = (StringSet*)pv.RObjPtr();
        ss->set_time(MemberTimeSeconds(op_args.db_cntx.time_now_ms));
      }
      container_utils::IterateSet(pv, [&uniques](container_utils::ContainerEntry ce) {
        uniques.emplace(ce.ToString());
        return true;
      });
      continue;
    }

    if (find_res.status() != OpStatus::KEY_NOTFOUND) {
      return find_res.status();
    }
  }

  return ToVec(std::move(uniques));
}

// Read-only OpDiff op on sets.
OpResult<StringVec> OpDiff(const OpArgs& op_args, ShardArgs::Iterator start,
                           ShardArgs::Iterator end) {
  DCHECK(start != end);
  DVLOG(1) << "OpDiff from " << *start;
  EngineShard* es = op_args.shard;
  auto find_res = es->db_slice().FindReadOnly(op_args.db_cntx, *start, OBJ_SET);

  if (!find_res) {
    return find_res.status();
  }

  absl::flat_hash_set<string> uniques;
  const PrimeValue& pv = find_res.value()->second;
  if (IsDenseEncoding(pv)) {
    StringSet* ss = (StringSet*)pv.RObjPtr();
    ss->set_time(MemberTimeSeconds(op_args.db_cntx.time_now_ms));
  }

  container_utils::IterateSet(pv, [&uniques](container_utils::ContainerEntry ce) {
    uniques.emplace(ce.ToString());
    return true;
  });

  DCHECK(!uniques.empty());  // otherwise the key would not exist.

  for (++start; start != end; ++start) {
    auto diff_res = es->db_slice().FindReadOnly(op_args.db_cntx, *start, OBJ_SET);
    if (!diff_res) {
      if (diff_res.status() == OpStatus::WRONG_TYPE) {
        return OpStatus::WRONG_TYPE;
      }
      continue;  // KEY_NOTFOUND
    }

    SetType st2{diff_res.value()->second.RObjPtr(), diff_res.value()->second.Encoding()};
    if (st2.second == kEncodingIntSet) {
      int ii = 0;
      intset* is = (intset*)st2.first;
      int64_t intele;
      char buf[32];

      while (intsetGet(is, ii++, &intele)) {
        char* next = absl::numbers_internal::FastIntToBuffer(intele, buf);
        uniques.erase(string_view{buf, size_t(next - buf)});
      }
    } else {
      DiffStrSet(op_args.db_cntx, st2, &uniques);
    }
  }

  return ToVec(std::move(uniques));
}

// Read-only OpInter op on sets.
OpResult<StringVec> OpInter(const Transaction* t, EngineShard* es, bool remove_first) {
  ShardArgs args = t->GetShardArgs(es->shard_id());
  auto it = args.begin();
  if (remove_first) {
    ++it;
  }
  DCHECK(it != args.end());

  StringVec result;
  if (args.Size() == 1 + unsigned(remove_first)) {
    auto find_res = es->db_slice().FindReadOnly(t->GetDbContext(), *it, OBJ_SET);
    if (!find_res)
      return find_res.status();

    const PrimeValue& pv = find_res.value()->second;
    if (IsDenseEncoding(pv)) {
      StringSet* ss = (StringSet*)pv.RObjPtr();
      ss->set_time(MemberTimeSeconds(t->GetDbContext().time_now_ms));
    }

    container_utils::IterateSet(find_res.value()->second,
                                [&result](container_utils::ContainerEntry ce) {
                                  result.push_back(ce.ToString());
                                  return true;
                                });
    return result;
  }

  vector<SetType> sets(args.Size() - int(remove_first));

  OpStatus status = OpStatus::OK;
  unsigned index = 0;
  for (; it != args.end(); ++it) {
    auto& dest = sets[index++];
    auto find_res = es->db_slice().FindReadOnly(t->GetDbContext(), *it, OBJ_SET);
    if (!find_res) {
      if (status == OpStatus::OK || status == OpStatus::KEY_NOTFOUND ||
          find_res.status() != OpStatus::KEY_NOTFOUND) {
        status = find_res.status();
      }
      continue;
    }
    const PrimeValue& pv = find_res.value()->second;
    void* ptr = pv.RObjPtr();
    dest = make_pair(ptr, pv.Encoding());
  }

  if (status != OpStatus::OK)
    return status;

  auto comp = [db_contx = t->GetDbContext()](const SetType& left, const SetType& right) {
    return SetTypeLen(db_contx, left) < SetTypeLen(db_contx, right);
  };

  std::sort(sets.begin(), sets.end(), comp);

  int encoding = sets.front().second;
  if (encoding == kEncodingIntSet) {
    int ii = 0;
    intset* is = (intset*)sets.front().first;
    int64_t intele;

    while (intsetGet(is, ii++, &intele)) {
      size_t j = 1;
      for (j = 1; j < sets.size(); j++) {
        if (sets[j].first != is && !IsInSet(t->GetDbContext(), sets[j], intele))
          break;
      }

      /* Only take action when all sets contain the member */
      if (j == sets.size()) {
        result.push_back(absl::StrCat(intele));
      }
    }
  } else {
    InterStrSet(t->GetDbContext(), sets, &result);
  }

  return result;
}

// count - how many elements to pop.
OpResult<StringVec> OpPop(const OpArgs& op_args, string_view key, unsigned count) {
  auto& db_slice = op_args.shard->db_slice();
  auto find_res = db_slice.FindMutable(op_args.db_cntx, key, OBJ_SET);
  if (!find_res)
    return find_res.status();

  StringVec result;
  if (count == 0)
    return result;

  auto it = find_res->it;
  size_t slen = it->second.Size();

  /* CASE 1:
   * The number of requested elements is greater than or equal to
   * the number of elements inside the set: simply return the whole set. */
  if (count >= slen) {
    PrimeValue& pv = it->second;
    if (IsDenseEncoding(pv)) {
      StringSet* ss = (StringSet*)pv.RObjPtr();
      ss->set_time(MemberTimeSeconds(op_args.db_cntx.time_now_ms));
    }

    container_utils::IterateSet(it->second, [&result](container_utils::ContainerEntry ce) {
      result.push_back(ce.ToString());
      return true;
    });

    // Delete the set as it is now empty
    find_res->post_updater.Run();
    CHECK(db_slice.Del(op_args.db_cntx.db_index, it));

    // Replicate as DEL.
    if (op_args.shard->journal()) {
      RecordJournal(op_args, "DEL"sv, ArgSlice{key});
    }
  } else {
    SetType st{it->second.RObjPtr(), it->second.Encoding()};
    if (st.second == kEncodingIntSet) {
      intset* is = (intset*)st.first;
      int64_t val = 0;

      // copy last count values.
      for (uint32_t i = slen - count; i < slen; ++i) {
        intsetGet(is, i, &val);
        result.push_back(absl::StrCat(val));
      }

      is = intsetTrimTail(is, count);  // now remove last count items
      it->second.SetRObjPtr(is);
    } else {
      result = PopStrSet(op_args.db_cntx, count, st);
    }

    // Replicate as SREM with removed keys, because SPOP is not deterministic.
    if (op_args.shard->journal()) {
      vector<string_view> mapped(result.size() + 1);
      mapped[0] = key;
      std::copy(result.begin(), result.end(), mapped.begin() + 1);
      RecordJournal(op_args, "SREM"sv, mapped);
    }
  }
  return result;
}

OpResult<StringVec> OpScan(const OpArgs& op_args, string_view key, uint64_t* cursor,
                           const ScanOpts& scan_op) {
  auto find_res = op_args.shard->db_slice().FindReadOnly(op_args.db_cntx, key, OBJ_SET);

  if (!find_res)
    return find_res.status();

  auto it = find_res.value();
  StringVec res;

  if (it->second.Encoding() == kEncodingIntSet) {
    intset* is = (intset*)it->second.RObjPtr();
    int64_t intele;
    uint32_t pos = 0;
    while (intsetGet(is, pos++, &intele)) {
      std::string int_str = absl::StrCat(intele);
      if (scan_op.Matches(int_str)) {
        res.push_back(int_str);
      }
    }
    *cursor = 0;
  } else {
    *cursor = ScanStrSet(op_args.db_cntx, it->second, *cursor, scan_op, &res);
  }

  return res;
}

void SAdd(CmdArgList args, ConnectionContext* cntx) {
  string_view key = ArgS(args, 0);
  vector<string_view> vals(args.size() - 1);
  for (size_t i = 1; i < args.size(); ++i) {
    vals[i - 1] = ArgS(args, i);
  }
  ArgSlice arg_slice{vals.data(), vals.size()};

  auto cb = [&](Transaction* t, EngineShard* shard) {
    return OpAdd(t->GetOpArgs(shard), key, arg_slice, false, false);
  };

  OpResult<uint32_t> result = cntx->transaction->ScheduleSingleHopT(std::move(cb));
  if (result) {
    return cntx->SendLong(result.value());
  }

  cntx->SendError(result.status());
}

void SIsMember(CmdArgList args, ConnectionContext* cntx) {
  string_view key = ArgS(args, 0);
  string_view val = ArgS(args, 1);

  auto cb = [&](Transaction* t, EngineShard* shard) {
    auto find_res = shard->db_slice().FindReadOnly(t->GetDbContext(), key, OBJ_SET);

    if (find_res) {
      SetType st{find_res.value()->second.RObjPtr(), find_res.value()->second.Encoding()};
      return IsInSet(t->GetDbContext(), st, val) ? OpStatus::OK : OpStatus::KEY_NOTFOUND;
    }

    return find_res.status();
  };

  OpResult<void> result = cntx->transaction->ScheduleSingleHop(std::move(cb));
  switch (result.status()) {
    case OpStatus::OK:
      return cntx->SendLong(1);
    default:
      return cntx->SendLong(0);
  }
}

void SMIsMember(CmdArgList args, ConnectionContext* cntx) {
  string_view key = ArgS(args, 0);

  vector<string_view> vals(args.size() - 1);
  for (size_t i = 1; i < args.size(); ++i) {
    vals[i - 1] = ArgS(args, i);
  }

  StringVec memberships;
  memberships.reserve(vals.size());

  auto cb = [&](Transaction* t, EngineShard* shard) {
    auto find_res = shard->db_slice().FindReadOnly(t->GetDbContext(), key, OBJ_SET);
    if (find_res) {
      SetType st{find_res.value()->second.RObjPtr(), find_res.value()->second.Encoding()};
      FindInSet(memberships, t->GetDbContext(), st, vals);
      return OpStatus::OK;
    }
    return find_res.status();
  };

  auto* rb = static_cast<RedisReplyBuilder*>(cntx->reply_builder());
  OpResult<void> result = cntx->transaction->ScheduleSingleHop(std::move(cb));
  if (result == OpStatus::KEY_NOTFOUND) {
    memberships.assign(vals.size(), "0");
    return rb->SendStringArr(memberships);
  } else if (result == OpStatus::OK) {
    return rb->SendStringArr(memberships);
  }
  cntx->SendError(result.status());
}

void SMove(CmdArgList args, ConnectionContext* cntx) {
  string_view src = ArgS(args, 0);
  string_view dest = ArgS(args, 1);
  string_view member = ArgS(args, 2);

  Mover mover{src, dest, member, true};
  mover.Find(cntx->transaction);

  OpResult<unsigned> result = mover.Commit(cntx->transaction);
  if (!result) {
    return cntx->SendError(result.status());
    return;
  }

  cntx->SendLong(result.value());
}

void SRem(CmdArgList args, ConnectionContext* cntx) {
  string_view key = ArgS(args, 0);
  vector<string_view> vals(args.size() - 1);
  for (size_t i = 1; i < args.size(); ++i) {
    vals[i - 1] = ArgS(args, i);
  }
  ArgSlice span{vals.data(), vals.size()};

  auto cb = [&](Transaction* t, EngineShard* shard) {
    return OpRem(t->GetOpArgs(shard), key, span, false);
  };
  OpResult<uint32_t> result = cntx->transaction->ScheduleSingleHopT(std::move(cb));

  switch (result.status()) {
    case OpStatus::WRONG_TYPE:
      return cntx->SendError(kWrongTypeErr);
    case OpStatus::OK:
      return cntx->SendLong(result.value());
    default:
      return cntx->SendLong(0);
  }
}

void SCard(CmdArgList args, ConnectionContext* cntx) {
  string_view key = ArgS(args, 0);

  auto cb = [&](Transaction* t, EngineShard* shard) -> OpResult<uint32_t> {
    auto find_res = shard->db_slice().FindReadOnly(t->GetDbContext(), key, OBJ_SET);
    if (!find_res) {
      return find_res.status();
    }

    return find_res.value()->second.Size();
  };

  OpResult<uint32_t> result = cntx->transaction->ScheduleSingleHopT(std::move(cb));

  switch (result.status()) {
    case OpStatus::OK:
      return cntx->SendLong(result.value());
    case OpStatus::WRONG_TYPE:
      return cntx->SendError(kWrongTypeErr);
    default:
      return cntx->SendLong(0);
  }
}

void SPop(CmdArgList args, ConnectionContext* cntx) {
  string_view key = ArgS(args, 0);
  unsigned count = 1;
  if (args.size() > 1) {
    string_view arg = ArgS(args, 1);
    if (!absl::SimpleAtoi(arg, &count)) {
      cntx->SendError(kInvalidIntErr);
      return;
    }
  }

  auto cb = [&](Transaction* t, EngineShard* shard) {
    return OpPop(t->GetOpArgs(shard), key, count);
  };

  auto* rb = static_cast<RedisReplyBuilder*>(cntx->reply_builder());
  OpResult<StringVec> result = cntx->transaction->ScheduleSingleHopT(std::move(cb));
  if (result || result.status() == OpStatus::KEY_NOTFOUND) {
    if (args.size() == 1) {  // SPOP key
      if (result.status() == OpStatus::KEY_NOTFOUND) {
        rb->SendNull();
      } else {
        DCHECK_EQ(1u, result.value().size());
        rb->SendBulkString(result.value().front());
      }
    } else {  // SPOP key cnt
      rb->SendStringArr(*result, RedisReplyBuilder::SET);
    }
    return;
  }

  cntx->SendError(result.status());
}

void SDiff(CmdArgList args, ConnectionContext* cntx) {
  ResultStringVec result_set(shard_set->size(), OpStatus::SKIPPED);
  string_view src_key = ArgS(args, 0);
  ShardId src_shard = Shard(src_key, result_set.size());

  auto cb = [&](Transaction* t, EngineShard* shard) {
    ShardArgs largs = t->GetShardArgs(shard->shard_id());
    if (shard->shard_id() == src_shard) {
      CHECK_EQ(src_key, largs.Front());
      result_set[shard->shard_id()] = OpDiff(t->GetOpArgs(shard), largs.begin(), largs.end());
    } else {
      result_set[shard->shard_id()] = OpUnion(t->GetOpArgs(shard), largs.begin(), largs.end());
    }

    return OpStatus::OK;
  };

  cntx->transaction->ScheduleSingleHop(std::move(cb));
  ResultSetView rsv = DiffResultVec(result_set, src_shard);
  if (!rsv) {
    cntx->SendError(rsv.status());
    return;
  }

  SvArray arr = ToSvArray(rsv.value());
  if (cntx->conn_state.script_info) {  // sort under script
    sort(arr.begin(), arr.end());
  }
  auto* rb = static_cast<RedisReplyBuilder*>(cntx->reply_builder());
  rb->SendStringArr(arr, RedisReplyBuilder::SET);
}

void SDiffStore(CmdArgList args, ConnectionContext* cntx) {
  ResultStringVec result_set(shard_set->size(), OpStatus::SKIPPED);
  string_view dest_key = ArgS(args, 0);
  ShardId dest_shard = Shard(dest_key, result_set.size());
  string_view src_key = ArgS(args, 1);
  ShardId src_shard = Shard(src_key, result_set.size());

  VLOG(1) << "SDiffStore " << src_key << " " << src_shard;

  // read-only op
  auto diff_cb = [&](Transaction* t, EngineShard* shard) {
    ShardArgs largs = t->GetShardArgs(shard->shard_id());
    OpArgs op_args = t->GetOpArgs(shard);
    DCHECK(!largs.Empty());
    ShardArgs::Iterator start = largs.begin();
    ShardArgs::Iterator end = largs.end();
    if (shard->shard_id() == dest_shard) {
      CHECK_EQ(*start, dest_key);
      ++start;
      if (start == end)
        return OpStatus::OK;
    }

    if (shard->shard_id() == src_shard) {
      CHECK_EQ(src_key, *start);
      result_set[shard->shard_id()] = OpDiff(op_args, start, end);  // Diff
    } else {
      result_set[shard->shard_id()] = OpUnion(op_args, start, end);  // Union
    }

    return OpStatus::OK;
  };

  cntx->transaction->Execute(std::move(diff_cb), false);
  ResultSetView rsv = DiffResultVec(result_set, src_shard);
  if (!rsv) {
    cntx->transaction->Conclude();
    cntx->SendError(rsv.status());
    return;
  }

  SvArray result = ToSvArray(rsv.value());
  auto store_cb = [&](Transaction* t, EngineShard* shard) {
    if (shard->shard_id() == dest_shard) {
      OpAdd(t->GetOpArgs(shard), dest_key, result, true, true);
    }

    return OpStatus::OK;
  };

  cntx->transaction->Execute(std::move(store_cb), true);
  cntx->SendLong(result.size());
}

void SMembers(CmdArgList args, ConnectionContext* cntx) {
  auto cb = [](Transaction* t, EngineShard* shard) { return OpInter(t, shard, false); };

  OpResult<StringVec> result = cntx->transaction->ScheduleSingleHopT(std::move(cb));

  if (result || result.status() == OpStatus::KEY_NOTFOUND) {
    StringVec& svec = result.value();

    if (cntx->conn_state.script_info) {  // sort under script
      sort(svec.begin(), svec.end());
    }
    auto* rb = static_cast<RedisReplyBuilder*>(cntx->reply_builder());
    rb->SendStringArr(*result, RedisReplyBuilder::SET);
  } else {
    cntx->SendError(result.status());
  }
}

void SRandMember(CmdArgList args, ConnectionContext* cntx) {
  CmdArgParser parser{args};
  string_view key = parser.Next();

  bool is_count = parser.HasNext();
  int count = is_count ? parser.Next<int>() : 1;

  if (parser.HasNext())
    return cntx->SendError(WrongNumArgsError("SRANDMEMBER"));

  if (auto err = parser.Error(); err)
    return cntx->SendError(err->MakeReply());

  const unsigned ucount = std::abs(count);

  const auto cb = [&](Transaction* t, EngineShard* shard) -> OpResult<StringVec> {
    StringVec result;
    auto find_res = shard->db_slice().FindReadOnly(t->GetDbContext(), key, OBJ_SET);
    if (!find_res) {
      return find_res.status();
    }

    const PrimeValue& pv = find_res.value()->second;
    if (IsDenseEncoding(pv)) {
      StringSet* ss = (StringSet*)pv.RObjPtr();
      ss->set_time(MemberTimeSeconds(t->GetDbContext().time_now_ms));
    }

    container_utils::IterateSet(find_res.value()->second,
                                [&result, ucount](container_utils::ContainerEntry ce) {
                                  if (result.size() < ucount) {
                                    result.push_back(ce.ToString());
                                    return true;
                                  }
                                  return false;
                                });
    return result;
  };

  OpResult<StringVec> result = cntx->transaction->ScheduleSingleHopT(cb);
  auto* rb = static_cast<RedisReplyBuilder*>(cntx->reply_builder());
  if (result) {
    if (count < 0 && !result->empty()) {
      for (auto i = result->size(); i < ucount; ++i) {
        // we can return duplicate elements, so first is OK
        result->push_back(result->front());
      }
    }
    rb->SendStringArr(*result, RedisReplyBuilder::SET);
  } else if (result.status() == OpStatus::KEY_NOTFOUND) {
    if (is_count) {
      rb->SendStringArr(StringVec(), RedisReplyBuilder::SET);
    } else {
      rb->SendNull();
    }
  } else {
    rb->SendError(result.status());
  }
}

void SInter(CmdArgList args, ConnectionContext* cntx) {
  ResultStringVec result_set(shard_set->size(), OpStatus::SKIPPED);

  auto cb = [&](Transaction* t, EngineShard* shard) {
    result_set[shard->shard_id()] = OpInter(t, shard, false);

    return OpStatus::OK;
  };

  cntx->transaction->ScheduleSingleHop(std::move(cb));
  OpResult<SvArray> result = InterResultVec(result_set, cntx->transaction->GetUniqueShardCnt());
  if (result) {
    SvArray arr = std::move(*result);
    if (cntx->conn_state.script_info) {  // sort under script
      sort(arr.begin(), arr.end());
    }
    auto* rb = static_cast<RedisReplyBuilder*>(cntx->reply_builder());
    rb->SendStringArr(arr, RedisReplyBuilder::SET);
  } else {
    cntx->SendError(result.status());
  }
}

void SInterStore(CmdArgList args, ConnectionContext* cntx) {
  ResultStringVec result_set(shard_set->size(), OpStatus::SKIPPED);
  string_view dest_key = ArgS(args, 0);
  ShardId dest_shard = Shard(dest_key, result_set.size());
  atomic_uint32_t inter_shard_cnt{0};

  auto inter_cb = [&](Transaction* t, EngineShard* shard) {
    ShardArgs largs = t->GetShardArgs(shard->shard_id());
    if (shard->shard_id() == dest_shard) {
      CHECK_EQ(largs.Front(), dest_key);
      if (largs.Size() == 1)
        return OpStatus::OK;
    }
    inter_shard_cnt.fetch_add(1, memory_order_relaxed);
    result_set[shard->shard_id()] = OpInter(t, shard, shard->shard_id() == dest_shard);
    return OpStatus::OK;
  };

  cntx->transaction->Execute(std::move(inter_cb), false);

  OpResult<SvArray> result = InterResultVec(result_set, inter_shard_cnt.load(memory_order_relaxed));
  if (!result) {
    cntx->transaction->Conclude();
    cntx->SendError(result.status());
    return;
  }

  auto store_cb = [&](Transaction* t, EngineShard* shard) {
    if (shard->shard_id() == dest_shard) {
      OpAdd(t->GetOpArgs(shard), dest_key, result.value(), true, true);
    }

    return OpStatus::OK;
  };

  cntx->transaction->Execute(std::move(store_cb), true);
  cntx->SendLong(result->size());
}

void SInterCard(CmdArgList args, ConnectionContext* cntx) {
  unsigned num_keys;
  if (!absl::SimpleAtoi(ArgS(args, 0), &num_keys))
    return cntx->SendError(kSyntaxErr);

  unsigned limit = 0;
  if (args.size() == (num_keys + 3) && ArgS(args, 1 + num_keys) == "LIMIT") {
    if (!absl::SimpleAtoi(ArgS(args, num_keys + 2), &limit))
      return cntx->SendError("limit can't be negative");
  } else if (args.size() > (num_keys + 1))
    return cntx->SendError(kSyntaxErr);

  ResultStringVec result_set(shard_set->size(), OpStatus::SKIPPED);
  auto cb = [&](Transaction* t, EngineShard* shard) {
    result_set[shard->shard_id()] = OpInter(t, shard, false);
    return OpStatus::OK;
  };

  cntx->transaction->ScheduleSingleHop(std::move(cb));
  OpResult<SvArray> result =
      InterResultVec(result_set, cntx->transaction->GetUniqueShardCnt(), limit);

  return cntx->SendLong(result->size());
}

void SUnion(CmdArgList args, ConnectionContext* cntx) {
  ResultStringVec result_set(shard_set->size());

  auto cb = [&](Transaction* t, EngineShard* shard) {
    ShardArgs largs = t->GetShardArgs(shard->shard_id());
    result_set[shard->shard_id()] = OpUnion(t->GetOpArgs(shard), largs.begin(), largs.end());
    return OpStatus::OK;
  };

  cntx->transaction->ScheduleSingleHop(std::move(cb));

  ResultSetView unionset = UnionResultVec(result_set);
  if (unionset) {
    SvArray arr = ToSvArray(*unionset);
    if (cntx->conn_state.script_info) {  // sort under script
      sort(arr.begin(), arr.end());
    }
    auto* rb = static_cast<RedisReplyBuilder*>(cntx->reply_builder());
    rb->SendStringArr(arr, RedisReplyBuilder::SET);
  } else {
    cntx->SendError(unionset.status());
  }
}

void SUnionStore(CmdArgList args, ConnectionContext* cntx) {
  ResultStringVec result_set(shard_set->size(), OpStatus::SKIPPED);
  string_view dest_key = ArgS(args, 0);
  ShardId dest_shard = Shard(dest_key, result_set.size());

  auto union_cb = [&](Transaction* t, EngineShard* shard) {
    ShardArgs largs = t->GetShardArgs(shard->shard_id());
    ShardArgs::Iterator start = largs.begin(), end = largs.end();
    if (shard->shard_id() == dest_shard) {
      CHECK_EQ(*start, dest_key);
      ++start;
      if (start == end)
        return OpStatus::OK;
    }
    result_set[shard->shard_id()] = OpUnion(t->GetOpArgs(shard), start, end);
    return OpStatus::OK;
  };

  cntx->transaction->Execute(std::move(union_cb), false);

  ResultSetView unionset = UnionResultVec(result_set);
  if (!unionset) {
    cntx->transaction->Conclude();
    cntx->SendError(unionset.status());
    return;
  }

  SvArray result = ToSvArray(unionset.value());

  auto store_cb = [&](Transaction* t, EngineShard* shard) {
    if (shard->shard_id() == dest_shard) {
      OpAdd(t->GetOpArgs(shard), dest_key, result, true, true);
    }

    return OpStatus::OK;
  };

  cntx->transaction->Execute(std::move(store_cb), true);
  cntx->SendLong(result.size());
}

void SScan(CmdArgList args, ConnectionContext* cntx) {
  string_view key = ArgS(args, 0);
  string_view token = ArgS(args, 1);

  uint64_t cursor = 0;

  if (!absl::SimpleAtoi(token, &cursor)) {
    return cntx->SendError("invalid cursor");
  }

  // SSCAN key cursor [MATCH pattern] [COUNT count]
  if (args.size() > 6) {
    DVLOG(1) << "got " << args.size() << " this is more than it should be";
    return cntx->SendError(kSyntaxErr);
  }

  OpResult<ScanOpts> ops = ScanOpts::TryFrom(args.subspan(2));
  if (!ops) {
    DVLOG(1) << "SScan invalid args - return " << ops << " to the user";
    return cntx->SendError(ops.status());
  }

  ScanOpts scan_op = ops.value();

  auto cb = [&](Transaction* t, EngineShard* shard) {
    return OpScan(t->GetOpArgs(shard), key, &cursor, scan_op);
  };

  auto* rb = static_cast<RedisReplyBuilder*>(cntx->reply_builder());
  OpResult<StringVec> result = cntx->transaction->ScheduleSingleHopT(std::move(cb));
  if (result.status() != OpStatus::WRONG_TYPE) {
    rb->StartArray(2);
    rb->SendBulkString(absl::StrCat(cursor));
    rb->StartArray(result->size());  // Within scan the return page is of type array
    for (const auto& k : *result) {
      rb->SendBulkString(k);
    }
  } else {
    rb->SendError(result.status());
  }
}

// Syntax: saddex key ttl_sec member [member...]
void SAddEx(CmdArgList args, ConnectionContext* cntx) {
  string_view key = ArgS(args, 0);
  string_view ttl_str = ArgS(args, 1);
  uint32_t ttl_sec;
  constexpr uint32_t kMaxTtl = (1UL << 26);

  if (!absl::SimpleAtoi(ttl_str, &ttl_sec) || ttl_sec == 0 || ttl_sec > kMaxTtl) {
    return cntx->SendError(kInvalidIntErr);
  }

  vector<string_view> vals(args.size() - 2);
  for (size_t i = 2; i < args.size(); ++i) {
    vals[i - 2] = ArgS(args, i);
  }

  ArgSlice arg_slice{vals.data(), vals.size()};

  auto cb = [&](Transaction* t, EngineShard* shard) {
    return OpAddEx(t->GetOpArgs(shard), key, ttl_sec, arg_slice);
  };

  OpResult<uint32_t> result = cntx->transaction->ScheduleSingleHopT(std::move(cb));
  if (result) {
    return cntx->SendLong(result.value());
  }

  cntx->SendError(result.status());
}

}  // namespace

StringSet* SetFamily::ConvertToStrSet(const intset* is, size_t expected_len) {
  int64_t intele;
  char buf[32];
  int ii = 0;

  StringSet* ss = CompactObj::AllocateMR<StringSet>();
  if (expected_len) {
    ss->Reserve(expected_len);
  }

  while (intsetGet(const_cast<intset*>(is), ii++, &intele)) {
    char* next = absl::numbers_internal::FastIntToBuffer(intele, buf);
    string_view str{buf, size_t(next - buf)};
    CHECK(ss->Add(str));
  }

  return ss;
}

using CI = CommandId;

#define HFUNC(x) SetHandler(&x)

namespace acl {
constexpr uint32_t kSAdd = WRITE | SET | FAST;
constexpr uint32_t kSDiff = READ | SET | SLOW;
constexpr uint32_t kSDiffStore = WRITE | SET | SLOW;
constexpr uint32_t kSInter = READ | SET | SLOW;
constexpr uint32_t kSInterStore = WRITE | SET | SLOW;
constexpr uint32_t kSInterCard = READ | SET | SLOW;
constexpr uint32_t kSMembers = READ | SET | SLOW;
constexpr uint32_t kSIsMember = READ | SET | SLOW;
constexpr uint32_t kSMIsMember = READ | SET | FAST;
constexpr uint32_t kSMove = WRITE | SET | FAST;
constexpr uint32_t kSRem = WRITE | SET | FAST;
constexpr uint32_t kSCard = READ | SET | FAST;
constexpr uint32_t kSPop = WRITE | SET | SLOW;
constexpr uint32_t kSRandMember = READ | SET | SLOW;
constexpr uint32_t kSUnion = READ | SET | SLOW;
constexpr uint32_t kSUnionStore = WRITE | SET | SLOW;
constexpr uint32_t kSScan = READ | SET | SLOW;
}  // namespace acl

void SetFamily::Register(CommandRegistry* registry) {
  registry->StartFamily();
  *registry
      << CI{"SADD", CO::WRITE | CO::FAST | CO::DENYOOM, -3, 1, 1, acl::kSAdd}.HFUNC(SAdd)
      << CI{"SDIFF", CO::READONLY, -2, 1, -1, acl::kSDiff}.HFUNC(SDiff)
      << CI{"SDIFFSTORE", CO::WRITE | CO::DENYOOM | CO::NO_AUTOJOURNAL, -3, 1, -1, acl::kSDiffStore}
             .HFUNC(SDiffStore)
      << CI{"SINTER", CO::READONLY, -2, 1, -1, acl::kSInter}.HFUNC(SInter)
      << CI{"SINTERSTORE",    CO::WRITE | CO::DENYOOM | CO::NO_AUTOJOURNAL, -3, 1, -1,
            acl::kSInterStore}
             .HFUNC(SInterStore)
      << CI{"SINTERCARD", CO::READONLY | CO::VARIADIC_KEYS, -3, 2, 2, acl::kSInterCard}.HFUNC(
             SInterCard)
      << CI{"SMEMBERS", CO::READONLY, 2, 1, 1, acl::kSMembers}.HFUNC(SMembers)
      << CI{"SISMEMBER", CO::FAST | CO::READONLY, 3, 1, 1, acl::kSIsMember}.HFUNC(SIsMember)
      << CI{"SMISMEMBER", CO::READONLY, -3, 1, 1, acl::kSMIsMember}.HFUNC(SMIsMember)
      << CI{"SMOVE", CO::FAST | CO::WRITE | CO::NO_AUTOJOURNAL, 4, 1, 2, acl::kSMove}.HFUNC(SMove)
      << CI{"SREM", CO::WRITE | CO::FAST, -3, 1, 1, acl::kSRem}.HFUNC(SRem)
      << CI{"SCARD", CO::READONLY | CO::FAST, 2, 1, 1, acl::kSCard}.HFUNC(SCard)
      << CI{"SPOP", CO::WRITE | CO::FAST | CO::NO_AUTOJOURNAL, -2, 1, 1, acl::kSPop}.HFUNC(SPop)
      << CI{"SRANDMEMBER", CO::READONLY, -2, 1, 1, acl::kSRandMember}.HFUNC(SRandMember)
      << CI{"SUNION", CO::READONLY, -2, 1, -1, acl::kSUnion}.HFUNC(SUnion)
      << CI{"SUNIONSTORE",    CO::WRITE | CO::DENYOOM | CO::NO_AUTOJOURNAL, -3, 1, -1,
            acl::kSUnionStore}
             .HFUNC(SUnionStore)
      << CI{"SSCAN", CO::READONLY, -3, 1, 1, acl::kSScan}.HFUNC(SScan)
      << CI{"SADDEX", CO::WRITE | CO::FAST | CO::DENYOOM, -4, 1, 1, acl::kSAdd}.HFUNC(SAddEx);
}

uint32_t SetFamily::MaxIntsetEntries() {
  return kMaxIntSetEntries;
}

int32_t SetFamily::FieldExpireTime(const DbContext& db_context, const PrimeValue& pv,
                                   std::string_view field) {
  DCHECK_EQ(OBJ_SET, pv.ObjType());

  SetType st{pv.RObjPtr(), pv.Encoding()};
  return GetExpiry(db_context, st, field);
}

}  // namespace dfly
