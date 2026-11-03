#include "custom_overlay_d3d7.h"

#ifndef DIRECTDRAW_VERSION
#define DIRECTDRAW_VERSION 0x0700
#endif

#ifndef DIRECT3D_VERSION
#define DIRECT3D_VERSION 0x0700
#endif

#include <ddraw.h>

#include <d3d.h>

#include <cstring>

#include "hook/runtime/hook_common.h"

// This translation unit is the only place the legacy Direct3D headers are
// visible; `d3dtypes.h` and `d3d9types.h` define the same enumerator names and
// cannot coexist. Nothing here may be moved into a header.

namespace CustomOverlay {

namespace {

IDirect3DDevice7* Device(void* opaque) {
    return static_cast<IDirect3DDevice7*>(opaque);
}

IDirectDraw7* DirectDraw(void* opaque) {
    return static_cast<IDirectDraw7*>(opaque);
}

IDirectDrawSurface7* Surface(void* opaque) {
    return static_cast<IDirectDrawSurface7*>(opaque);
}

// The atlas is uploaded as straight ARGB8888. DX6/DX7 hardware that reaches a
// modern driver always exposes it; nothing older than that runs this path.
bool IsArgb8888(const DDPIXELFORMAT& format) {
    return (format.dwFlags & DDPF_RGB) != 0 && (format.dwFlags & DDPF_ALPHAPIXELS) != 0 &&
           format.dwRGBBitCount == 32 && format.dwRBitMask == 0x00FF0000u && format.dwGBitMask == 0x0000FF00u &&
           format.dwBBitMask == 0x000000FFu && format.dwRGBAlphaBitMask == 0xFF000000u;
}

struct TextureFormatSearch {
    DDPIXELFORMAT format = {};
    bool found = false;
};

HRESULT WINAPI EnumTextureFormatsCallback(DDPIXELFORMAT* format, void* context) {
    auto* search = static_cast<TextureFormatSearch*>(context);
    if (format && search && !search->found && IsArgb8888(*format)) {
        search->format = *format;
        search->found = true;
        return D3DENUMRET_CANCEL;
    }
    return D3DENUMRET_OK;
}

// Stage 0's filter and address states, saved and restored by hand around each
// draw instead of through the state block: Wine's D3D7 applies these
// sampler-type stage states immediately even while a block is recording, so a
// recorded block would neither hold them nor leave the device alone.
constexpr D3DTEXTURESTAGESTATETYPE kStageZeroSamplerStates[] = {
    D3DTSS_MINFILTER, D3DTSS_MAGFILTER, D3DTSS_MIPFILTER, D3DTSS_ADDRESSU, D3DTSS_ADDRESSV,
};
constexpr DWORD kStageZeroSamplerValues[] = {
    D3DTFN_LINEAR, D3DTFG_LINEAR, D3DTFP_NONE, D3DTADDRESS_CLAMP, D3DTADDRESS_CLAMP,
};

void SetStageZeroMode(IDirect3DDevice7* dev, bool useTexture, IDirectDrawSurface7* fontSurface) {
    if (useTexture) {
        dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
        dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        dev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
        dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
        dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
        dev->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
        dev->SetTexture(0, fontSurface);
    } else {
        dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
        dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
        dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
        dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
        dev->SetTexture(0, nullptr);
    }
}

// Every state the overlay's draw sets apart from kStageZeroSamplerStates, in
// one place: the draw calls it, and so does the recording of the state block
// that saves and restores the application's values. Stage 0's colour/alpha
// operations and texture are recorded through the textured SetStageZeroMode,
// which covers the states of both variants.
void SetOverlayDeviceState(IDirect3DDevice7* dev, D3DVIEWPORT7* viewport) {
    dev->SetViewport(viewport);

    dev->SetRenderState(D3DRENDERSTATE_ALPHABLENDENABLE, TRUE);
    dev->SetRenderState(D3DRENDERSTATE_SRCBLEND, D3DBLEND_SRCALPHA);
    dev->SetRenderState(D3DRENDERSTATE_DESTBLEND, D3DBLEND_INVSRCALPHA);
    dev->SetRenderState(D3DRENDERSTATE_ALPHATESTENABLE, FALSE);
    dev->SetRenderState(D3DRENDERSTATE_ZENABLE, FALSE);
    dev->SetRenderState(D3DRENDERSTATE_ZWRITEENABLE, FALSE);
    dev->SetRenderState(D3DRENDERSTATE_STENCILENABLE, FALSE);
    dev->SetRenderState(D3DRENDERSTATE_CULLMODE, D3DCULL_NONE);
    dev->SetRenderState(D3DRENDERSTATE_LIGHTING, FALSE);
    dev->SetRenderState(D3DRENDERSTATE_FOGENABLE, FALSE);
    dev->SetRenderState(D3DRENDERSTATE_SPECULARENABLE, FALSE);
    dev->SetRenderState(D3DRENDERSTATE_COLORKEYENABLE, FALSE);
    dev->SetRenderState(D3DRENDERSTATE_SHADEMODE, D3DSHADE_GOURAUD);
    dev->SetRenderState(D3DRENDERSTATE_DITHERENABLE, FALSE);
    // Transformed vertices are already inside the viewport; clipping them costs
    // the runtime a per-vertex pass for nothing.
    dev->SetRenderState(D3DRENDERSTATE_CLIPPING, FALSE);

    dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    dev->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
}

}  // namespace

D3D7Backend::D3D7Backend(void* dev) : device(dev) {
    if (device) {
        Device(device)->AddRef();
    }
}

D3D7Backend::~D3D7Backend() {
    Shutdown();
    if (device) {
        Device(device)->Release();
        device = nullptr;
    }
}

bool D3D7Backend::CreateFontSurface(int width, int height, const uint8_t* pixels) {
    if (!directDraw || !device || width <= 0 || height <= 0 || !pixels)
        return false;

    TextureFormatSearch search;
    if (FAILED(Device(device)->EnumTextureFormats(EnumTextureFormatsCallback, &search)) || !search.found) {
        HookLogImportant("[Overlay] D3D7: no ARGB8888 texture format available for the font atlas");
        return false;
    }

    DDSURFACEDESC2 desc = {};
    desc.dwSize = sizeof(desc);
    desc.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
    desc.dwWidth = static_cast<DWORD>(width);
    desc.dwHeight = static_cast<DWORD>(height);
    desc.ddsCaps.dwCaps = DDSCAPS_TEXTURE;
    // Let the driver own residency: the atlas is written once and then only
    // sampled, so a managed surface never costs a transfer on the present path.
    desc.ddsCaps.dwCaps2 = DDSCAPS2_TEXTUREMANAGE;
    desc.ddpfPixelFormat = search.format;

    IDirectDrawSurface7* surface = nullptr;
    HRESULT hr = DirectDraw(directDraw)->CreateSurface(&desc, &surface, nullptr);
    if (FAILED(hr) || !surface) {
        HookLogImportant("[Overlay] D3D7: font atlas surface creation failed (hr=0x%08X)", static_cast<unsigned>(hr));
        return false;
    }

    DDSURFACEDESC2 locked = {};
    locked.dwSize = sizeof(locked);
    // DDLOCK_NOSYSLOCK is not negotiable here: this runs on the application's
    // render thread inside its Flip, and without the flag a DDLOCK_WAIT lock
    // takes the Win16 lock in a process that also hosts other overlays (the
    // Gothic II session 20260916_005504 hang class). A rejected lock fails the
    // atlas upload and the caller falls back to the CPU composite.
    hr = surface->Lock(nullptr, &locked,
                       DDLOCK_WAIT | DDLOCK_WRITEONLY | DDLOCK_SURFACEMEMORYPTR | DDLOCK_NOSYSLOCK, nullptr);
    if (FAILED(hr) || !locked.lpSurface) {
        HookLogImportant("[Overlay] D3D7: font atlas lock failed (hr=0x%08X)", static_cast<unsigned>(hr));
        surface->Release();
        return false;
    }

    for (int y = 0; y < height; ++y) {
        const uint8_t* src = pixels + static_cast<size_t>(y) * static_cast<size_t>(width) * 4u;
        uint8_t* dst = static_cast<uint8_t*>(locked.lpSurface) + static_cast<ptrdiff_t>(locked.lPitch) * y;
        for (int x = 0; x < width; ++x) {
            dst[0] = src[2];
            dst[1] = src[1];
            dst[2] = src[0];
            dst[3] = src[3];
            dst += 4;
            src += 4;
        }
    }
    hr = surface->Unlock(nullptr);
    if (FAILED(hr)) {
        HookLogImportant("[Overlay] D3D7: font atlas unlock failed (hr=0x%08X)", static_cast<unsigned>(hr));
        surface->Release();
        return false;
    }
    fontSurface = surface;
    return true;
}

bool D3D7Backend::Initialize(int fontTextureWidth, int fontTextureHeight, const uint8_t* fontTextureData) {
    if (initialized || !device)
        return false;

    IDirect3D7* d3d = nullptr;
    if (FAILED(Device(device)->GetDirect3D(&d3d)) || !d3d) {
        HookLogImportant("[Overlay] D3D7: device does not expose its IDirect3D7");
        return false;
    }
    // The Direct3D and DirectDraw interfaces are the same object, so the
    // texture surface is created from the DirectDraw the game already owns.
    IDirectDraw7* ddraw = nullptr;
    const HRESULT ddHr = d3d->QueryInterface(IID_IDirectDraw7, reinterpret_cast<void**>(&ddraw));
    d3d->Release();
    if (FAILED(ddHr) || !ddraw) {
        HookLogImportant("[Overlay] D3D7: IDirectDraw7 unavailable from the device (hr=0x%08X)",
                         static_cast<unsigned>(ddHr));
        return false;
    }
    directDraw = ddraw;

    if (!CreateFontSurface(fontTextureWidth, fontTextureHeight, fontTextureData)) {
        ddraw->Release();
        directDraw = nullptr;
        return false;
    }

    // Resource and state-block creation happens while priming at Flip, outside
    // the application's active scene. EndScene then performs draw calls only.
    //
    // A D3DSBT_ALL block captured and re-applied every render state, all eight
    // texture stages and their textures, transforms, lights, material, clip
    // planes and the viewport around each overlay draw - on a single-threaded
    // DX7 title, on the thread that renders the game. A recorded block holds
    // only the states set while recording, and CaptureStateBlock refreshes
    // exactly those, so the overlay saves and restores what it changes. The
    // recording runs under LegacyD3DInternalScope like the rest of priming, so
    // neither the texture-binding shadow nor the sampler shadow takes its calls
    // for the application's.
    IDirect3DDevice7* dev = Device(device);
    DWORD handle = 0;
    HRESULT stateHr = dev->BeginStateBlock();
    if (SUCCEEDED(stateHr)) {
        D3DVIEWPORT7 recordedViewport = {0, 0, 1, 1, 0.0f, 1.0f};
        SetOverlayDeviceState(dev, &recordedViewport);
        SetStageZeroMode(dev, true, Surface(fontSurface));
        stateHr = dev->EndStateBlock(&handle);
    }
    if (FAILED(stateHr) || handle == 0) {
        HookLogImportant("[Overlay] D3D7: state block creation failed (hr=0x%08X)",
                         static_cast<unsigned>(stateHr));
        Surface(fontSurface)->Release();
        fontSurface = nullptr;
        ddraw->Release();
        directDraw = nullptr;
        return false;
    }
    stateBlock = static_cast<uint32_t>(handle);

    needsReinitialize = false;
    initialized = true;
    HookLogImportant("[Overlay] D3D7 backend initialized (device=%p atlas=%dx%d)", device, fontTextureWidth,
                     fontTextureHeight);
    return true;
}

void D3D7Backend::Shutdown() {
    if (stateBlock && device) {
        Device(device)->DeleteStateBlock(stateBlock);
    }
    stateBlock = 0;
    if (fontSurface) {
        Surface(fontSurface)->Release();
        fontSurface = nullptr;
    }
    if (directDraw) {
        DirectDraw(directDraw)->Release();
        directDraw = nullptr;
    }
    scratchVertices.clear();
    scratchVertices.shrink_to_fit();
    lastUseTexture = false;
    lastRenderSucceeded = false;
    needsReinitialize = false;
    initialized = false;
}

void D3D7Backend::ApplyStageMode(bool useTexture) {
    SetStageZeroMode(Device(device), useTexture, Surface(fontSurface));
    lastUseTexture = useTexture;
}

void D3D7Backend::Render(const std::vector<DrawVertex>& vertices, const std::vector<uint16_t>& indices,
                         const std::vector<DrawCommand>& commands, int viewportWidth, int viewportHeight) {
    lastRenderSucceeded = false;
    if (!IsUsable() || !device || vertices.empty() || indices.empty() || commands.empty())
        return;
    if (viewportWidth <= 0 || viewportHeight <= 0)
        return;

    IDirect3DDevice7* dev = Device(device);

    // The state block restores what the overlay changes (see Initialize);
    // without it a DX7 title would inherit the overlay's blend and stage setup
    // on its next draw.
    if (!stateBlock)
        return;
    if (FAILED(dev->CaptureStateBlock(stateBlock))) {
        static int captureFailureLogCount = 0;
        if (captureFailureLogCount < 4) {
            HookLogImportant("[Overlay] D3D7::Render: state capture failed; reinitializing with bounded retry");
            captureFailureLogCount++;
        }
        needsReinitialize = true;
        return;
    }

    const size_t scratchBytes = vertices.size() * sizeof(D3DTLVERTEX);
    scratchVertices.resize((scratchBytes + sizeof(uint32_t) - 1u) / sizeof(uint32_t));
    auto* transformed = reinterpret_cast<D3DTLVERTEX*>(scratchVertices.data());
    for (size_t i = 0; i < vertices.size(); ++i) {
        const DrawVertex& source = vertices[i];
        D3DTLVERTEX& target = transformed[i];
        // Half-pixel placement, matching the DX8/DX9 backends: a transformed
        // vertex addresses a pixel's corner while sampling happens at its centre.
        target.sx = source.x - 0.5f;
        target.sy = source.y - 0.5f;
        target.sz = 0.0f;
        target.rhw = 1.0f;
        // The shared draw format is ABGR; D3DCOLOR is ARGB.
        const uint32_t color = source.color;
        target.color = (color & 0xFF00FF00u) | ((color & 0x00FF0000u) >> 16) | ((color & 0x000000FFu) << 16);
        target.specular = 0;
        target.tu = source.u;
        target.tv = source.v;
    }

    D3DVIEWPORT7 overlayViewport = {};
    overlayViewport.dwX = 0;
    overlayViewport.dwY = 0;
    overlayViewport.dwWidth = static_cast<DWORD>(viewportWidth);
    overlayViewport.dwHeight = static_cast<DWORD>(viewportHeight);
    overlayViewport.dvMinZ = 0.0f;
    overlayViewport.dvMaxZ = 1.0f;
    SetOverlayDeviceState(dev, &overlayViewport);

    constexpr size_t kSamplerStateCount = sizeof(kStageZeroSamplerStates) / sizeof(kStageZeroSamplerStates[0]);
    DWORD savedSamplerStates[kSamplerStateCount] = {};
    bool samplerStateSaved[kSamplerStateCount] = {};
    for (size_t i = 0; i < kSamplerStateCount; ++i) {
        samplerStateSaved[i] = SUCCEEDED(dev->GetTextureStageState(0, kStageZeroSamplerStates[i],
                                                                   &savedSamplerStates[i]));
        dev->SetTextureStageState(0, kStageZeroSamplerStates[i], kStageZeroSamplerValues[i]);
    }

    lastUseTexture = !commands.front().useTexture;

    static int drawLogCount = 0;
    bool drew = false;
    bool allDrawsSucceeded = true;
    for (const auto& command : commands) {
        if (command.indexCount == 0 ||
            static_cast<size_t>(command.indexOffset) + command.indexCount > indices.size()) {
            continue;
        }
        if (command.useTexture != lastUseTexture) {
            ApplyStageMode(command.useTexture);
        }
        const HRESULT drawHr =
            dev->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, D3DFVF_TLVERTEX, transformed,
                                      static_cast<DWORD>(vertices.size()),
                                      const_cast<WORD*>(reinterpret_cast<const WORD*>(indices.data())) +
                                          command.indexOffset,
                                       command.indexCount, 0);
        drew = true;
        allDrawsSucceeded = allDrawsSucceeded && SUCCEEDED(drawHr);
        if (drawLogCount < 4) {
            HookLogImportant("[Overlay] D3D7::Draw: hr=0x%08X verts=%u idxOff=%u idxCnt=%u tex=%d",
                             static_cast<unsigned>(drawHr), static_cast<unsigned>(vertices.size()),
                             command.indexOffset, command.indexCount, command.useTexture ? 1 : 0);
            drawLogCount++;
        }
    }

