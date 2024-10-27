// Copyright 2023, DragonflyDB authors.  All rights reserved.
// See LICENSE for licensing terms.
//

#pragma once

#include <absl/container/flat_hash_map.h>
#include <absl/types/span.h>

#include <string>
#include <utility>

#include "core/json/json_object.h"
#include "core/search/search.h"
#include "server/common.h"
#include "server/search/doc_index.h"
#include "server/table.h"

namespace dfly {

class StringMap;

// Document accessors allow different types (json/hset) to be hidden
// behind a document interface for quering fields and serializing.
// Field string_view's are only valid until the next is requested.
struct BaseAccessor : public search::DocumentAccessor {
  // Serialize all fields
  virtual SearchDocData Serialize(const search::Schema& schema) const = 0;

  // Serialize selected fields
  virtual SearchDocData Serialize(const search::Schema& schema,
                                  absl::Span<const SearchField<std::string_view>> fields) const;

  /*
  Serialize the whole type, the default implementation is to serialize all fields.
  For JSON in FT.SEARCH we need to get answer as {"$", <the whole document>}, but it is not an
  indexed field
  */
  virtual SearchDocData SerializeDocument(const search::Schema& schema) const;
};

// Accessor for hashes stored with listpack
struct ListPackAccessor : public BaseAccessor {
  using LpPtr = uint8_t*;

  explicit ListPackAccessor(LpPtr ptr) : lp_{ptr} {
  }

  StringList GetStrings(std::string_view field) const override;
  VectorInfo GetVector(std::string_view field) const override;
  SearchDocData Serialize(const search::Schema& schema) const override;

 private:
  mutable std::array<uint8_t, 33> intbuf_[2];
  LpPtr lp_;
};

// Accessor for hashes stored with StringMap
struct StringMapAccessor : public BaseAccessor {
  explicit StringMapAccessor(StringMap* hset) : hset_{hset} {
  }

  StringList GetStrings(std::string_view field) const override;
  VectorInfo GetVector(std::string_view field) const override;
  SearchDocData Serialize(const search::Schema& schema) const override;

 private:
  StringMap* hset_;
};

// Accessor for json values
struct JsonAccessor : public BaseAccessor {
  struct JsonPathContainer;  // contains jsoncons::jsonpath::jsonpath_expression

  explicit JsonAccessor(const JsonType* json) : json_{*json} {
  }

  StringList GetStrings(std::string_view field) const override;
  VectorInfo GetVector(std::string_view field) const override;

  // The JsonAccessor works with structured types and not plain strings, so an overload is needed
  SearchDocData Serialize(const search::Schema& schema,
                          absl::Span<const SearchField<std::string_view>> fields) const override;
  SearchDocData Serialize(const search::Schema& schema) const override;
  SearchDocData SerializeDocument(const search::Schema& schema) const override;

  static void RemoveFieldFromCache(std::string_view field);

 private:
  /// Parses `field` into a JSON path. Caches the results internally.
  JsonPathContainer* GetPath(std::string_view field) const;

  const JsonType& json_;
  mutable std::string buf_;

  // Contains built json paths to avoid parsing them repeatedly
  static thread_local absl::flat_hash_map<std::string, std::unique_ptr<JsonPathContainer>>
      path_cache_;
};

// Get accessor for value
std::unique_ptr<BaseAccessor> GetAccessor(const DbContext& db_cntx, const PrimeValue& pv);

}  // namespace dfly
