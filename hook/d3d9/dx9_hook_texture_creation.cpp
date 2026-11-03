#include "dx9_hook_internal.h"

// Texture creation on a D3D9 device vtable, watched so the forced-AF texture
// metadata cache can answer SetTexture without asking the texture
// (dx9_sampler_config_policy.h, FindTrustedTextureMetadata): an address names a
// different texture only after one of these three calls returned it. The slots
// are armed with the sampler override, before SetTexture, and creation is rare,
// so the detours cost nothing per draw. The cache is trusted only while all
// three are CE's; a failed install or a drifted slot ends that for good.

namespace {

constexpr int kCreateTextureSlot = 23;
constexpr int kCreateVolumeTextureSlot = 24;
constexpr int kCreateCubeTextureSlot = 25;

void NoteTextureCreated(IDirect3DDevice9* device, HRESULT hr, const void* texture) {
    if (SUCCEEDED(hr) && texture && !HookIsShuttingDown())
        ce::dx9_sampler_state::OnTextureCreated(device, texture);
}

}  // namespace

HRESULT STDMETHODCALLTYPE DetourCreateTexture(IDirect3DDevice9* device, UINT width, UINT height, UINT levels,
                                              DWORD usage, D3DFORMAT format, D3DPOOL pool,
                                              IDirect3DTexture9** texture, HANDLE* sharedHandle) {
    D3D9SamplerVTableRecord* record = ResolveD3D9SamplerVTableRecord(device);
    const CreateTexture_t original = record ? record->createTexture.load(std::memory_order_acquire) : nullptr;
    if (!original)
        return D3DERR_INVALIDCALL;
    const HRESULT hr = original(device, width, height, levels, usage, format, pool, texture, sharedHandle);
    NoteTextureCreated(device, hr, texture ? *texture : nullptr);
    return hr;
}

HRESULT STDMETHODCALLTYPE DetourCreateVolumeTexture(IDirect3DDevice9* device, UINT width, UINT height, UINT depth,
                                                    UINT levels, DWORD usage, D3DFORMAT format, D3DPOOL pool,
                                                    IDirect3DVolumeTexture9** texture, HANDLE* sharedHandle) {
    D3D9SamplerVTableRecord* record = ResolveD3D9SamplerVTableRecord(device);
    const CreateVolumeTexture_t original =
        record ? record->createVolumeTexture.load(std::memory_order_acquire) : nullptr;
    if (!original)
        return D3DERR_INVALIDCALL;
    const HRESULT hr = original(device, width, height, depth, levels, usage, format, pool, texture, sharedHandle);
    NoteTextureCreated(device, hr, texture ? *texture : nullptr);
    return hr;
}

HRESULT STDMETHODCALLTYPE DetourCreateCubeTexture(IDirect3DDevice9* device, UINT edgeLength, UINT levels, DWORD usage,
                                                  D3DFORMAT format, D3DPOOL pool, IDirect3DCubeTexture9** texture,
                                                  HANDLE* sharedHandle) {
    D3D9SamplerVTableRecord* record = ResolveD3D9SamplerVTableRecord(device);
    const CreateCubeTexture_t original = record ? record->createCubeTexture.load(std::memory_order_acquire) : nullptr;
    if (!original)
        return D3DERR_INVALIDCALL;
    const HRESULT hr = original(device, edgeLength, levels, usage, format, pool, texture, sharedHandle);
    NoteTextureCreated(device, hr, texture ? *texture : nullptr);
    return hr;
}

