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
#include "hook/runtime/hook_common.h"

namespace CustomOverlay {

namespace {

void SetStage0Mode(IDirect3DDevice9* device, bool useTexture) {
    if (useTexture) {
        device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
        device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        device->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
        device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
        device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
        device->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
    } else {
        device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
        device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
        device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
        device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
    }

    device->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    device->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
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

// Every state the overlay's draw sets, in one place: the draw calls it, and so
// does the recording of the state block that saves and restores the
// application's values around the draw. The draw loop only switches stage 0
// between the two SetStage0Mode variants, whose states the textured variant
// covers; the recording passes preserveAlpha=true, whose states cover the other
// branch.
void SetOverlayState(IDirect3DDevice9* device, const D3DVIEWPORT9& viewport, bool preserveAlpha,
                     IDirect3DTexture9* fontTexture, IDirect3DVertexBuffer9* vertexBuffer,
                     IDirect3DIndexBuffer9* indexBuffer) {
    device->SetViewport(&viewport);
    device->SetVertexShader(nullptr);
    device->SetPixelShader(nullptr);
    device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    device->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
    device->SetRenderState(D3DRS_ZENABLE, FALSE);
    device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    device->SetRenderState(D3DRS_LIGHTING, FALSE);
    if (preserveAlpha) {
        // Over a target cleared to transparent black, SRCALPHA/INVSRCALPHA on
        // colour and ONE/INVSRCALPHA on alpha accumulate exactly the
        // premultiplied colour and the coverage, which is what a sprite blended
        // later on the CPU needs.
        device->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, TRUE);
        device->SetRenderState(D3DRS_SRCBLENDALPHA, D3DBLEND_ONE);
        device->SetRenderState(D3DRS_DESTBLENDALPHA, D3DBLEND_INVSRCALPHA);
        device->SetRenderState(D3DRS_BLENDOPALPHA, D3DBLENDOP_ADD);
        device->SetRenderState(D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN |
                                                           D3DCOLORWRITEENABLE_BLUE | D3DCOLORWRITEENABLE_ALPHA);
    } else {
        device->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
        device->SetRenderState(D3DRS_COLORWRITEENABLE,
                               D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE);
    }
    device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    device->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    device->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
    device->SetRenderState(D3DRS_FOGENABLE, FALSE);

    SetStage0Mode(device, true);

    // Bind font atlas before sampler setup so DX9 sampler override logic can
    // correctly detect this as a single-mip UI texture.
    device->SetTexture(0, fontTexture);
    device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    device->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    device->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, 0);

    device->SetStreamSource(0, vertexBuffer, 0, sizeof(DX9Vertex));
    device->SetIndices(indexBuffer);
    device->SetFVF(D3DFVF_CUSTOMVERTEX);
}

}  // namespace

DX9Backend::DX9Backend(IDirect3DDevice9* dev) : device(dev) {}

DX9Backend::~DX9Backend() {
    Shutdown();
}

bool DX9Backend::Initialize(int fontTextureWidth, int fontTextureHeight, const uint8_t* fontTextureData) {
    if (initialized || !device)
        return false;

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

    return true;
}

