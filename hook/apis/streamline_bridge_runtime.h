#pragma once

#include <windows.h>

#include <string>

#include "../common/streamline_api_generation.h"

// Bringing the CE-owned Streamline 2.x runtime up, for the generation bridge.
//
// Split out of streamline_bridge.cpp along the seam the restructure created: that unit owns
// the game's import slots and must be able to take them over in a few microseconds, while
// everything here is the slow half it deliberately runs behind - a LoadLibrary by full path
// and an `slInit` that maps a whole plugin set. Keeping them apart makes it obvious which
// side of that ordering any given change lands on.
namespace ce::streamline_bridge {

// The generation of the interposer the game itself is running. Read off the loaded module's
// own file, so it is available before CE has hooked anything.
ce::streamline_api::Generation ProcessGeneration();

// Which Streamline modules are resident and where they came from, as one line each.
//
// This is the measurement the first bridged sessions were missing: "the game already drove
// its own Streamline" named a decision without naming the state behind it, and a game one
// call into `slInit` is indistinguishable in that log from one that has already bound every
// feature plugin - while the two want opposite answers.
void LogStreamlineModuleInventory(const char* when);

// Loads the configured 2.x interposer by full path and initialises it with its plugin
// folder pinned. Returns nullptr on any failure, having said why; the caller then leaves
// every bridged call forwarding to the game's own 1.x runtime.
HMODULE LoadAndInitializeV2Runtime(const std::string& runtimeDir);

}  // namespace ce::streamline_bridge
