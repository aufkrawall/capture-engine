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

// The native adapter guard (module_replacement_adapters.cpp): while dxgi.dll is replaced, System32's
// D3D10/D3D11 device-creation entries receive System32's adapter in place of the replacement's.
struct AdapterGuardTally {
    size_t installed = 0;
    size_t leftToForeignOwner = 0;
    size_t missing = 0;
    size_t failed = 0;
};

// The runtimes the guard covers ("d3d11.dll", "d3d10.dll", "d3d10_1.dll").
const char* const* NativeAdapterRuntimeNames(size_t* count);
// Which dxgi.dll is System32's and which replaces it; published before any guard is installed.
void PublishDxgiPairing(HMODULE systemDxgi, HMODULE replacementDxgi);
// Guards the device-creation entries of `runtime`, a loaded System32 image named `baseName`.
AdapterGuardTally InstallNativeAdapterGuards(const char* baseName, HMODULE runtime);

}  // namespace ce::module_replacement::detail