D3D9TextureCreationEvent HookD3D9TextureCreationSlotsLocked(D3D9SamplerVTableRecord* record, uintptr_t* vtable) {
    if (record->textureCreationHookAttempted)
        return D3D9TextureCreationEvent::kNone;
    record->textureCreationHookAttempted = true;
    // The detour may run on another thread the moment its slot is written, so
    // the original is published first.
    record->createTexture.store(reinterpret_cast<CreateTexture_t>(vtable[kCreateTextureSlot]),
                                std::memory_order_release);
    record->createVolumeTexture.store(reinterpret_cast<CreateVolumeTexture_t>(vtable[kCreateVolumeTextureSlot]),
                                      std::memory_order_release);
    record->createCubeTexture.store(reinterpret_cast<CreateCubeTexture_t>(vtable[kCreateCubeTextureSlot]),
                                    std::memory_order_release);

    CreateTexture_t texture = nullptr;
    CreateVolumeTexture_t volume = nullptr;
    CreateCubeTexture_t cube = nullptr;
    const bool textureHooked =
        VTableHook::Create(&vtable[kCreateTextureSlot], reinterpret_cast<void*>(&DetourCreateTexture),
                           reinterpret_cast<void**>(&texture)) == VTableHook::Success;
    if (textureHooked)
        record->createTexture.store(texture, std::memory_order_release);
    const bool volumeHooked =
        VTableHook::Create(&vtable[kCreateVolumeTextureSlot], reinterpret_cast<void*>(&DetourCreateVolumeTexture),
                           reinterpret_cast<void**>(&volume)) == VTableHook::Success;
    if (volumeHooked)
        record->createVolumeTexture.store(volume, std::memory_order_release);
    const bool cubeHooked =
        VTableHook::Create(&vtable[kCreateCubeTextureSlot], reinterpret_cast<void*>(&DetourCreateCubeTexture),
                           reinterpret_cast<void**>(&cube)) == VTableHook::Success;
    if (cubeHooked)
        record->createCubeTexture.store(cube, std::memory_order_release);

    record->textureCreationHooked = textureHooked && volumeHooked && cubeHooked;
    if (record->textureCreationHooked) {
        HookLogImportant("DX9: Texture creation hooks installed for vtable=%p", vtable);
        return D3D9TextureCreationEvent::kTracked;
    }
    HookLogImportant("DX9: Texture creation hooks FAILED for vtable=%p texture=%d volume=%d cube=%d", vtable,
                     textureHooked ? 1 : 0, volumeHooked ? 1 : 0, cubeHooked ? 1 : 0);
    return D3D9TextureCreationEvent::kInstallFailed;
}

D3D9TextureCreationEvent CheckD3D9TextureCreationHookDriftLocked(D3D9SamplerVTableRecord* record,
                                                                 uintptr_t* vtable) {
    if (!record->textureCreationHooked || record->textureCreationDrifted)
        return D3D9TextureCreationEvent::kNone;
    // A slot that no longer holds CE's detour may lead to the implementation
    // without passing it, and a creation CE does not see can reuse a cached
    // address. Unprovable either way, so the cache stops being trusted.
    const bool intact =
        reinterpret_cast<void*>(vtable[kCreateTextureSlot]) == reinterpret_cast<void*>(&DetourCreateTexture) &&
        reinterpret_cast<void*>(vtable[kCreateVolumeTextureSlot]) ==
            reinterpret_cast<void*>(&DetourCreateVolumeTexture) &&
        reinterpret_cast<void*>(vtable[kCreateCubeTextureSlot]) == reinterpret_cast<void*>(&DetourCreateCubeTexture);
    if (intact)
        return D3D9TextureCreationEvent::kNone;
    record->textureCreationDrifted = true;
    HookLogImportant("DX9: Texture creation hook drift for vtable=%p (slot23=%p slot24=%p slot25=%p)", vtable,
                     reinterpret_cast<void*>(vtable[kCreateTextureSlot]),
                     reinterpret_cast<void*>(vtable[kCreateVolumeTextureSlot]),
                     reinterpret_cast<void*>(vtable[kCreateCubeTextureSlot]));
    return D3D9TextureCreationEvent::kDrifted;
}

void ReportD3D9TextureCreationEvent(D3D9TextureCreationEvent event) {
    switch (event) {
        case D3D9TextureCreationEvent::kTracked:
            ce::dx9_sampler_state::SetTextureCreationTracked(true, "creation hooks installed");
            break;
        case D3D9TextureCreationEvent::kInstallFailed:
            ce::dx9_sampler_state::SetTextureCreationTracked(false, "a creation hook failed to install");
            break;
        case D3D9TextureCreationEvent::kDrifted:
            ce::dx9_sampler_state::SetTextureCreationTracked(false, "a creation slot drifted");
            break;
        case D3D9TextureCreationEvent::kNone:
            break;
    }
}
