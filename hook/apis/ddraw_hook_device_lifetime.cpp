#include "ddraw_hook_internal.h"

#include "../common/ddraw_chain_lifetime_policy.h"

#include <atomic>

// CE's references to the application's Direct3D 7 device end inside the
// application's own last Release.
//
// CE references the device twice - the tracker the native route acquires it
// from, and the sidecar that draws with it. A Direct3D 7 device is torn down
// through its render target, so it has to die before the application releases
// the surfaces it renders to; applications do exactly that. A CE reference
// that outlives the application's breaks the order: the device survives its
// own release and dies later, whenever CE lets go, after the chain is gone.
// Gothic II session 20260924_235830 faulted in ~CDirect3DDevice7 ->
// ~CDirect3DDeviceIDP2 -> D3DFree reading null, reached from CE releasing the
// tracked device before a primary creation. (Holding it indefinitely instead
// kept the chain alive and failed the creation, session 20260924_233030.)
//
// So the Release interception looks at what is left after the application's
// Release: when every remaining reference is CE's, the application has let go,
// and CE releases its own right there - the render target still exists,
// because the application is still inside its device Release.

namespace {

std::atomic<D3D7DeviceRelease_t> g_fallbackRelease{nullptr};

uint32_t CountCeDeviceReferences(void* device) {
    return (TrackedLegacyD3D7DeviceIs(device) ? 1u : 0u) + (NativeLegacyD3DOverlayHoldsDevice(device) ? 1u : 0u);
}

}  // namespace

ULONG STDMETHODCALLTYPE DetourD3D7DeviceRelease(void* ddraw_hook_device) {
    LegacyD3DSamplerVTableRecord* record =
        ResolveLegacyD3DSamplerVTable(ce::legacy_d3d_sampler_state::Api::D3D7, ddraw_hook_device);
    D3D7DeviceRelease_t release = record ? record->release.load(std::memory_order_acquire) : nullptr;
    if (!release)
        release = g_fallbackRelease.load(std::memory_order_acquire);
    if (!release)
        return 0;
    // CE's own releases run under the internal scope and must not recurse into
    // this decision; teardown leaves everything to the process exit.
    if (HookIsShuttingDown() || LegacyD3DInternalCallActive())
        return release(ddraw_hook_device);

    // Pointer comparisons only: counted before the call, because afterwards the
    // device may no longer exist when CE holds nothing.
    const uint32_t ceReferences = CountCeDeviceReferences(ddraw_hook_device);
    const ULONG remaining = release(ddraw_hook_device);
    if (!ce::ddraw_chain_lifetime::ApplicationReleasedLastDeviceReference(ceReferences, remaining))
        return remaining;

    uint32_t dropped = 0;
    bool texturesDropped = false;
    {
        LegacyD3DInternalScope internalScope;
        // The sidecar first: its state block is deleted on the still-live device.
        dropped += ReleaseNativeLegacyD3DOverlayForDevice(ddraw_hook_device) ? 1u : 0u;
        dropped += ReleaseTrackedLegacyD3D7DeviceIf(ddraw_hook_device) ? 1u : 0u;
    }
    // The device is gone now; its texture shadow is keyed by the pointer only.
    texturesDropped = ReleaseLegacyD3D7TextureBindingsForDevice(ddraw_hook_device);
    HookLogImportant(
        "DDraw: Application released its last reference to D3D7 device=%p; CE dropped its %u of %u reference(s) "
        "inside that Release (textureShadow=%d)",
        ddraw_hook_device, dropped, ceReferences, texturesDropped ? 1 : 0);
    // What the application would have seen without CE.
    return remaining > dropped ? remaining - dropped : 0;
}

void InstallD3D7DeviceReleaseHook(LegacyD3DSamplerVTableRecord* record, void** vtable) {
    if (!record || !vtable || record->release.load(std::memory_order_acquire))
        return;
    D3D7DeviceRelease_t original = nullptr;
    const VTableHook::Status status =
        VTableHook::Create(reinterpret_cast<void*>(&vtable[D3D7_VTABLE_RELEASE]),
                           reinterpret_cast<LPVOID>(&DetourD3D7DeviceRelease), reinterpret_cast<LPVOID*>(&original));
    if (status == VTableHook::Success && original) {
        record->release.store(original, std::memory_order_release);
        D3D7DeviceRelease_t expected = nullptr;
        g_fallbackRelease.compare_exchange_strong(expected, original, std::memory_order_acq_rel);
        HookLog("DDraw: D3D7 device Release interception installed (vtable=%p)", vtable);
        return;
    }
    HookLogImportant("DDraw: D3D7 device Release hook FAILED for vtable=%p (status=%d); CE will not hold this "
                     "device, so the native sidecar stays off and the CPU composite draws the overlay",
                     vtable, static_cast<int>(status));
}

bool LegacyD3D7DeviceReleaseIsIntercepted(void* device) {
    const LegacyD3DSamplerVTableRecord* record =
        device ? ResolveLegacyD3DSamplerVTable(ce::legacy_d3d_sampler_state::Api::D3D7, device) : nullptr;
    return record && record->release.load(std::memory_order_acquire) != nullptr;
}
