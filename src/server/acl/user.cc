// Copyright 2023, DragonflyDB authors.  All rights reserved.
// See LICENSE for licensing terms.
//

#include "server/acl/user.h"

#include <xxhash.h>

namespace dfly {

User::User() {
  // acl_categories_ = AclCat::ACL_CATEGORY_ADMIN;
}

void User::Update(UpdateRequest&& req) {
  if (req.password) {
    SetPassword(*req.password);
  }

  if (req.plus_acl_categories) {
    SetAclCategories(*req.plus_acl_categories);
  }

  if (req.minus_acl_categories) {
    UnsetAclCategories(*req.minus_acl_categories);
  }

  if (req.is_active) {
    SetIsActive(*req.is_active);
  }
}

void User::SetPassword(std::string_view password) {
  password_ = HashPassword(password);
}

bool User::HasPassword(std::string_view password) const {
  if (!password_) {
    if (password == "nopass") {
      return true;
    }
    return false;
  }
  // hash password and compare
  return *password_ == HashPassword(password);
}

void User::SetAclCategories(uint64_t cat) {
  acl_categories_ |= cat;
}

void User::UnsetAclCategories(uint64_t cat) {
  SetAclCategories(cat);
  acl_categories_ ^= cat;
}

uint32_t User::AclCategory() const {
  return acl_categories_;
}

// For ACL commands
// void SetAclCommand()
// void AclCommand() const;

void User::SetIsActive(bool is_active) {
  is_active_ = is_active;
}

bool User::IsActive() const {
  return is_active_;
}

uint32_t User::HashPassword(std::string_view password) const {
  return XXH3_64bits(password.data(), password.size());
}

}  // namespace dfly