    dev->SetTexture(0, nullptr);
    for (size_t i = 0; i < kSamplerStateCount; ++i) {
        if (samplerStateSaved[i])
            dev->SetTextureStageState(0, kStageZeroSamplerStates[i], savedSamplerStates[i]);
    }

    // A failed restore leaves the application's device carrying the overlay's
    // blend and stage setup. There is no way to put that back, so the backend
    // stops drawing rather than corrupting every later frame.
    const HRESULT applyHr = dev->ApplyStateBlock(stateBlock);
    if (FAILED(applyHr)) {
        HookLogImportant("[Overlay] D3D7::Render: state restore FAILED (hr=0x%08X); disabling the native overlay to "
                         "leave the application's device alone",
                         static_cast<unsigned>(applyHr));
        dev->DeleteStateBlock(stateBlock);
        stateBlock = 0;
        return;
    }
    lastRenderSucceeded = drew && allDrawsSucceeded;
    if (drew && !allDrawsSucceeded) {
        needsReinitialize = true;
        static uint32_t drawFailureCount = 0;
        ++drawFailureCount;
        if (drawFailureCount <= 4 || (drawFailureCount & (drawFailureCount - 1)) == 0) {
            HookLogImportant("[Overlay] D3D7::Render: draw submission failed; reinitializing the native sidecar "
                             "(occurrence=%u)",
                             drawFailureCount);
        }
    }
}

}  // namespace CustomOverlay
