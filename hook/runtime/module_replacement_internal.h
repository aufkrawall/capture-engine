#pragma once

#include <windows.h>

#include <cstddef>

// Shared between the two halves of the module replacement (module_replacement.cpp owns the
// published replacements and the caller policy, module_replacement_exports.cpp the entry-point
// forwarders); not an interface for anything else.
namespace ce::module_replacement::detail {

// Whether a call from `callerAddress` that reached a superseded entry point continues in the
// replacement (ShouldForwardEntryCall for that caller). False until a replacement is published.
bool ForwardsEntryCallFrom(const void* callerAddress);

struct EntryForwardTally {
    size_t installed = 0;
    size_t leftToForeignOwner = 0;  // another component patched the entry first; CE wrote nothing there
    size_t nativeAliases = 0;  // replacement export forwards back to the superseded runtime
    size_t missingInReplacement = 0;
    size_t failed = 0;
    const char* firstMissing = nullptr;  // a static export name
};

// Installs forwarders on the superseded image's known runtime entry points (dxgi, d3d12, d3d11,
// d3d10, d3d10_1) that the replacement exports too. Once per replaced name.
EntryForwardTally InstallEntryForwarders(const char* baseName, HMODULE superseded, HMODULE replacement);

}  // namespace ce::module_replacement::detail
