#include "ddraw_hook_internal.h"

#include "../common/legacy_d3d_texture_bindings.h"

#include <array>

// Ownership of the application's Direct3D 7 texture bindings.
//
// The native overlay sidecar brackets its draw with a `D3DSBT_ALL` state block.
// Such a block records each stage's texture as an unreferenced
// `IDirectDrawSurface7*`, so restoring it re-binds whatever the application had
// - including a surface the application has since released. Nothing else in a
// Direct3D 7 process ever re-binds an old texture, which is why an application
// releasing a still-bound surface is legal and why CE's restore is what turns
// it into an access violation. See `legacy_d3d_texture_bindings.h` for the
// crash signature this closes.
//
// CE therefore shadows every binding with a reference of its own. The shadow is
// written from `IDirect3DDevice7::SetTexture`, which a Direct3D 7 title calls
// once per material, so the uncontended-lock and single reference swap per
// changed binding are the entire cost; the runtime's own `SetTexture` already
// performs more reference traffic than that on the same call.

namespace {

struct ComTextureTraits {
    static void AddRef(void* texture) {
        static_cast<IUnknown*>(texture)->AddRef();
    }
    static void Release(void* texture) {
        static_cast<IUnknown*>(texture)->Release();
    }
};

using TextureBindingSet = ce::legacy_d3d::TextureBindingSet<ComTextureTraits>;

// A Direct3D 7 title owns one device, occasionally a second across a mode
// change. Four entries cover that with room to spare, and the least recently
// used entry is evicted - dropping its references and its restore proof -
// rather than letting a process that recreates devices accumulate them.
constexpr size_t kMaxTrackedDevices = 4;

struct DeviceTextureBindings {
    void* device = nullptr;
    bool ownedSinceCreation = false;
    uint32_t lastUse = 0;
    TextureBindingSet bindings;
};

std::mutex g_TextureBindingMutex;
std::array<std::unique_ptr<DeviceTextureBindings>, kMaxTrackedDevices> g_TextureBindings;
uint32_t g_TextureBindingUseCounter = 0;

DeviceTextureBindings* FindLocked(void* device) {
    for (auto& entry : g_TextureBindings) {
        if (entry && entry->device == device) {
            entry->lastUse = ++g_TextureBindingUseCounter;
            return entry.get();
        }
    }
    return nullptr;
}

DeviceTextureBindings* AcquireLocked(void* device) {
    if (auto* existing = FindLocked(device)) {
        return existing;
    }

    size_t slot = kMaxTrackedDevices;
    for (size_t i = 0; i < kMaxTrackedDevices; ++i) {
        if (!g_TextureBindings[i]) {
            slot = i;
            break;
        }
        if (slot == kMaxTrackedDevices || g_TextureBindings[i]->lastUse < g_TextureBindings[slot]->lastUse) {
            slot = i;
        }
    }
    if (slot >= kMaxTrackedDevices) {
        return nullptr;
    }
    if (g_TextureBindings[slot]) {
        HookLogImportant("DDraw: Dropping tracked D3D7 texture bindings for device=%p to track device=%p",
                         g_TextureBindings[slot]->device, device);
        g_TextureBindings[slot]->bindings.ReleaseAll(!HookIsShuttingDown());
    }
    g_TextureBindings[slot] = std::make_unique<DeviceTextureBindings>();
    g_TextureBindings[slot]->device = device;
    g_TextureBindings[slot]->lastUse = ++g_TextureBindingUseCounter;
    return g_TextureBindings[slot].get();
}

}  // namespace

void ResetLegacyD3D7TextureBindingsForNewDevice(void* ddraw_hook_device) {


    if (!ddraw_hook_device)
        return;

    std::lock_guard<std::mutex> lock(g_TextureBindingMutex);
    // A freshly created device has no textures bound, so resetting here is both
    // correct and the only defence against the allocator handing a new device
    // the address a destroyed one used to occupy.
    DeviceTextureBindings* entry = AcquireLocked(ddraw_hook_device);
    if (!entry)
        return;
    entry->bindings.ReleaseAll(!HookIsShuttingDown());
    entry->ownedSinceCreation = true;

}

