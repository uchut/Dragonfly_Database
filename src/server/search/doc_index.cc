// Copyright 2023, DragonflyDB authors.  All rights reserved.
// See LICENSE for licensing terms.
//

#include "server/search/doc_index.h"

#include "base/logging.h"
#include "server/engine_shard_set.h"
#include "server/search/doc_accessors.h"

extern "C" {
#include "redis/object.h"
};

namespace dfly {

using namespace std;

namespace {

template <typename F>
void TraverseAllMatching(const DocIndex& index, const OpArgs& op_args, F&& f) {
  auto& db_slice = op_args.shard->db_slice();
  DCHECK(db_slice.IsDbValid(op_args.db_cntx.db_index));
  auto [prime_table, _] = db_slice.GetTables(op_args.db_cntx.db_index);

  string scratch;
  auto cb = [&](PrimeTable::iterator it) {
    const PrimeValue& pv = it->second;
    if (pv.ObjType() != index.GetObjCode())
      return;

    string_view key = it->first.GetSlice(&scratch);
    if (key.rfind(index.prefix, 0) != 0)
      return;

    auto accessor = GetAccessor(op_args.db_cntx, pv);
    f(key, accessor.get());
  };

  PrimeTable::Cursor cursor;
  do {
    cursor = prime_table->Traverse(cursor, cb);
  } while (cursor);
}

}  // namespace

ShardDocIndex::DocId ShardDocIndex::DocKeyIndex::Add(string_view key) {
  DCHECK_EQ(ids_.count(key), 0u);

  DocId id;
  if (!free_ids_.empty()) {
    id = free_ids_.back();
    free_ids_.pop_back();
    keys_[id] = key;
  } else {
    id = last_id_++;
    DCHECK_EQ(keys_.size(), id);
    keys_.emplace_back(key);
  }

  ids_[key] = id;
  return id;
}

ShardDocIndex::DocId ShardDocIndex::DocKeyIndex::Remove(string_view key) {
  DCHECK_GT(ids_.count(key), 0u);

  DocId id = ids_.find(key)->second;
  keys_[id] = "";
  ids_.erase(key);
  free_ids_.push_back(id);

  return id;
}

string_view ShardDocIndex::DocKeyIndex::Get(DocId id) const {
  DCHECK_LT(id, keys_.size());
  DCHECK_GT(keys_[id].size(), 0u);

  return keys_[id];
}

uint8_t DocIndex::GetObjCode() const {
  return type == JSON ? OBJ_JSON : OBJ_HASH;
}

bool DocIndex::Matches(string_view key, unsigned obj_code) const {
  return obj_code == GetObjCode() && key.rfind(prefix, 0) == 0;
}

ShardDocIndex::ShardDocIndex(shared_ptr<DocIndex> index)
    : base_{index}, indices_{index->schema}, key_index_{} {
}

void ShardDocIndex::Init(const OpArgs& op_args) {
  auto cb = [this](string_view key, BaseAccessor* doc) { indices_.Add(key_index_.Add(key), doc); };
  TraverseAllMatching(*base_, op_args, cb);
}

void ShardDocIndex::AddDoc(string_view key, const DbContext& db_cntx, const PrimeValue& pv) {
  auto accessor = GetAccessor(db_cntx, pv);
  indices_.Add(key_index_.Add(key), accessor.get());
}

void ShardDocIndex::RemoveDoc(string_view key, const DbContext& db_cntx, const PrimeValue& pv) {
  auto accessor = GetAccessor(db_cntx, pv);
  DocId id = key_index_.Remove(key);
  indices_.Remove(id, accessor.get());
}

bool ShardDocIndex::Matches(string_view key, unsigned obj_code) const {
  return base_->Matches(key, obj_code);
}

SearchResult ShardDocIndex::Search(const OpArgs& op_args, const SearchParams& params,
                                   search::SearchAlgorithm* search_algo) const {
  auto& db_slice = op_args.shard->db_slice();

  auto doc_ids = search_algo->Search(&indices_);

  vector<SerializedSearchDoc> out;
  for (search::DocId doc : doc_ids) {
    auto key = key_index_.Get(doc);
    auto it = db_slice.Find(op_args.db_cntx, key, base_->GetObjCode());
    CHECK(it) << "Expected key: " << key << " to exist";
    auto doc_access = GetAccessor(op_args.db_cntx, (*it)->second);
    out.emplace_back(key, doc_access->Serialize());

    // Scoring is not implemented yet, so we take just the first documents
    if (out.size() >= params.limit_offset + params.limit_total)
      break;
  }

  return SearchResult{std::move(out), doc_ids.size()};
}

ShardDocIndex* ShardDocIndices::GetIndex(string_view name) {
  auto it = indices_.find(name);
  return it != indices_.end() ? it->second.get() : nullptr;
}

void ShardDocIndices::InitIndex(const OpArgs& op_args, std::string_view name,
                                shared_ptr<DocIndex> index_ptr) {
  auto shard_index = make_unique<ShardDocIndex>(index_ptr);
  auto [it, _] = indices_.emplace(name, move(shard_index));
  it->second->Init(op_args);

  op_args.shard->db_slice().SetDocDeletionCallback(
      [this](string_view key, const DbContext& cntx, const PrimeValue& pv) {
        RemoveDoc(key, cntx, pv);
      });
}

void ShardDocIndices::AddDoc(string_view key, const DbContext& db_cntx, const PrimeValue& pv) {
  for (auto& [_, index] : indices_) {
    if (index->Matches(key, pv.ObjType()))
      index->AddDoc(key, db_cntx, pv);
  }
}

void ShardDocIndices::RemoveDoc(string_view key, const DbContext& db_cntx, const PrimeValue& pv) {
  for (auto& [_, index] : indices_) {
    if (index->Matches(key, pv.ObjType()))
      index->RemoveDoc(key, db_cntx, pv);
  }
}

}  // namespace dfly
