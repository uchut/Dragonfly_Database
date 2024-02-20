// Copyright 2023, DragonflyDB authors.  All rights reserved.
// See LICENSE for licensing terms.
//

// GCC yields a spurious warning about uninitialized data in DocumentAccessor::StringList.
#ifdef __GNUC__
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#endif

#include "server/search/doc_accessors.h"

#include <absl/strings/str_cat.h>
#include <absl/strings/str_join.h>

#include <jsoncons/json.hpp>

#include "base/flags.h"
#include "core/json/path.h"
#include "core/overloaded.h"
#include "core/search/search.h"
#include "core/search/vector_utils.h"
#include "core/string_map.h"
#include "server/container_utils.h"

extern "C" {
#include "redis/listpack.h"
};

ABSL_DECLARE_FLAG(bool, jsonpathv2);

namespace dfly {

using namespace std;

namespace {

string_view SdsToSafeSv(sds str) {
  return str != nullptr ? string_view{str, sdslen(str)} : ""sv;
}

string PrintField(search::SchemaField::FieldType type, string_view value) {
  if (type == search::SchemaField::VECTOR) {
    auto [ptr, size] = search::BytesToFtVector(value);
    return absl::StrCat("[", absl::StrJoin(absl::Span<const float>{ptr.get(), size}, ","), "]");
  }
  return string{value};
}

string ExtractValue(const search::Schema& schema, string_view key, string_view value) {
  auto it = schema.fields.find(key);
  if (it == schema.fields.end())
    return string{value};

  return PrintField(it->second.type, value);
}

}  // namespace

SearchDocData BaseAccessor::Serialize(const search::Schema& schema,
                                      const SearchParams::FieldReturnList& fields) const {
  SearchDocData out{};
  for (const auto& [fident, fname] : fields) {
    auto it = schema.fields.find(fident);
    auto type = it != schema.fields.end() ? it->second.type : search::SchemaField::TEXT;
    out[fname] = PrintField(type, absl::StrJoin(GetStrings(fident), ","));
  }
  return out;
}

BaseAccessor::StringList ListPackAccessor::GetStrings(string_view active_field) const {
  auto strsv = container_utils::LpFind(lp_, active_field, intbuf_[0].data());
  return strsv.has_value() ? StringList{*strsv} : StringList{};
}

BaseAccessor::VectorInfo ListPackAccessor::GetVector(string_view active_field) const {
  auto strlist = GetStrings(active_field);
  return strlist.empty() ? VectorInfo{} : search::BytesToFtVector(strlist.front());
}

SearchDocData ListPackAccessor::Serialize(const search::Schema& schema) const {
  SearchDocData out{};

  uint8_t* fptr = lpFirst(lp_);
  DCHECK_NE(fptr, nullptr);

  while (fptr) {
    string_view k = container_utils::LpGetView(fptr, intbuf_[0].data());
    fptr = lpNext(lp_, fptr);
    string_view v = container_utils::LpGetView(fptr, intbuf_[1].data());
    fptr = lpNext(lp_, fptr);

    out[k] = ExtractValue(schema, k, v);
  }

  return out;
}

BaseAccessor::StringList StringMapAccessor::GetStrings(string_view active_field) const {
  auto it = hset_->Find(active_field);
  return it != hset_->end() ? StringList{SdsToSafeSv(it->second)} : StringList{};
}

BaseAccessor::VectorInfo StringMapAccessor::GetVector(string_view active_field) const {
  auto strlist = GetStrings(active_field);
  return strlist.empty() ? VectorInfo{} : search::BytesToFtVector(strlist.front());
}

SearchDocData StringMapAccessor::Serialize(const search::Schema& schema) const {
  SearchDocData out{};
  for (const auto& [kptr, vptr] : *hset_)
    out[SdsToSafeSv(kptr)] = ExtractValue(schema, SdsToSafeSv(kptr), SdsToSafeSv(vptr));

  return out;
}

struct JsonAccessor::JsonPathContainer {
  vector<JsonType> Evaluate(const JsonType& json) const {
    vector<JsonType> res;

    visit(Overloaded{[&](const json::Path& path) {
                       json::EvaluatePath(path, json,
                                          [&](auto, const JsonType& v) { res.push_back(v); });
                     },
                     [&](const jsoncons::jsonpath::jsonpath_expression<JsonType>& path) {
                       auto json_arr = path.evaluate(json);
                       for (const auto& v : json_arr.array_range())
                         res.push_back(v);
                     }},
          val);

    return res;
  }