void RecordLegacyD3D7TextureBinding(void* ddraw_hook_device,  DWORD ddraw_hook_stage,  void* ddraw_hook_texture) {


    if (!ddraw_hook_device)
        return;

    std::lock_guard<std::mutex> lock(g_TextureBindingMutex);
    DeviceTextureBindings* entry = FindLocked(ddraw_hook_device);
    if (!entry) {
        // The device existed before CE could observe its bindings. Track it so
        // the stage state stays consistent, but never claim the restore proof.
        entry = AcquireLocked(ddraw_hook_device);
        if (!entry)
            return;
    }
    entry->bindings.Bind(static_cast<size_t>(ddraw_hook_stage), ddraw_hook_texture);

}

bool LegacyD3D7TextureBindingsAreRestoreSafe(void* ddraw_hook_device) {


    if (!ddraw_hook_device)
        return false;

    // Seeing the device created is not enough on its own. The shadow is written
    // from the SetTexture detour, so an interception that never installed -
    // another overlay owning that slot, a vtable CE could not patch - would
    // leave the shadow empty while the device looked trustworthy, which is
    // exactly the state this gate exists to refuse.
    const LegacyD3DSamplerVTableRecord* record =
        ResolveLegacyD3DSamplerVTable(ce::legacy_d3d_sampler_state::Api::D3D7, ddraw_hook_device);
    if (!record || !record->setTexture.load(std::memory_order_acquire))
        return false;

    std::lock_guard<std::mutex> lock(g_TextureBindingMutex);
    const DeviceTextureBindings* entry = FindLocked(ddraw_hook_device);
    return entry && entry->ownedSinceCreation && entry->bindings.RestoreIsReferenceSafe();

}

IUnknown* AcquireLegacyD3D7TextureBinding(void* ddraw_hook_device,  DWORD ddraw_hook_stage) {


    if (!ddraw_hook_device)
        return nullptr;

    std::lock_guard<std::mutex> lock(g_TextureBindingMutex);
    DeviceTextureBindings* entry = FindLocked(ddraw_hook_device);
    if (!entry)
        return nullptr;
    auto* texture = static_cast<IUnknown*>(entry->bindings.Get(static_cast<size_t>(ddraw_hook_stage)));
    if (texture)
        texture->AddRef();
    return texture;

}

void ReleaseLegacyD3D7TextureBindings() {


    const bool releaseReferences = !HookIsShuttingDown();
    std::lock_guard<std::mutex> lock(g_TextureBindingMutex);
    for (auto& entry : g_TextureBindings) {
        if (!entry)
            continue;
        entry->bindings.ReleaseAll(releaseReferences);
        entry.reset();
    }

}

bool ReleaseLegacyD3D7TextureBindingsForDevice(void* ddraw_hook_device) {
    if (!ddraw_hook_device)
        return false;
    // "For as long as the device does": once the device is gone its bindings
    // can never be restored again, and the textures keep their DirectDraw
    // object alive. The entry goes too, so a new device at this address starts
    // without a borrowed restore proof.
    std::lock_guard<std::mutex> lock(g_TextureBindingMutex);
    for (auto& entry : g_TextureBindings) {
        if (!entry || entry->device != ddraw_hook_device)
            continue;
        entry->bindings.ReleaseAll(!HookIsShuttingDown());
        entry.reset();
        return true;
    }
    return false;
}

HRESULT STDMETHODCALLTYPE DetourSetTexture7(IDirect3DDevice7* ddraw_hook_device,  DWORD Stage, 
                                                   IDirectDrawSurface7* ddraw_hook_texture) {


    LegacyD3DSamplerVTableRecord* record =
        ResolveLegacyD3DSamplerVTable(ce::legacy_d3d_sampler_state::Api::D3D7, ddraw_hook_device);
    auto setTexture = record ? record->setTexture.load(std::memory_order_acquire) : nullptr;
    if (!setTexture)
        return DDERR_GENERIC;

    const HRESULT hr = setTexture(ddraw_hook_device, Stage, ddraw_hook_texture);
    // CE's own overlay binds run under LegacyD3DInternalScope and are not the
    // application's state; shadowing them would make the restore put CE's font
    // atlas back instead of the material the application had.
    if (SUCCEEDED(hr) && !HookIsShuttingDown() && !LegacyD3DInternalCallActive())
        RecordLegacyD3D7TextureBinding(ddraw_hook_device, Stage, ddraw_hook_texture);
    return hr;

}
