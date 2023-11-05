// Copyright 2022, DragonflyDB authors.  All rights reserved.
// See LICENSE for licensing terms.
//

#include "server/acl/helpers.h"

#include <limits>
#include <vector>

#include "absl/strings/ascii.h"
#include "absl/strings/escaping.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_split.h"
#include "server/acl/acl_commands_def.h"
#include "server/common.h"

namespace dfly::acl {

std::string AclCatToString(uint32_t acl_category) {
  std::string tmp;

  if (acl_category == acl::ALL) {
    return "+@ALL";
  }

  if (acl_category == acl::NONE) {
    return "+@NONE";
  }

  const std::string prefix = "+@";
  const std::string postfix = " ";

  for (uint32_t i = 0; i < 32; ++i) {
    uint32_t cat_bit = 1ULL << i;
    if (acl_category & cat_bit) {
      absl::StrAppend(&tmp, prefix, REVERSE_CATEGORY_INDEX_TABLE[i], postfix);
    }
  }

  tmp.pop_back();

  return tmp;
}

std::string AclCommandToString(const std::vector<uint64_t>& acl_category) {
  std::string result;

  const std::string prefix = "+";
  const std::string postfix = " ";
  const auto& rev_index = CommandsRevIndexer();
  bool all = true;

  size_t family_id = 0;
  for (auto family : acl_category) {
    for (uint64_t i = 0; i < 64; ++i) {
      const uint64_t cmd_bit = 1ULL << i;
      if (family & cmd_bit && i < rev_index[family_id].size()) {
        absl::StrAppend(&result, prefix, rev_index[family_id][i], postfix);
        continue;
      }
      if (i < rev_index[family_id].size()) {
        all = false;
      }
    }
    ++family_id;
  }

  if (!result.empty()) {
    result.pop_back();
  }
  return all ? "+ALL" : result;
}

std::string PrettyPrintSha(std::string_view pass, bool all) {
  if (all) {
    return absl::BytesToHexString(pass);
  }
  return absl::BytesToHexString(pass.substr(0, 15)).substr(0, 15);
};

std::optional<std::string> MaybeParsePassword(std::string_view command, bool hashed) {
  if (command == "nopass") {
    return std::string(command);
  }

  char symbol = hashed ? '#' : '>';
  if (command[0] != symbol) {
    return {};
  }

  return std::string(command.substr(1));
}

std::optional<bool> MaybeParseStatus(std::string_view command) {
  if (command == "ON") {
    return true;
  }
  if (command == "OFF") {
    return false;
  }
  return {};
}

using OptCat = std::optional<uint32_t>;

// bool == true if +
// bool == false if -
std::pair<OptCat, bool> MaybeParseAclCategory(std::string_view command) {
  if (absl::StartsWith(command, "+@")) {
    auto res = CATEGORY_INDEX_TABLE.find(command.substr(2));
    if (res == CATEGORY_INDEX_TABLE.end()) {
      return {};
    }
    return {res->second, true};
  }

  if (absl::StartsWith(command, "-@")) {
    auto res = CATEGORY_INDEX_TABLE.find(command.substr(2));
    if (res == CATEGORY_INDEX_TABLE.end()) {
      return {};
    }
    return {res->second, false};
  }

  return {};
}

bool IsIndexAllCommandsFlag(size_t index) {
  return index == std::numeric_limits<size_t>::max();
}

std::pair<OptCommand, bool> MaybeParseAclCommand(std::string_view command,
                                                 const CommandRegistry& registry) {
  const auto all_commands = std::pair<size_t, uint64_t>{std::numeric_limits<size_t>::max(), 0};
  if (command == "+ALL") {
    return {all_commands, true};
  }

  if (command == "-ALL") {
    return {all_commands, false};
  }

  if (absl::StartsWith(command, "+")) {
    auto res = registry.Find(command.substr(1));
    if (!res) {
      return {};
    }
    std::pair<size_t, uint64_t> cmd{res->GetFamily(), res->GetBitIndex()};
    return {cmd, true};
  }

  if (absl::StartsWith(command, "-")) {
    auto res = registry.Find(command.substr(1));
    if (!res) {
      return {};
    }
    std::pair<size_t, uint64_t> cmd{res->GetFamily(), res->GetBitIndex()};
    return {cmd, false};
  }

  return {};
}

MaterializedContents MaterializeFileContents(std::vector<std::string>* usernames,
                                             std::string_view file_contents) {
  // This is fine, a very large file will top at 1-2 mb. And that's for 5000+ users with 400
  // characters per line
  std::vector<std::string_view> commands = absl::StrSplit(file_contents, "\n");
  std::vector<std::vector<std::string_view>> materialized;
  materialized.reserve(commands.size());
  usernames->reserve(commands.size());
  for (auto& command : commands) {
    if (command.empty())
      continue;
    std::vector<std::string_view> cmds = absl::StrSplit(command, ' ');
    if (cmds[0] != "USER" || cmds.size() < 4) {
      return {};
    }

    usernames->push_back(std::string(cmds[1]));
    cmds.erase(cmds.begin(), cmds.begin() + 2);
    materialized.push_back(cmds);
  }
  return materialized;
}

using facade::ErrorReply;

template <typename T>
std::variant<User::UpdateRequest, ErrorReply> ParseAclSetUser(T args,
                                                              const CommandRegistry& registry,
                                                              bool hashed) {
  User::UpdateRequest req;

  for (auto& arg : args) {
    if (auto pass = MaybeParsePassword(facade::ToSV(arg), hashed); pass) {
      if (req.password) {
        return ErrorReply("Only one password is allowed");
      }
      req.password = std::move(pass);
      req.is_hashed = hashed;
      continue;
    }
    std::string buffer;
    std::string_view command;
    if constexpr (std::is_same_v<T, facade::CmdArgList>) {
      ToUpper(&arg);
      command = facade::ToSV(arg);
    } else {
      // Guaranteed SSO because commands are small
      buffer = arg;
      absl::Span<char> view{buffer.data(), buffer.size()};
      ToUpper(&view);
      command = buffer;
    }

    if (auto status = MaybeParseStatus(command); status) {
      if (req.is_active) {
        return ErrorReply("Multiple ON/OFF are not allowed");
      }
      req.is_active = *status;
      continue;
    }

    auto [cat, add] = MaybeParseAclCategory(command);
    if (cat) {
      using Sign = User::Sign;
      using Val = std::pair<Sign, uint32_t>;
      auto val = add ? Val{Sign::PLUS, *cat} : Val{Sign::MINUS, *cat};
      req.categories.push_back(val);
      continue;
    }

    auto [cmd, sign] = MaybeParseAclCommand(command, registry);
    if (!cmd) {
      return ErrorReply(absl::StrCat("Unrecognized parameter ", command));
    }

    using Sign = User::Sign;
    using Val = User::UpdateRequest::CommandsValueType;
    ;
    auto [index, bit] = *cmd;
    auto val = sign ? Val{Sign::PLUS, index, bit} : Val{Sign::MINUS, index, bit};
    req.commands.push_back(val);
  }

  return req;
}

using facade::CmdArgList;

template std::variant<User::UpdateRequest, ErrorReply>
ParseAclSetUser<std::vector<std::string_view>&>(std::vector<std::string_view>&,
                                                const CommandRegistry& registry, bool hashed);

template std::variant<User::UpdateRequest, ErrorReply> ParseAclSetUser<CmdArgList>(
    CmdArgList args, const CommandRegistry& registry, bool hashed);
}  // namespace dfly::acl
