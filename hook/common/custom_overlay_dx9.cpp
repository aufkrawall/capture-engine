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
#include "hook_common.h"

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

    // Resize buffers if needed
    size_t vbSize = vertices.size() * sizeof(DX9Vertex);
    if (vbSize > vertexBufferSize) {
        if (!ResizeVertexBuffer(vbSize))
            return;
    }

    size_t ibSize = indices.size() * sizeof(uint16_t);
    if (ibSize > indexBufferSize) {
        if (!ResizeIndexBuffer(ibSize))
            return;
    }

    // Upload vertices
    void* vbPtr;
    HRESULT vbLockHr = vertexBuffer->Lock(0, 0, &vbPtr, D3DLOCK_DISCARD);
    static int lockLogCount = 0;
    if (lockLogCount < 2) {
        HookLogImportant("[Overlay] DX9::Render: VBLock hr=0x%08X IBLock (pending), vbSize=%zu ibSize=%zu",
                         (unsigned)vbLockHr, vbSize, ibSize);
        lockLogCount++;
    }
    if (SUCCEEDED(vbLockHr)) {
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
        vertexBuffer->Unlock();
    }

    // Upload indices
    void* ibPtr;
    if (SUCCEEDED(indexBuffer->Lock(0, 0, &ibPtr, D3DLOCK_DISCARD))) {
        memcpy(ibPtr, indices.data(), ibSize);
        indexBuffer->Unlock();
    }

    // Redirect to backbuffer so the overlay always lands on the presented surface,
    // regardless of what render target the game left active when it called Present.
    IDirect3DSurface9* oldRT = nullptr;
    IDirect3DStateBlock9* stateBlock = nullptr;
    D3DMATRIX oldWorld = {};
    D3DMATRIX oldView = {};
    D3DMATRIX oldProjection = {};
    bool hasOldWorld = SUCCEEDED(device->GetTransform(D3DTS_WORLD, &oldWorld));
    bool hasOldView = SUCCEEDED(device->GetTransform(D3DTS_VIEW, &oldView));
    bool hasOldProjection = SUCCEEDED(device->GetTransform(D3DTS_PROJECTION, &oldProjection));
    D3DVIEWPORT9 oldViewport = {};
    bool hasOldViewport = SUCCEEDED(device->GetViewport(&oldViewport));
    IDirect3DSurface9* backBuffer = nullptr;
    device->GetRenderTarget(0, &oldRT);
    HRESULT stateBlockHr = device->CreateStateBlock(D3DSBT_ALL, &stateBlock);
    if (SUCCEEDED(stateBlockHr) && stateBlock) {
        stateBlockHr = stateBlock->Capture();
    }
    if (FAILED(stateBlockHr)) {
        HookLogImportant("[Overlay] DX9::Render: Create/CaptureStateBlock FAILED hr=0x%08X", (unsigned)stateBlockHr);
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
        device->SetRenderTarget(0, backBuffer);
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

    // Set viewport
    D3DVIEWPORT9 vp = {0, 0, (DWORD)viewportWidth, (DWORD)viewportHeight, 0.0f, 1.0f};
    device->SetViewport(&vp);

    // BeginScene - track if we started it to avoid double-EndScene
    bool weStartedScene = SUCCEEDED(device->BeginScene());
    static int bsLogCount = 0;
    if (bsLogCount < 3) {
        HookLogImportant("[Overlay] DX9::Render: BeginScene weStarted=%d vp=%dx%d", weStartedScene ? 1 : 0,
                         viewportWidth, viewportHeight);
        bsLogCount++;
    }

    // Set render state
    device->SetVertexShader(nullptr);
    device->SetPixelShader(nullptr);
    device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    device->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
    device->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
    device->SetRenderState(D3DRS_ZENABLE, FALSE);
    device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    device->SetRenderState(D3DRS_LIGHTING, FALSE);
    device->SetRenderState(D3DRS_COLORWRITEENABLE,
                           D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE);
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

    if (stateBlock) {
        HRESULT applyHr = stateBlock->Apply();
        static int applyLogCount = 0;
        if (applyLogCount < 2) {
            HookLogImportant("[Overlay] DX9::Render: ApplyStateBlock hr=0x%08X", (unsigned)applyHr);
            applyLogCount++;
        }
        stateBlock->Release();
        stateBlock = nullptr;
    }

    if (hasOldWorld) {
        device->SetTransform(D3DTS_WORLD, &oldWorld);
    }
    if (hasOldView) {
        device->SetTransform(D3DTS_VIEW, &oldView);
    }
    if (hasOldProjection) {
        device->SetTransform(D3DTS_PROJECTION, &oldProjection);
    }
    if (hasOldViewport) {
        device->SetViewport(&oldViewport);
    }

    // Restore render target
    if (oldRT) {
        device->SetRenderTarget(0, oldRT);
        oldRT->Release();
    }
}

}  // namespace CustomOverlay
