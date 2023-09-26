// Copyright 2022, DragonflyDB authors.  All rights reserved.
// See LICENSE for licensing terms.
//

#pragma once

namespace dfly {

extern const char kGitTag[];
extern const char kGitSha[];
extern const char kGitClean[];
extern const char kBuildTime[];

const char* GetVersion();

// An enum for internal versioning of dragonfly specific behavior.
// Please document for each new entry what the behavior changes are
// and to which released versions this corresponds.
enum class DflyVersion {
  //         ver <= 1.3
  VER0,

  // 1.4  <= ver <= 1.10
  // - Supports receiving ACKs from replicas
  // - Sends version back on REPLCONF capa dragonfly
  VER1,

  // 1.11 <= ver
  // Supports limited partial sync
  VER2,

  // Always points to the latest version
  CURRENT_VER = VER2,
};

}  // namespace dfly