// A D3DSBT_ALL block captured and re-applied every render state, every sampler
// and texture stage, all shader constants, transforms and lights around each
// overlay draw - thousands of runtime calls per frame, and a driver that then
// revalidates the application's whole state on its next draw. A recorded block
// holds only the states set while recording, and Capture() refreshes exactly
// those, so the overlay saves and restores what it changes and nothing else.
// The scissor rectangle is recorded too: switching render targets resets it,
// as it resets the viewport.
HRESULT DX9Backend::RecordStateBlock() {
    HRESULT hr = device->BeginStateBlock();
    if (FAILED(hr))
        return hr;
    const D3DVIEWPORT9 viewport = {0, 0, 1, 1, 0.0f, 1.0f};
    SetOverlayState(device, viewport, true, fontTexture, vertexBuffer, indexBuffer);
    const RECT scissor = {0, 0, 1, 1};
    device->SetScissorRect(&scissor);
    hr = device->EndStateBlock(&stateBlock);
    if (FAILED(hr))
        stateBlock = nullptr;
    return hr;
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

        void* vbPtr = nullptr;
        const HRESULT vbLockHr = vertexBuffer->Lock(0, 0, &vbPtr, D3DLOCK_DISCARD);
        static int lockLogCount = 0;
        if (lockLogCount < 2) {
            HookLogImportant("[Overlay] DX9::Render: VBLock hr=0x%08X, vbSize=%zu ibSize=%zu", (unsigned)vbLockHr,
                             vbSize, ibSize);
            lockLogCount++;
        }
        if (FAILED(vbLockHr) || !vbPtr) {
            geometryUpload.MarkUploadFailed();
            return;
        }

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
            return;
        }

        void* ibPtr = nullptr;
        const HRESULT ibLockHr = indexBuffer->Lock(0, 0, &ibPtr, D3DLOCK_DISCARD);
        if (FAILED(ibLockHr) || !ibPtr) {
            geometryUpload.MarkUploadFailed();
            return;
        }
        memcpy(ibPtr, indices.data(), ibSize);
        if (FAILED(indexBuffer->Unlock())) {
            geometryUpload.MarkUploadFailed();
            return;
        }
        geometryUpload.MarkUploadSucceeded();
    }

    // Redirect to backbuffer so the overlay always lands on the presented surface,
    // regardless of what render target the game left active when it called Present.
    IDirect3DSurface9* oldRT = nullptr;
    IDirect3DSurface9* backBuffer = nullptr;
    bool switchedRT = false;
    device->GetRenderTarget(0, &oldRT);
    HRESULT stateBlockHr = D3D_OK;
    if (!stateBlock) {
        stateBlockHr = RecordStateBlock();
        static int recordLogCount = 0;
        if (recordLogCount < 2) {
            HookLogImportant("[Overlay] DX9::Render: recorded overlay state block hr=0x%08X",
                             (unsigned)stateBlockHr);
            recordLogCount++;
        }
    }
    if (SUCCEEDED(stateBlockHr) && stateBlock) {
        stateBlockHr = stateBlock->Capture();
    }
    if (FAILED(stateBlockHr)) {
        static int stateCaptureFailureCount = 0;
        if (stateCaptureFailureCount < 4) {
            HookLogImportant("[Overlay] DX9::Render: state-block capture failed hr=0x%08X", (unsigned)stateBlockHr);
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

    const D3DVIEWPORT9 vp = {0, 0, (DWORD)viewportWidth, (DWORD)viewportHeight, 0.0f, 1.0f};
    SetOverlayState(device, vp, preserveAlpha, fontTexture, vertexBuffer, indexBuffer);

    // Draw with texture caching. Track the texture we just bound above.
    bool lastUseTexture = true;
    lastTexture = fontTexture;
    static int drawHrLogCount = 0;
    for (const auto& cmd : commands) {
        if (cmd.useTexture != lastUseTexture) {
            SetStage0Mode(device, cmd.useTexture);
            lastUseTexture = cmd.useTexture;
        }

        IDirect3DBaseTexture9* targetTex = cmd.useTexture ? fontTexture : nullptr;

        if (targetTex != lastTexture) {
            device->SetTexture(0, targetTex);
            lastTexture = targetTex;
        }

        HRESULT drawHr = device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, (UINT)vertices.size(), cmd.indexOffset,
                                                      cmd.indexCount / 3);
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
    // rectangle, which the block then restores.
    if (oldRT) {
        if (switchedRT) {
            device->SetRenderTarget(0, oldRT);
        }
        oldRT->Release();
    }

    if (stateBlock) {
        HRESULT applyHr = stateBlock->Apply();
        static int applyLogCount = 0;
        if (applyLogCount < 2) {
            HookLogImportant("[Overlay] DX9::Render: ApplyStateBlock hr=0x%08X", (unsigned)applyHr);
            applyLogCount++;
        }
        if (FAILED(applyHr)) {
            stateBlock->Release();
            stateBlock = nullptr;
        }
    }
}

}  // namespace CustomOverlay
