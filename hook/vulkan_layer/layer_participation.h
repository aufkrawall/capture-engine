#pragma once

// Whether CaptureEngine's Vulkan layer may enter the current process.
//
// Two images decide this with the same code: the negotiation gate
// (layer_gate.cpp, the library the implicit-layer manifest names, loaded into
// every Vulkan process on the machine) and the full layer (layer_main.cpp,
// loaded by the gate only for a process it admits). The policy itself is
// documented in common/vulkan_layer_target_list.h.
//
// Nothing here may depend on the full layer's runtime (IPC client, logging,
// Vulkan dispatch): the gate links this unit alone, so a declined process never
// maps more than the gate.

#include <windows.h>

#include <cstddef>

#include "../../common/shared_defs.h"

namespace ce::vulkan_layer_participation {

// The executable's base name in UTF-16 - the one name source every
// participation match runs on. The host whitelist (UTF-8, converted once per
// entry in common/vulkan_layer_target_list.h) and the persisted list (UTF-16)
// both match against this spelling.
void GetCurrentProcessBaseNameWide(wchar_t* out, size_t outSize);

// The same name in UTF-8 - the spelling `g_ProcessName` and the log lines use.
// Matching never compares these bytes: they make one round trip through
// Utf8ToWide and are compared in UTF-16 like everything else.
void GetCurrentProcessBaseName(char* out, size_t outSize);

bool IsProcessNameWhitelisted(const DiscoveryInfo* info, const wchar_t* processName);

// Eligibility under a compatible published host: whitelisted by name, or the
// direct child renderer of the published profile target / active source. The
// narrow overload accepts the UTF-8 spelling and converts it once.
bool IsProcessEligibleByDiscovery(const DiscoveryInfo* info, const wchar_t* processName, DWORD* inheritedParentPid);
bool IsProcessEligibleByDiscovery(const DiscoveryInfo* info, const char* processName, DWORD* inheritedParentPid);

// True when a CaptureEngine host with this image's shared-memory layout has
// published its discovery mapping.
bool IsCompatibleHostPublished();

// The injection whitelist the host persisted for use while it is not running.
// Compared in UTF-16, the form both the list and the executable path have.
bool IsListedAsResidentTarget();

// Whether a compatible host has debug logging on. `logsPath` (optional)
// receives the host's session logs directory, empty when none is published.
bool ReadPublishedHostLogging(char* logsPath, size_t logsPathSize);

struct Decision {
    bool participate = false;
    bool hostPublished = false;
    bool eligibleByHost = false;
    bool listedTarget = false;
    DWORD inheritedParentPid = 0;
};

Decision DecideParticipation(const wchar_t* processName);

}  // namespace ce::vulkan_layer_participation