  variant<json::Path, jsoncons::jsonpath::jsonpath_expression<JsonType>> val;
};

BaseAccessor::StringList JsonAccessor::GetStrings(string_view active_field) const {
  auto* path = GetPath(active_field);
  if (!path)
    return {};

  auto path_res = path->Evaluate(json_);
  if (path_res.empty())
    return {};

  if (path_res.size() == 1) {
    buf_ = path_res[0].as_string();
    return {buf_};
  }
  buf_.clear();

  // First, grow buffer and compute string sizes
  vector<size_t> sizes;
  for (const auto& element : path_res) {
    size_t start = buf_.size();
    buf_ += element.as_string();
    sizes.push_back(buf_.size() - start);
  }

  // Reposition start pointers to the most recent allocation of buf
  StringList out(sizes.size());

  size_t start = 0;
  for (size_t i = 0; i < out.size(); i++) {
    out[i] = string_view{buf_}.substr(start, sizes[i]);
    start += sizes[i];
  }

  return out;
}

BaseAccessor::VectorInfo JsonAccessor::GetVector(string_view active_field) const {
  auto* path = GetPath(active_field);
  if (!path)
    return {};

  auto res = path->Evaluate(json_);
  if (res.empty())
    return {nullptr, 0};

  size_t size = res[0].size();
  auto ptr = make_unique<float[]>(size);

  size_t i = 0;
  for (const auto& v : res[0].array_range())
    ptr[i++] = v.as<float>();

  return {std::move(ptr), size};
}

JsonAccessor::JsonPathContainer* JsonAccessor::GetPath(std::string_view field) const {
  if (auto it = path_cache_.find(field); it != path_cache_.end()) {
    return it->second.get();
  }

  string ec_msg;
  unique_ptr<JsonPathContainer> ptr;
  if (absl::GetFlag(FLAGS_jsonpathv2)) {
    auto path_expr = json::ParsePath(field);
    if (path_expr) {
      ptr.reset(new JsonPathContainer{std::move(path_expr.value())});
    } else {
      ec_msg = path_expr.error();
    }
  } else {
    error_code ec;
    auto path_expr = MakeJsonPathExpr(field, ec);
    if (ec) {
      ec_msg = ec.message();
    } else {
      ptr.reset(new JsonPathContainer{std::move(path_expr)});
    }
  }

  if (!ptr) {
    LOG(WARNING) << "Invalid Json path: " << field << ' ' << ec_msg;
    return nullptr;
  }

  JsonPathContainer* path = ptr.get();
  path_cache_[field] = std::move(ptr);
  return path;
}

SearchDocData JsonAccessor::Serialize(const search::Schema& schema) const {
  return {{"$", json_.to_string()}};
}

SearchDocData JsonAccessor::Serialize(const search::Schema& schema,
                                      const SearchParams::FieldReturnList& fields) const {
  SearchDocData out{};
  for (const auto& [ident, name] : fields) {
    if (auto* path = GetPath(ident); path) {
      if (auto res = path->Evaluate(json_); !res.empty())
        out[name] = res[0].to_string();
    }
  }
  return out;
}

void JsonAccessor::RemoveFieldFromCache(string_view field) {
  path_cache_.erase(field);
}

thread_local absl::flat_hash_map<std::string, std::unique_ptr<JsonAccessor::JsonPathContainer>>
    JsonAccessor::path_cache_;

unique_ptr<BaseAccessor> GetAccessor(const DbContext& db_cntx, const PrimeValue& pv) {
  DCHECK(pv.ObjType() == OBJ_HASH || pv.ObjType() == OBJ_JSON);

  if (pv.ObjType() == OBJ_JSON) {
    DCHECK(pv.GetJson());
    return make_unique<JsonAccessor>(pv.GetJson());
  }

  if (pv.Encoding() == kEncodingListPack) {
    auto ptr = reinterpret_cast<ListPackAccessor::LpPtr>(pv.RObjPtr());
    return make_unique<ListPackAccessor>(ptr);
  } else {
    auto* sm = container_utils::GetStringMap(pv, db_cntx);
    return make_unique<StringMapAccessor>(sm);
  }
}

}  // namespace dfly
