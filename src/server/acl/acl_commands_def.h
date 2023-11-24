// Copyright 2023, DragonflyDB authors.  All rights reserved.
// See LICENSE for licensing terms.
//

#pragma once

#include "absl/container/flat_hash_map.h"
#include "facade/acl_commands_def.h"

namespace dfly::acl {
/* There are 21 ACL categories as of redis 7
 *
 * bit 0: keyspace
 * bit 1: read
 * bit 2: write
 * bit 3: set
 * bit 4: sortedset
 * bit 5: list
 * bit 6: hash
 * bit 7: string
 * bit 8: bitmap
 * bit 9: hyperloglog
 * bit 10: geo
 * bit 11: stream
 * bit 12: pubsub
 * bit 13: admin
 * bit 14: fast
 * bit 15: slow
 * bit 16: blocking
 * bit 17: dangerous
 * bit 18: connection
 * bit 19: transaction
 * bit 20: scripting
 * bits 21..28: tba
 * Dragonfly extensions:
 * bit 29: ft_search
 * bit 30: throttle
 * bit 31: json
 */

enum AclCat {
  KEYSPACE = 1ULL << 0,
  READ = 1ULL << 1,
  WRITE = 1ULL << 2,
  SET = 1ULL << 3,
  SORTEDSET = 1ULL << 4,
  LIST = 1ULL << 5,
  HASH = 1ULL << 6,
  STRING = 1ULL << 7,
  BITMAP = 1ULL << 8,
  HYPERLOGLOG = 1ULL << 9,
  GEO = 1ULL << 10,
  STREAM = 1ULL << 11,
  PUBSUB = 1ULL << 12,
  ADMIN = 1ULL << 13,
  FAST = 1ULL << 14,
  SLOW = 1ULL << 15,
  BLOCKING = 1ULL << 16,
  DANGEROUS = 1ULL << 17,
  CONNECTION = 1ULL << 18,
  TRANSACTION = 1ULL << 19,
  SCRIPTING = 1ULL << 20,
  FT_SEARCH = 1ULL << 29,
  THROTTLE = 1ULL << 30,
  JSON = 1ULL << 31
};

// See definitions for NONE and ALL in facade/acl_commands_def.h

inline const absl::flat_hash_map<std::string_view, uint32_t> CATEGORY_INDEX_TABLE{
    {"KEYSPACE", KEYSPACE},
    {"READ", READ},
    {"WRITE", WRITE},
    {"SET", SET},
    {"SORTED_SET", SORTEDSET},
    {"LIST", LIST},
    {"HASH", HASH},
    {"STRING", STRING},
    {"BITMAP", BITMAP},
    {"HYPERLOG", HYPERLOGLOG},
    {"GEO", GEO},
    {"STREAM", STREAM},
    {"PUBSUB", PUBSUB},
    {"ADMIN", ADMIN},
    {"FAST", FAST},
    {"SLOW", SLOW},
    {"BLOCKING", BLOCKING},
    {"DANGEROUS", DANGEROUS},
    {"CONNECTION", CONNECTION},
    {"TRANSACTION", TRANSACTION},
    {"SCRIPTING", SCRIPTING},
    {"FT_SEARCH", FT_SEARCH},
    {"THROTTLE", THROTTLE},
    {"JSON", JSON},
    {"ALL", ALL},
    {"NONE", NONE}};

// bit 0 at index 0
// bit 1 at index 1
// bit n at index n
inline const std::vector<std::string> REVERSE_CATEGORY_INDEX_TABLE{
    "KEYSPACE",  "READ",      "WRITE",     "SET",       "SORTED_SET", "LIST",        "HASH",
    "STRING",    "BITMAP",    "HYPERLOG",  "GEO",       "STREAM",     "PUBSUB",      "ADMIN",
    "FAST",      "SLOW",      "BLOCKING",  "DANGEROUS", "CONNECTION", "TRANSACTION", "SCRIPTING",
    "_RESERVED", "_RESERVED", "_RESERVED", "_RESERVED", "_RESERVED",  "_RESERVED",   "_RESERVED",
    "_RESERVED", "FT_SEARCH", "THROTTLE",  "JSON"};

using RevCommandField = std::vector<std::string>;
using RevCommandsIndexStore = std::vector<RevCommandField>;

constexpr uint64_t ALL_COMMANDS = std::numeric_limits<uint64_t>::max();
constexpr uint64_t NONE_COMMANDS = std::numeric_limits<uint64_t>::min();

// A variation of meyers singleton
// This is initialized when the constructor of Service is called.
// Basically, it calls this functions within the AclFamily::Register
// functions which has the number of all the acl families registered
inline size_t NumberOfFamilies(size_t number = 0) {
  static size_t number_of_families = number;
  return number_of_families;
}

inline const RevCommandsIndexStore& CommandsRevIndexer(RevCommandsIndexStore store = {}) {
  static RevCommandsIndexStore rev_index_store = std::move(store);
  return rev_index_store;
}

inline void BuildIndexers(std::vector<std::vector<std::string>> families) {
  acl::NumberOfFamilies(families.size());
  acl::CommandsRevIndexer(std::move(families));
}

}  // namespace dfly::acl
