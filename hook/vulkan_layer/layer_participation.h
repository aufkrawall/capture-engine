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

// The executable's base name in the ANSI code page - the same spelling the host
// whitelist and every other layer-side process-name comparison use.
void GetCurrentProcessBaseName(char* out, size_t outSize);

bool IsProcessNameWhitelisted(const DiscoveryInfo* info, const char* processName);

// Eligibility under a compatible published host: whitelisted by name, or the
// direct child renderer of the published profile target / active source.
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

Decision DecideParticipation(const char* processName);

}  // namespace ce::vulkan_layer_participation
