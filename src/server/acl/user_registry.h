// Copyright 2022, DragonflyDB authors.  All rights reserved.
// See LICENSE for licensing terms.
//

#pragma once

#include <absl/container/flat_hash_map.h>
#include <absl/synchronization/mutex.h>

#include <algorithm>
#include <shared_mutex>
#include <string>
#include <utility>
#include <vector>

#include "core/fibers.h"
#include "server/acl/user.h"

namespace dfly::acl {

class UserRegistry {
 private:
  template <template <typename T> typename LockT, typename RegT> class RegistryWithLock;

 public:
  UserRegistry() = default;

  UserRegistry(const UserRegistry&) = delete;
  UserRegistry(UserRegistry&&) = delete;

  void Init();

  using RegistryType = absl::flat_hash_map<std::string, User>;

  // Acquires a write lock of mu_
  // If the user with name `username` does not exist, it's added in the store with
  // the exact fields found in req
  // If the user exists, the bitfields are updated with a `logical and` operation
  void MaybeAddAndUpdate(std::string_view username, User::UpdateRequest req);

  // Acquires a write lock on mu_
  // Removes user from the store
  // kills already existing connections from the removed user
  bool RemoveUser(std::string_view username);

  struct UserCredentials {
    uint32_t acl_categories{0};
    std::vector<uint64_t> acl_commands;
    AclKeys keys;
  };

  // Acquires a read lock
  UserCredentials GetCredentials(std::string_view username) const;

  // Acquires a read lock
  bool IsUserActive(std::string_view username) const;

  // Acquires a read lock
  bool AuthUser(std::string_view username, std::string_view password) const;

  using RegistryViewWithLock = RegistryWithLock<std::shared_lock, const RegistryType&>;
  using RegistryWithWriteLock = RegistryWithLock<std::unique_lock, RegistryType&>;

  // Helper function used for printing users via ACL LIST
  RegistryViewWithLock GetRegistryWithLock() const;

  // Helper function to propagate a write lock outside the registry's scope
  RegistryWithWriteLock GetRegistryWithWriteLock();

  // Helper class for accessing a user with a ReadLock outside the scope of UserRegistry
  class UserWithWriteLock {
   public:
    UserWithWriteLock(std::unique_lock<util::SharedMutex> lk, const User& user, bool exists);
    const User& user;
    const bool exists;

   private:
    std::unique_lock<util::SharedMutex> registry_lk_;
  };

  UserWithWriteLock MaybeAddAndUpdateWithLock(std::string_view username, User::UpdateRequest req);

  User::UpdateRequest DefaultUserUpdateRequest() const;

 private:
  RegistryType registry_;
  mutable util::SharedMutex mu_;

  // Helper class for accessing the registry with a ReadLock outside the scope of UserRegistry
  template <template <typename T> typename LockT, typename RegT> class RegistryWithLock {
   public:
    RegistryWithLock(LockT<util::SharedMutex> lk, RegT reg)
        : registry(reg), registry_lk_(std::move(lk)) {
    }
    RegT registry;

   private:
    LockT<util::SharedMutex> registry_lk_;
  };
};

}  // namespace dfly::acl
