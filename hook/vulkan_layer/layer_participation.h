#pragma once

// Whether CaptureEngine's Vulkan layer may enter the current process.
//
// Two images decide this with the same code: the negotiation gate
// (layer_gate.cpp, the library the implicit-layer manifest names, loaded into
// every Vulkan process on the machine) and the full layer (layer_main.cpp,
// loaded by the gate only for a process it admits). The policy itself is
// documented in common/graphics/vulkan_layer_target_list.h.
//
// Nothing here may depend on the full layer's runtime (IPC client, logging,
// Vulkan dispatch): the gate links this unit alone, so a declined process never
// maps more than the gate.

#include <windows.h>

#include <cstddef>

#include "common/ipc/shared_defs.h"

namespace ce::vulkan_layer_participation {

// The executable's base name in UTF-16. Path selectors additionally use the
// current full image path; the host whitelist (UTF-8) and persisted list
// (UTF-16) share the same matching policy.
void GetCurrentProcessBaseNameWide(wchar_t* out, size_t outSize);

// The same name in UTF-8 - the spelling `g_ProcessName` and the log lines use.
// Matching never compares these bytes: they make one round trip through
// Utf8ToWide and are compared in UTF-16 like everything else.
void GetCurrentProcessBaseName(char* out, size_t outSize);

struct HostEligibility {
    bool eligible = false;
    bool explicitlyDenied = false;
    DWORD inheritedParentPid = 0;
};

// Eligibility under a compatible published host: whitelisted by name, or the
// direct child renderer of the published profile target / active source. The
// narrow overload accepts the UTF-8 spelling and converts it once. An explicit
// denial forbids inheritance and takes precedence over a persisted admission.
HostEligibility EvaluateHostEligibility(const DiscoveryInfo* info, const wchar_t* processName);
HostEligibility EvaluateHostEligibility(const DiscoveryInfo* info, const char* processName);

// The injection selector list the host persisted for use while it is not
// running. Name and path selectors are compared in UTF-16.
bool IsListedAsResidentTarget();

// Whether a compatible host has debug logging on. `logsPath` (optional)
// receives the host's session logs directory, empty when none is published.
bool ReadPublishedHostLogging(char* logsPath, size_t logsPathSize);

struct Decision {
    bool participate = false;
    bool hostPublished = false;
    bool eligibleByHost = false;
    bool explicitlyDeniedByHost = false;
    bool listedTarget = false;
    DWORD inheritedParentPid = 0;
};

Decision DecideParticipation(const wchar_t* processName);

}  // namespace ce::vulkan_layer_participation
