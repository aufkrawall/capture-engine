#include "dx9_sampler_state.h"

#include <algorithm>
#include <atomic>
#include <mutex>

#include "dx9_sampler_state_internal.h"
#include "hook/runtime/hook_common.h"

// Texture metadata for the D3D9 sampler shadow: the level count and resource
// type of a bound texture decide whether forced AF applies to it. They come
// from the texture over COM, or - while CE sees every texture creation - from
// the per-device cache (dx9_sampler_config_policy.h, FindTrustedTextureMetadata).

namespace ce::dx9_sampler_state {

using detail::DeviceState;
using detail::FindExistingDevice;

namespace {

std::atomic<bool> g_textureCreationTracked{false};
std::atomic<bool> g_textureCreationLost{false};

UINT FullMipCount(UINT width, UINT height, UINT depth = 1) {
    UINT levels = 1;
    UINT extent = std::max(width, std::max(height, depth));
    while (extent > 1) {
        extent >>= 1;
        ++levels;
    }
    return levels;
}

// A single-level texture created with D3DUSAGE_AUTOGENMIPMAP reports one level
// but samples a full chain.
UINT EffectiveMipLevels(IDirect3DBaseTexture9* texture, D3DRESOURCETYPE type, UINT rawLevels) {
    if (rawLevels > 1)
        return rawLevels;
    switch (type) {
        case D3DRTYPE_TEXTURE: {
            D3DSURFACE_DESC desc;
            if (SUCCEEDED(static_cast<IDirect3DTexture9*>(texture)->GetLevelDesc(0, &desc)) &&
                (desc.Usage & D3DUSAGE_AUTOGENMIPMAP))
                return FullMipCount(desc.Width, desc.Height);
            break;
        }
        case D3DRTYPE_CUBETEXTURE: {
            D3DSURFACE_DESC desc;
            if (SUCCEEDED(static_cast<IDirect3DCubeTexture9*>(texture)->GetLevelDesc(0, &desc)) &&
                (desc.Usage & D3DUSAGE_AUTOGENMIPMAP))
                return FullMipCount(desc.Width, desc.Height);
            break;
        }
        case D3DRTYPE_VOLUMETEXTURE: {
            D3DVOLUME_DESC desc;
            if (SUCCEEDED(static_cast<IDirect3DVolumeTexture9*>(texture)->GetLevelDesc(0, &desc)) &&
                (desc.Usage & D3DUSAGE_AUTOGENMIPMAP))
                return FullMipCount(desc.Width, desc.Height, desc.Depth);
            break;
        }
        default:
            break;
    }
    return rawLevels;
}

}  // namespace

namespace detail {

bool TextureCreationTracked() {
    return g_textureCreationTracked.load(std::memory_order_acquire);
}

void UpdateTextureMetadata(DeviceState& deviceState, SamplerState& state, IDirect3DBaseTexture9* texture) {
    state.boundTexture = texture;
    state.textureBound = texture != nullptr;
    state.textureMipLevels = 0;
    state.textureUsesAddressW = false;
    state.textureSupportsAnisotropy = false;
    state.textureSupportsMagAnisotropy = false;
    if (!texture)
        return;

    D3DRESOURCETYPE type = D3DRTYPE_TEXTURE;
    TextureMetaCacheEntry& entry = deviceState.textureMetaCache[HashTexturePointer(texture)];
    if (const TextureMetaCacheEntry* cached = FindTrustedTextureMetadata(
            deviceState.textureMetaCache, texture, TextureCreationTracked())) {
        ++deviceState.textureMetadataCacheHits;
        type = static_cast<D3DRESOURCETYPE>(cached->type);
        state.textureMipLevels = cached->effectiveMipLevels;
    } else {
        ++deviceState.textureMetadataQueries;
        const UINT rawLevels = texture->GetLevelCount();
        type = texture->GetType();
        // Untracked, an entry still saves the level-0 query of a single-level
        // texture whose level count and type match.
        const bool sameTexture = entry.texture == texture && entry.rawLevelCount == rawLevels &&
                                 entry.type == static_cast<DWORD>(type);
        state.textureMipLevels = sameTexture ? entry.effectiveMipLevels : EffectiveMipLevels(texture, type, rawLevels);
        entry = {texture, rawLevels, static_cast<DWORD>(type), state.textureMipLevels};
    }

    DWORD filterCaps = 0;
    switch (type) {
        case D3DRTYPE_TEXTURE:
            filterCaps = deviceState.textureFilterCaps;
            break;
        case D3DRTYPE_CUBETEXTURE:
            filterCaps = deviceState.cubeTextureFilterCaps;
            break;
        case D3DRTYPE_VOLUMETEXTURE:
            state.textureUsesAddressW = true;
            filterCaps = deviceState.volumeTextureFilterCaps;
            break;
        default:
            return;
    }
    state.textureSupportsAnisotropy = FilterCapsAllowMinAnisotropy(filterCaps);
    state.textureSupportsMagAnisotropy = FilterCapsAllowMagAnisotropy(filterCaps);
}

}  // namespace detail

void OnTextureCreated(IDirect3DDevice9* device, const void* texture) {
    if (!device || !texture)
        return;
    DeviceState* deviceState = FindExistingDevice(device);
    if (!deviceState)
        return;
    detail::ShadowLock lock(*deviceState, 0);
    ForgetCreatedTexture(deviceState->textureMetaCache, texture);
}

void OnDispatchTableRewritten(IDirect3DDevice9* device, bool multithreaded) {
    // A single-threaded device is called by one thread at a time: nothing could
    // pass the rewritten table before the hooks were back.
    if (!device || !multithreaded)
        return;
    DeviceState* deviceState = FindExistingDevice(device);
    if (!deviceState)
        return;
    detail::ShadowLock lock(*deviceState, 0);
    deviceState->textureMetaCache = {};
    static std::atomic<int> s_logCount{0};
    if (s_logCount.fetch_add(1, std::memory_order_relaxed) < 4) {
        HookLogImportant("DX9: Dispatch table of multithreaded device %p was rewritten; its cached texture metadata "
                         "is dropped (a texture created meanwhile was not seen)",
                         device);
    }
}

void SetTextureCreationTracked(bool tracked, const char* reason) {
    if (tracked) {
        if (g_textureCreationLost.load(std::memory_order_acquire) ||
            g_textureCreationTracked.exchange(true, std::memory_order_acq_rel))
            return;
        HookLogImportant("DX9: Texture creation tracked (%s); SetTexture answers texture metadata from its cache",
                         reason ? reason : "unknown");
        return;
    }
    // Entries filled while tracked stay in the caches but are never trusted again.
    g_textureCreationTracked.store(false, std::memory_order_release);
    if (!g_textureCreationLost.exchange(true, std::memory_order_acq_rel)) {
        // A creation CE missed before noticing may have reused a bound
        // texture's address: every stage asks its next texture afresh.
        detail::ForgetBoundTextures();
        HookLogImportant("DX9: Texture creation no longer tracked (%s); SetTexture asks each new texture for its "
                         "metadata again",
                         reason ? reason : "unknown");
    }
}

}  // namespace ce::dx9_sampler_state
