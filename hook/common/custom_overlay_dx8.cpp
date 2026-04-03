/**
 * Custom Overlay - DX8 Backend Implementation
 */

#include <d3d8.h>

#include "custom_overlay_dx8.h"

#include <cstring>

#include "hook_common.h"

namespace CustomOverlay {

namespace {

void SetStage0Mode(IDirect3DDevice8* device, bool useTexture) {
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

struct DX8Vertex {
    float x, y, z, rhw;
    DWORD color;
    float u, v;
};

#define D3DFVF_DX8_CUSTOMVERTEX (D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1)

DX8Backend::DX8Backend(IDirect3DDevice8* dev) : device(dev) {}

DX8Backend::~DX8Backend() {
    Shutdown();
}

bool DX8Backend::Initialize(int fontTextureWidth, int fontTextureHeight, const uint8_t* fontTextureData) {
    if (initialized || !device) {
        return false;
    }

    HRESULT hr = device->CreateTexture(fontTextureWidth, fontTextureHeight, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED,
                                       &fontTexture);
    if (FAILED(hr) || !fontTexture) {
        return false;
    }

    D3DLOCKED_RECT lockedRect = {};
    hr = fontTexture->LockRect(0, &lockedRect, nullptr, 0);
    if (SUCCEEDED(hr)) {
        for (int y = 0; y < fontTextureHeight; ++y) {
            uint8_t* dst = static_cast<uint8_t*>(lockedRect.pBits) + y * lockedRect.Pitch;
            const uint8_t* src = fontTextureData + y * fontTextureWidth * 4;
            for (int x = 0; x < fontTextureWidth; ++x) {
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

    vertexBufferSize = DX8_VERTEX_BUFFER_SIZE;
    hr = device->CreateVertexBuffer(static_cast<UINT>(vertexBufferSize), D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY,
                                    D3DFVF_DX8_CUSTOMVERTEX, D3DPOOL_DEFAULT, &vertexBuffer);
    if (FAILED(hr) || !vertexBuffer) {
        return false;
    }

    indexBufferSize = DX8_INDEX_BUFFER_SIZE;
    hr = device->CreateIndexBuffer(static_cast<UINT>(indexBufferSize), D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY,
                                   D3DFMT_INDEX16, D3DPOOL_DEFAULT, &indexBuffer);
    if (FAILED(hr) || !indexBuffer) {
        return false;
    }

    initialized = true;
    return true;
}

void DX8Backend::Shutdown() {
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
    initialized = false;
}

bool DX8Backend::ResizeVertexBuffer(size_t requiredBytes) {
    if (!device) {
        return false;
    }

    size_t newSize = vertexBufferSize ? vertexBufferSize * 2 : DX8_VERTEX_BUFFER_SIZE;
    while (newSize < requiredBytes) {
        newSize *= 2;
    }

    IDirect3DVertexBuffer8* newBuffer = nullptr;
    HRESULT hr = device->CreateVertexBuffer(static_cast<UINT>(newSize), D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY,
                                            D3DFVF_DX8_CUSTOMVERTEX, D3DPOOL_DEFAULT, &newBuffer);
    if (FAILED(hr) || !newBuffer) {
        return false;
    }

    if (vertexBuffer) {
        vertexBuffer->Release();
    }

    vertexBuffer = newBuffer;
    vertexBufferSize = newSize;
    return true;
}

bool DX8Backend::ResizeIndexBuffer(size_t requiredBytes) {
    if (!device) {
        return false;
    }

    size_t newSize = indexBufferSize ? indexBufferSize * 2 : DX8_INDEX_BUFFER_SIZE;
    while (newSize < requiredBytes) {
        newSize *= 2;
    }

    IDirect3DIndexBuffer8* newBuffer = nullptr;
    HRESULT hr = device->CreateIndexBuffer(static_cast<UINT>(newSize), D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY,
                                           D3DFMT_INDEX16, D3DPOOL_DEFAULT, &newBuffer);
    if (FAILED(hr) || !newBuffer) {
        return false;
    }

    if (indexBuffer) {
        indexBuffer->Release();
    }

    indexBuffer = newBuffer;
    indexBufferSize = newSize;
    return true;
}

void DX8Backend::Render(const std::vector<DrawVertex>& vertices, const std::vector<uint16_t>& indices,
                        const std::vector<DrawCommand>& commands, int viewportWidth, int viewportHeight) {
    if (!initialized || !device || vertices.empty()) {
        return;
    }

    const size_t vbSize = vertices.size() * sizeof(DX8Vertex);
    if (vbSize > vertexBufferSize && !ResizeVertexBuffer(vbSize)) {
        return;
    }

    const size_t ibSize = indices.size() * sizeof(uint16_t);
    if (ibSize > indexBufferSize && !ResizeIndexBuffer(ibSize)) {
        return;
    }

    void* vbPtr = nullptr;
    HRESULT hr = vertexBuffer->Lock(0, 0, reinterpret_cast<BYTE**>(&vbPtr), D3DLOCK_DISCARD);
    if (FAILED(hr) || !vbPtr) {
        return;
    }

    DX8Vertex* dst = static_cast<DX8Vertex*>(vbPtr);
    for (const auto& v : vertices) {
        dst->x = v.x - 0.5f;
        dst->y = v.y - 0.5f;
        dst->z = 0.0f;
        dst->rhw = 1.0f;
        const uint8_t r = (v.color >> 0) & 0xFF;
        const uint8_t g = (v.color >> 8) & 0xFF;
        const uint8_t b = (v.color >> 16) & 0xFF;
        const uint8_t a = (v.color >> 24) & 0xFF;
        dst->color = (static_cast<DWORD>(a) << 24) | (static_cast<DWORD>(r) << 16) | (static_cast<DWORD>(g) << 8) | b;
        dst->u = v.u;
        dst->v = v.v;
        ++dst;
    }
    vertexBuffer->Unlock();

    void* ibPtr = nullptr;
    hr = indexBuffer->Lock(0, 0, reinterpret_cast<BYTE**>(&ibPtr), D3DLOCK_DISCARD);
    if (FAILED(hr) || !ibPtr) {
        return;
    }
    memcpy(ibPtr, indices.data(), ibSize);
    indexBuffer->Unlock();

    IDirect3DSurface8* oldRT = nullptr;
    IDirect3DSurface8* oldDS = nullptr;
    IDirect3DSurface8* backBuffer = nullptr;
    DWORD stateBlock = 0;

    D3DMATRIX oldWorld = {};
    D3DMATRIX oldView = {};
    D3DMATRIX oldProjection = {};
    const bool hasOldWorld = SUCCEEDED(device->GetTransform(D3DTS_WORLD, &oldWorld));
    const bool hasOldView = SUCCEEDED(device->GetTransform(D3DTS_VIEW, &oldView));
    const bool hasOldProjection = SUCCEEDED(device->GetTransform(D3DTS_PROJECTION, &oldProjection));
    D3DVIEWPORT8 oldViewport = {};
    const bool hasOldViewport = SUCCEEDED(device->GetViewport(&oldViewport));

    DWORD oldVertexShader = 0;
    DWORD oldPixelShader = 0;
    const bool hasOldVertexShader = SUCCEEDED(device->GetVertexShader(&oldVertexShader));
    const bool hasOldPixelShader = SUCCEEDED(device->GetPixelShader(&oldPixelShader));

    device->GetRenderTarget(&oldRT);
    device->GetDepthStencilSurface(&oldDS);

    hr = device->CreateStateBlock(D3DSBT_ALL, &stateBlock);
    if (FAILED(hr) || stateBlock == 0) {
        if (oldRT) {
            oldRT->Release();
        }
        if (oldDS) {
            oldDS->Release();
        }
        return;
    }

    if (SUCCEEDED(device->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &backBuffer)) && backBuffer) {
        device->SetRenderTarget(backBuffer, oldDS);
        D3DSURFACE_DESC desc = {};
        if (SUCCEEDED(backBuffer->GetDesc(&desc))) {
            viewportWidth = static_cast<int>(desc.Width);
            viewportHeight = static_cast<int>(desc.Height);
        }
        backBuffer->Release();
        backBuffer = nullptr;
    }

    D3DVIEWPORT8 viewport = {0, 0, static_cast<DWORD>(viewportWidth), static_cast<DWORD>(viewportHeight), 0.0f, 1.0f};
    device->SetViewport(&viewport);

    const bool weStartedScene = SUCCEEDED(device->BeginScene());

    device->SetPixelShader(0);
    device->SetVertexShader(D3DFVF_DX8_CUSTOMVERTEX);
    device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    device->SetRenderState(D3DRS_ZENABLE, FALSE);
    device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    device->SetRenderState(D3DRS_LIGHTING, FALSE);
    device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    device->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    device->SetRenderState(D3DRS_FOGENABLE, FALSE);

    device->SetTextureStageState(0, D3DTSS_ADDRESSU, D3DTADDRESS_CLAMP);
    device->SetTextureStageState(0, D3DTSS_ADDRESSV, D3DTADDRESS_CLAMP);
    device->SetTextureStageState(0, D3DTSS_MAGFILTER, D3DTEXF_LINEAR);
    device->SetTextureStageState(0, D3DTSS_MINFILTER, D3DTEXF_LINEAR);
    device->SetTextureStageState(0, D3DTSS_MIPFILTER, D3DTEXF_NONE);
    SetStage0Mode(device, true);

    device->SetTexture(0, fontTexture);
    device->SetTexture(1, nullptr);
    device->SetStreamSource(0, vertexBuffer, sizeof(DX8Vertex));
    device->SetIndices(indexBuffer, 0);

    bool lastUseTexture = true;
    lastTexture = fontTexture;
    for (const auto& cmd : commands) {
        if (cmd.useTexture != lastUseTexture) {
            SetStage0Mode(device, cmd.useTexture);
            lastUseTexture = cmd.useTexture;
        }

        IDirect3DBaseTexture8* targetTexture = cmd.useTexture ? fontTexture : nullptr;
        if (targetTexture != lastTexture) {
            device->SetTexture(0, targetTexture);
            lastTexture = targetTexture;
        }

        device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, static_cast<UINT>(vertices.size()), cmd.indexOffset,
                                     cmd.indexCount / 3);
    }

    if (weStartedScene) {
        device->EndScene();
    }

    device->ApplyStateBlock(stateBlock);
    device->DeleteStateBlock(stateBlock);

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
    if (hasOldVertexShader) {
        device->SetVertexShader(oldVertexShader);
    }
    if (hasOldPixelShader) {
        device->SetPixelShader(oldPixelShader);
    }

    if (oldRT) {
        device->SetRenderTarget(oldRT, oldDS);
        oldRT->Release();
    }
    if (oldDS) {
        oldDS->Release();
    }
}

}  // namespace CustomOverlay
