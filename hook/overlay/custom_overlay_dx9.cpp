/**
 * Custom Overlay - DX9 Backend Implementation
 *
 * Optimized version with:
 * - Texture caching to avoid redundant SetTexture calls
 * - Dynamic buffer resizing
 * - Efficient vertex conversion
 */

#include "custom_overlay_dx9.h"
#include <cstring>
#include "hook/runtime/hook_clock.h"
#include "hook/runtime/hook_common.h"
#include "legacy_overlay_state_saver.h"

namespace CustomOverlay {

namespace {

struct DX9StateAccess {
    using Device = IDirect3DDevice9;
    static bool Read(Device* device, LegacyOverlayStateKind kind, uint32_t stage, uint32_t state, uint32_t* value) {
        DWORD current = 0;
        HRESULT hr = D3DERR_INVALIDCALL;
        switch (kind) {
        case LegacyOverlayStateKind::kRender:
            hr = device->GetRenderState(static_cast<D3DRENDERSTATETYPE>(state), &current);
            break;
        case LegacyOverlayStateKind::kStage:
            hr = device->GetTextureStageState(stage, static_cast<D3DTEXTURESTAGESTATETYPE>(state), &current);
            break;
        case LegacyOverlayStateKind::kSampler:
            hr = device->GetSamplerState(stage, static_cast<D3DSAMPLERSTATETYPE>(state), &current);
            break;
        }
        *value = current;
        return SUCCEEDED(hr);
    }
    static bool Write(Device* device, LegacyOverlayStateKind kind, uint32_t stage, uint32_t state, uint32_t value) {
        switch (kind) {
        case LegacyOverlayStateKind::kRender:
            return SUCCEEDED(device->SetRenderState(static_cast<D3DRENDERSTATETYPE>(state), value));
        case LegacyOverlayStateKind::kStage:
            return SUCCEEDED(device->SetTextureStageState(stage, static_cast<D3DTEXTURESTAGESTATETYPE>(state), value));
        case LegacyOverlayStateKind::kSampler:
            return SUCCEEDED(device->SetSamplerState(stage, static_cast<D3DSAMPLERSTATETYPE>(state), value));
        }
        return false;
    }
};

using DX9States = LegacyOverlayStates<DX9StateAccess>;

void SetStage0Mode(DX9States& states, bool useTexture) {
    if (useTexture) {
        states.Stage(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
        states.Stage(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        states.Stage(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
        states.Stage(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
        states.Stage(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
        states.Stage(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
    } else {
        states.Stage(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
        states.Stage(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
        states.Stage(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
        states.Stage(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
    }

    states.Stage(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    states.Stage(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
}

}  // namespace

// Custom vertex format for DX9
struct DX9Vertex {
    float x, y, z, rhw;
    DWORD color;
    float u, v;
};

#define D3DFVF_CUSTOMVERTEX (D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1)

namespace {

// Every state the overlay's draw sets, in one place. States go through
// `states`, which saves the application's values; the bindings set here are
// the ones DX9SavedBindings saves.
void SetOverlayState(IDirect3DDevice9* device, DX9States& states, const D3DVIEWPORT9& viewport, bool preserveAlpha,
                     IDirect3DTexture9* fontTexture, IDirect3DVertexBuffer9* vertexBuffer,
                     IDirect3DIndexBuffer9* indexBuffer) {
    device->SetViewport(&viewport);
    device->SetVertexShader(nullptr);
    device->SetPixelShader(nullptr);
    states.Render(D3DRS_ALPHABLENDENABLE, TRUE);
    states.Render(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    states.Render(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    states.Render(D3DRS_BLENDOP, D3DBLENDOP_ADD);
    states.Render(D3DRS_ZENABLE, FALSE);
    states.Render(D3DRS_CULLMODE, D3DCULL_NONE);
    states.Render(D3DRS_LIGHTING, FALSE);
    if (preserveAlpha) {
        // Over a target cleared to transparent black, SRCALPHA/INVSRCALPHA on
        // colour and ONE/INVSRCALPHA on alpha accumulate exactly the
        // premultiplied colour and the coverage, which is what a sprite blended
        // later on the CPU needs.
        states.Render(D3DRS_SEPARATEALPHABLENDENABLE, TRUE);
        states.Render(D3DRS_SRCBLENDALPHA, D3DBLEND_ONE);
        states.Render(D3DRS_DESTBLENDALPHA, D3DBLEND_INVSRCALPHA);
        states.Render(D3DRS_BLENDOPALPHA, D3DBLENDOP_ADD);
        states.Render(D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN |
                                                  D3DCOLORWRITEENABLE_BLUE | D3DCOLORWRITEENABLE_ALPHA);
    } else {
        states.Render(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
        states.Render(D3DRS_COLORWRITEENABLE,
                      D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE);
    }
    states.Render(D3DRS_ALPHATESTENABLE, FALSE);
    states.Render(D3DRS_STENCILENABLE, FALSE);
    states.Render(D3DRS_SCISSORTESTENABLE, FALSE);
    states.Render(D3DRS_SRGBWRITEENABLE, FALSE);
    states.Render(D3DRS_FOGENABLE, FALSE);

    SetStage0Mode(states, true);

    // Bind font atlas before sampler setup so DX9 sampler override logic can
    // correctly detect this as a single-mip UI texture.
    device->SetTexture(0, fontTexture);
    states.Sampler(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    states.Sampler(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    states.Sampler(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    states.Sampler(0, D3DSAMP_SRGBTEXTURE, 0);

    device->SetStreamSource(0, vertexBuffer, 0, sizeof(DX9Vertex));
    device->SetIndices(indexBuffer);
    device->SetFVF(D3DFVF_CUSTOMVERTEX);
}

// The application's bindings SetOverlayState replaces, each holding the
// reference its Get* call took. The viewport and scissor rectangle are here
// too: switching render targets resets both, so they are restored after the
// target.
class DX9SavedBindings {
public:
    DX9SavedBindings() = default;
    DX9SavedBindings(const DX9SavedBindings&) = delete;
    DX9SavedBindings& operator=(const DX9SavedBindings&) = delete;
    ~DX9SavedBindings() {
        Release();
    }

    bool Save(IDirect3DDevice9* device) {
        return SUCCEEDED(device->GetTexture(0, &texture)) && SUCCEEDED(device->GetVertexShader(&vertexShader)) &&
               SUCCEEDED(device->GetPixelShader(&pixelShader)) &&
               SUCCEEDED(device->GetVertexDeclaration(&declaration)) && SUCCEEDED(device->GetFVF(&fvf)) &&
               SUCCEEDED(device->GetStreamSource(0, &stream, &streamOffset, &streamStride)) &&
               SUCCEEDED(device->GetIndices(&indices)) && SUCCEEDED(device->GetViewport(&viewport)) &&
               SUCCEEDED(device->GetScissorRect(&scissor));
    }

    void Restore(IDirect3DDevice9* device) {
        device->SetViewport(&viewport);
        device->SetScissorRect(&scissor);
        device->SetVertexShader(vertexShader);
        device->SetPixelShader(pixelShader);
        // The declaration object itself, so the game's own stays bound. A format
        // the game set with SetFVF comes back through the declaration the
        // runtime made for it; the format alone only when no declaration was set.
        if (declaration)
            device->SetVertexDeclaration(declaration);
        else if (fvf)
            device->SetFVF(fvf);
        device->SetStreamSource(0, stream, streamOffset, streamStride);
        device->SetIndices(indices);
        device->SetTexture(0, texture);
        Release();
    }

private:
    template <typename T>
    static void ReleaseOne(T*& object) {
        if (object) {
            object->Release();
            object = nullptr;
        }
    }
    void Release() {
        ReleaseOne(texture);
        ReleaseOne(vertexShader);
        ReleaseOne(pixelShader);
        ReleaseOne(declaration);
        ReleaseOne(stream);
        ReleaseOne(indices);
    }

    IDirect3DBaseTexture9* texture = nullptr;
    IDirect3DVertexShader9* vertexShader = nullptr;
    IDirect3DPixelShader9* pixelShader = nullptr;
    IDirect3DVertexDeclaration9* declaration = nullptr;
    DWORD fvf = 0;
    IDirect3DVertexBuffer9* stream = nullptr;
    UINT streamOffset = 0;
    UINT streamStride = 0;
    IDirect3DIndexBuffer9* indices = nullptr;
    D3DVIEWPORT9 viewport = {};
    RECT scissor = {};
};

}  // namespace

DX9Backend::DX9Backend(IDirect3DDevice9* dev) : device(dev) {}

DX9Backend::~DX9Backend() {
    Shutdown();
}

bool DX9Backend::Initialize(int fontTextureWidth, int fontTextureHeight, const uint8_t* fontTextureData) {
    if (initialized || !device)
        return false;

    D3DDEVICE_CREATION_PARAMETERS creation = {};
    pureDevice = SUCCEEDED(device->GetCreationParameters(&creation)) &&
                 (creation.BehaviorFlags & D3DCREATE_PUREDEVICE) != 0;
    HookLogImportant("[Overlay] DX9 backend saves %s around its draw (pure=%d)",
                     pureDevice ? "the whole device with a D3DSBT_ALL block" : "only the states it changes",
                     pureDevice ? 1 : 0);

    HRESULT hr = device->CreateTexture(fontTextureWidth, fontTextureHeight, 1, D3DUSAGE_DYNAMIC, D3DFMT_A8R8G8B8,
                                       D3DPOOL_DEFAULT, &fontTexture, nullptr);
    if (FAILED(hr))
        return false;

    D3DLOCKED_RECT lockedRect;
    hr = fontTexture->LockRect(0, &lockedRect, nullptr, D3DLOCK_DISCARD);
    if (SUCCEEDED(hr)) {
        for (int y = 0; y < fontTextureHeight; y++) {
            uint8_t* dst = (uint8_t*)lockedRect.pBits + y * lockedRect.Pitch;
            const uint8_t* src = fontTextureData + y * fontTextureWidth * 4;
            for (int x = 0; x < fontTextureWidth; x++) {
                dst[0] = src[2];
                dst[1] = src[1];
                dst[2] = src[0];
                dst[3] = src[3];
                dst += 4;
                src += 4;
            }
        }
        fontTexture->UnlockRect(0);
    }

    vertexBufferSize = DX9_VERTEX_BUFFER_SIZE;
    hr = device->CreateVertexBuffer((UINT)vertexBufferSize, D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, D3DFVF_CUSTOMVERTEX,
                                    D3DPOOL_DEFAULT, &vertexBuffer, nullptr);
    if (FAILED(hr))
        return false;

    indexBufferSize = DX9_INDEX_BUFFER_SIZE;
    hr = device->CreateIndexBuffer((UINT)indexBufferSize, D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, D3DFMT_INDEX16,
                                   D3DPOOL_DEFAULT, &indexBuffer, nullptr);
    if (FAILED(hr))
        return false;

    initialized = true;
    return true;
}

void DX9Backend::Shutdown() {
    if (stateBlock) {
        stateBlock->Release();
        stateBlock = nullptr;
    }
    if (indexBuffer) {
        indexBuffer->Release();
        indexBuffer = nullptr;
    }
    if (vertexBuffer) {
        vertexBuffer->Release();
        vertexBuffer = nullptr;
    }
    if (fontTexture) {
        fontTexture->Release();
        fontTexture = nullptr;
    }
    lastTexture = nullptr;
    geometryUpload.MarkBufferRecreated();
    vertexRing.Invalidate();
    indexUpload.Invalidate();
    initialized = false;
}

bool DX9Backend::ResizeVertexBuffer(size_t requiredBytes) {
    if (!device)
        return false;

    size_t newSize = vertexBufferSize * 2;
    while (newSize < requiredBytes)
        newSize *= 2;

    IDirect3DVertexBuffer9* newBuffer = nullptr;
    HRESULT hr = device->CreateVertexBuffer((UINT)newSize, D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, D3DFVF_CUSTOMVERTEX,
                                            D3DPOOL_DEFAULT, &newBuffer, nullptr);
    if (FAILED(hr))
        return false;

    if (vertexBuffer)
        vertexBuffer->Release();

    vertexBuffer = newBuffer;
    vertexBufferSize = newSize;
    geometryUpload.MarkBufferRecreated();
    vertexRing.Invalidate();

    return true;
}

bool DX9Backend::ResizeIndexBuffer(size_t requiredBytes) {
    if (!device)
        return false;

    size_t newSize = indexBufferSize * 2;
    while (newSize < requiredBytes)
        newSize *= 2;

    IDirect3DIndexBuffer9* newBuffer = nullptr;
    HRESULT hr = device->CreateIndexBuffer((UINT)newSize, D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, D3DFMT_INDEX16,
                                           D3DPOOL_DEFAULT, &newBuffer, nullptr);
    if (FAILED(hr))
        return false;

    if (indexBuffer)
        indexBuffer->Release();

    indexBuffer = newBuffer;
    indexBufferSize = newSize;
    geometryUpload.MarkBufferRecreated();
    indexUpload.Invalidate();

    return true;
}

void DX9Backend::Render(const std::vector<DrawVertex>& vertices, const std::vector<uint16_t>& indices,
                        const std::vector<DrawCommand>& commands, int viewportWidth, int viewportHeight) {
    static int renderCount = 0;
    if (renderCount < 3) {
        HookLogImportant("[Overlay] DX9::Render#%d: verts=%zu cmds=%zu vp=%dx%d initialized=%d", renderCount,
                         vertices.size(), commands.size(), viewportWidth, viewportHeight, initialized ? 1 : 0);
        renderCount++;
    }
    if (!initialized || !device || vertices.empty())
        return;

    LARGE_INTEGER marks[5] = {};
    ce::hook_clock::QueryCounter(&marks[0]);
    const size_t vbSize = vertices.size() * sizeof(DX9Vertex);
    const size_t ibSize = indices.size() * sizeof(uint16_t);
    if (geometryUpload.NeedsUpload()) {
        if (vbSize > vertexBufferSize && !ResizeVertexBuffer(vbSize)) {
            geometryUpload.MarkUploadFailed();
            return;
        }
        if (ibSize > indexBufferSize && !ResizeIndexBuffer(ibSize)) {
            geometryUpload.MarkUploadFailed();
            return;
        }

        const LegacyVertexRing::Slot slot = vertexRing.Next(vbSize, vertexBufferSize);
        void* vbPtr = nullptr;
        const HRESULT vbLockHr = vertexBuffer->Lock(static_cast<UINT>(slot.offset), static_cast<UINT>(vbSize), &vbPtr,
                                                    slot.discard ? D3DLOCK_DISCARD : D3DLOCK_NOOVERWRITE);
        static int lockLogCount = 0;
        if (lockLogCount < 2) {
            HookLogImportant("[Overlay] DX9::Render: VBLock hr=0x%08X, vbSize=%zu ibSize=%zu offset=%zu discard=%d",
                             (unsigned)vbLockHr, vbSize, ibSize, slot.offset, slot.discard ? 1 : 0);
            lockLogCount++;
        }
        if (FAILED(vbLockHr) || !vbPtr) {
            geometryUpload.MarkUploadFailed();
            vertexRing.Invalidate();
            return;
        }
        ++submitCost.uploads;
        submitCost.discards += slot.discard ? 1u : 0u;

        DX9Vertex* dst = (DX9Vertex*)vbPtr;
        for (const auto& v : vertices) {
            // D3D9 rasterization targets pixel centers at +0.5. Shift to avoid
            // half-texel sampling that makes text look soft compared to newer APIs.
            dst->x = v.x - 0.5f;
            dst->y = v.y - 0.5f;
            dst->z = 0.0f;
            dst->rhw = 1.0f;
            uint8_t r = (v.color >> 0) & 0xFF;
            uint8_t g = (v.color >> 8) & 0xFF;
            uint8_t b = (v.color >> 16) & 0xFF;
            uint8_t a = (v.color >> 24) & 0xFF;
            dst->color = (a << 24) | (r << 16) | (g << 8) | b;
            dst->u = v.u;
            dst->v = v.v;
            dst++;
        }
        if (FAILED(vertexBuffer->Unlock())) {
            geometryUpload.MarkUploadFailed();
            vertexRing.Invalidate();
            return;
        }
        baseVertex = static_cast<UINT>(slot.offset / sizeof(DX9Vertex));

        if (indexUpload.NeedsUpload(indices)) {
            void* ibPtr = nullptr;
            const HRESULT ibLockHr = indexBuffer->Lock(0, 0, &ibPtr, D3DLOCK_DISCARD);
            if (FAILED(ibLockHr) || !ibPtr) {
                geometryUpload.MarkUploadFailed();
                indexUpload.Invalidate();
                return;
            }
            memcpy(ibPtr, indices.data(), ibSize);
            if (FAILED(indexBuffer->Unlock())) {
                geometryUpload.MarkUploadFailed();
                indexUpload.Invalidate();
                return;
            }
            indexUpload.MarkUploaded(indices);
            ++submitCost.indexUploads;
        }
        geometryUpload.MarkUploadSucceeded();
    }
    ce::hook_clock::QueryCounter(&marks[1]);

    // Redirect to backbuffer so the overlay always lands on the presented surface,
    // regardless of what render target the game left active when it called Present.
    IDirect3DSurface9* oldRT = nullptr;
    IDirect3DSurface9* backBuffer = nullptr;
    bool switchedRT = false;
    device->GetRenderTarget(0, &oldRT);
    // A pure device answers no Get* call for block-held state, so there the
    // whole device is saved with a D3DSBT_ALL block; everywhere else exactly
    // the states and bindings the overlay changes (legacy_overlay_state_saver.h).
    DX9States states(device, !pureDevice);
    DX9SavedBindings bindings;
    HRESULT stateBlockHr = D3D_OK;
    if (pureDevice) {
        if (!stateBlock)
            stateBlockHr = device->CreateStateBlock(D3DSBT_ALL, &stateBlock);
        if (SUCCEEDED(stateBlockHr) && stateBlock)
            stateBlockHr = stateBlock->Capture();
    } else if (!bindings.Save(device)) {
        stateBlockHr = D3DERR_INVALIDCALL;
    }
    if (FAILED(stateBlockHr)) {
        static int stateCaptureFailureCount = 0;
        if (stateCaptureFailureCount < 4) {
            HookLogImportant("[Overlay] DX9::Render: saving the application's state failed hr=0x%08X pure=%d",
                             (unsigned)stateBlockHr, pureDevice ? 1 : 0);
            stateCaptureFailureCount++;
        }
        if (stateBlock) {
            stateBlock->Release();
            stateBlock = nullptr;
        }
        if (oldRT) {
            oldRT->Release();
        }
        return;
    }
    if (SUCCEEDED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer))) {
        switchedRT = (oldRT != backBuffer);
        if (switchedRT) {
            device->SetRenderTarget(0, backBuffer);
        }
        // Use the actual backbuffer dimensions as the viewport, which may differ
        // from whatever viewport the game had active at Present time.
        D3DSURFACE_DESC desc;
        if (SUCCEEDED(backBuffer->GetDesc(&desc))) {
            viewportWidth = (int)desc.Width;
            viewportHeight = (int)desc.Height;
        }
        static int bbLogCount = 0;
        if (bbLogCount < 3) {
            HookLogImportant("[Overlay] DX9::Render: backbuffer=%p %dx%d (oldRT=%p)", (void*)backBuffer, viewportWidth,
                             viewportHeight, (void*)oldRT);
            bbLogCount++;
        }
        backBuffer->Release();
        backBuffer = nullptr;
    } else {
        static int bbFailCount = 0;
        if (bbFailCount < 3) {
            HookLogImportant("[Overlay] DX9::Render: GetBackBuffer FAILED (oldRT=%p)", (void*)oldRT);
            bbFailCount++;
        }
    }

    // BeginScene - track if we started it to avoid double-EndScene
    bool weStartedScene = SUCCEEDED(device->BeginScene());
    static int bsLogCount = 0;
    if (bsLogCount < 3) {
        HookLogImportant("[Overlay] DX9::Render: BeginScene weStarted=%d vp=%dx%d", weStartedScene ? 1 : 0,
                         viewportWidth, viewportHeight);
        bsLogCount++;
    }

    ce::hook_clock::QueryCounter(&marks[2]);
    const D3DVIEWPORT9 vp = {0, 0, (DWORD)viewportWidth, (DWORD)viewportHeight, 0.0f, 1.0f};
    SetOverlayState(device, states, vp, preserveAlpha, fontTexture, vertexBuffer, indexBuffer);

    // Draw with texture caching. Track the texture we just bound above.
    bool lastUseTexture = true;
    lastTexture = fontTexture;
    static int drawHrLogCount = 0;
    for (const auto& cmd : commands) {
        if (cmd.useTexture != lastUseTexture) {
            SetStage0Mode(states, cmd.useTexture);
            lastUseTexture = cmd.useTexture;
        }

        IDirect3DBaseTexture9* targetTex = cmd.useTexture ? fontTexture : nullptr;

        if (targetTex != lastTexture) {
            device->SetTexture(0, targetTex);
            lastTexture = targetTex;
        }

        HRESULT drawHr = device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, static_cast<INT>(baseVertex), 0,
                                                      (UINT)vertices.size(), cmd.indexOffset, cmd.indexCount / 3);
        if (drawHrLogCount < 4) {
            HookLogImportant("[Overlay] DX9::Draw: hr=0x%08X verts=%u idxOff=%u idxCnt=%u tex=%p", (unsigned)drawHr,
                             (unsigned)vertices.size(), cmd.indexOffset, cmd.indexCount, (void*)targetTex);
            drawHrLogCount++;
        }
    }

    if (weStartedScene) {
        HRESULT esHr = device->EndScene();
        static int esLogCount = 0;
        if (esLogCount < 2) {
            HookLogImportant("[Overlay] DX9::Render: EndScene hr=0x%08X", (unsigned)esHr);
            esLogCount++;
        }
    }

    // Render target first: switching it resets the viewport and the scissor
    // rectangle, which are restored after it.
    if (oldRT) {
        if (switchedRT) {
            device->SetRenderTarget(0, oldRT);
        }
        oldRT->Release();
    }
    ce::hook_clock::QueryCounter(&marks[3]);

    if (pureDevice) {
        HRESULT applyHr = stateBlock ? stateBlock->Apply() : D3DERR_INVALIDCALL;
        static int applyLogCount = 0;
        if (applyLogCount < 2) {
            HookLogImportant("[Overlay] DX9::Render: ApplyStateBlock hr=0x%08X", (unsigned)applyHr);
            applyLogCount++;
        }
        if (FAILED(applyHr) && stateBlock) {
            stateBlock->Release();
            stateBlock = nullptr;
        }
        ce::hook_clock::QueryCounter(&marks[4]);
        NoteSubmitCost(marks);
        return;
    }
    bindings.Restore(device);
    const size_t savedStates = states.Saved();
    states.Restore();
    ce::hook_clock::QueryCounter(&marks[4]);
    NoteSubmitCost(marks);
    static int restoreLogCount = 0;
    static int unreadLogCount = 0;
    const bool unread = states.Unread() != 0;
    if (restoreLogCount < 2 || (unread && unreadLogCount < 4)) {
        HookLogImportant("[Overlay] DX9::Render: restored the application's state (states=%zu unread=%u)",
                         savedStates, states.Unread());
        restoreLogCount++;
        unreadLogCount += unread ? 1 : 0;
    }
}

void DX9Backend::NoteSubmitCost(const LARGE_INTEGER (&marks)[5]) {
    submitCost.upload += marks[1].QuadPart - marks[0].QuadPart;
    submitCost.save += marks[2].QuadPart - marks[1].QuadPart;
    submitCost.draw += marks[3].QuadPart - marks[2].QuadPart;
    submitCost.restore += marks[4].QuadPart - marks[3].QuadPart;
    ++submitCost.frames;
    const DWORD now = ce::hook_clock::TickCount();
    if (submitCost.windowStart == 0) {
        submitCost = {};
        submitCost.windowStart = now;
        return;
    }
    if (now - submitCost.windowStart < 10000)
        return;
    LARGE_INTEGER frequency = {};
    QueryPerformanceFrequency(&frequency);
    const double usPerFrame = frequency.QuadPart > 0 && submitCost.frames > 0
                                  ? 1e6 / static_cast<double>(frequency.QuadPart) / submitCost.frames
                                  : 0.0;
    HookLogImportant(
        "[Overlay] DX9 submit cost over %lus: frames=%u upload=%.1fus save=%.1fus draw=%.1fus restore=%.1fus "
        "(vertex uploads=%u discards=%u index uploads=%u)",
        static_cast<unsigned long>((now - submitCost.windowStart) / 1000), submitCost.frames,
        static_cast<double>(submitCost.upload) * usPerFrame, static_cast<double>(submitCost.save) * usPerFrame,
        static_cast<double>(submitCost.draw) * usPerFrame, static_cast<double>(submitCost.restore) * usPerFrame,
        submitCost.uploads, submitCost.discards, submitCost.indexUploads);
    submitCost = {};
    submitCost.windowStart = now;
}

}  // namespace CustomOverlay
