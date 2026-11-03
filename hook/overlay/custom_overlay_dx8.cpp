/**
 * Custom Overlay - DX8 Backend Implementation
 */

#include <d3d8.h>

#include "custom_overlay_dx8.h"

#include <cstring>

#include "hook/runtime/hook_common.h"
#include "legacy_overlay_state_saver.h"

namespace CustomOverlay {

namespace {

// D3D8 keeps sampler state in the texture stage states, so kSampler is unused.
struct DX8StateAccess {
    using Device = IDirect3DDevice8;
    static bool Read(Device* device, LegacyOverlayStateKind kind, uint32_t stage, uint32_t state, uint32_t* value) {
        DWORD current = 0;
        HRESULT hr = D3DERR_INVALIDCALL;
        if (kind == LegacyOverlayStateKind::kRender)
            hr = device->GetRenderState(static_cast<D3DRENDERSTATETYPE>(state), &current);
        else if (kind == LegacyOverlayStateKind::kStage)
            hr = device->GetTextureStageState(stage, static_cast<D3DTEXTURESTAGESTATETYPE>(state), &current);
        *value = current;
        return SUCCEEDED(hr);
    }
    static bool Write(Device* device, LegacyOverlayStateKind kind, uint32_t stage, uint32_t state, uint32_t value) {
        if (kind == LegacyOverlayStateKind::kRender)
            return SUCCEEDED(device->SetRenderState(static_cast<D3DRENDERSTATETYPE>(state), value));
        if (kind == LegacyOverlayStateKind::kStage)
            return SUCCEEDED(device->SetTextureStageState(stage, static_cast<D3DTEXTURESTAGESTATETYPE>(state), value));
        return false;
    }
};

using DX8States = LegacyOverlayStates<DX8StateAccess>;

void SetStage0Mode(DX8States& states, bool useTexture) {
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

struct DX8Vertex {
    float x, y, z, rhw;
    DWORD color;
    float u, v;
};

#define D3DFVF_DX8_CUSTOMVERTEX (D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1)

namespace {

// Every state the overlay's draw sets, in one place. States go through
// `states`, which saves the application's values; the bindings set here are
// the ones DX8SavedBindings saves.
void SetOverlayState(IDirect3DDevice8* device, DX8States& states, const D3DVIEWPORT8& viewport,
                     IDirect3DTexture8* fontTexture, IDirect3DVertexBuffer8* vertexBuffer,
                     IDirect3DIndexBuffer8* indexBuffer, UINT baseVertex) {
    device->SetViewport(&viewport);
    device->SetPixelShader(0);
    device->SetVertexShader(D3DFVF_DX8_CUSTOMVERTEX);
    states.Render(D3DRS_ALPHABLENDENABLE, TRUE);
    states.Render(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    states.Render(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    states.Render(D3DRS_ZENABLE, FALSE);
    states.Render(D3DRS_ZWRITEENABLE, FALSE);
    states.Render(D3DRS_CULLMODE, D3DCULL_NONE);
    states.Render(D3DRS_LIGHTING, FALSE);
    states.Render(D3DRS_ALPHATESTENABLE, FALSE);
    states.Render(D3DRS_STENCILENABLE, FALSE);
    states.Render(D3DRS_FOGENABLE, FALSE);

    states.Stage(0, D3DTSS_ADDRESSU, D3DTADDRESS_CLAMP);
    states.Stage(0, D3DTSS_ADDRESSV, D3DTADDRESS_CLAMP);
    states.Stage(0, D3DTSS_MAGFILTER, D3DTEXF_LINEAR);
    states.Stage(0, D3DTSS_MINFILTER, D3DTEXF_LINEAR);
    states.Stage(0, D3DTSS_MIPFILTER, D3DTEXF_NONE);
    SetStage0Mode(states, true);

    device->SetTexture(0, fontTexture);
    device->SetTexture(1, nullptr);
    device->SetStreamSource(0, vertexBuffer, sizeof(DX8Vertex));
    // Direct3D 8 takes the base vertex here, not in the draw.
    device->SetIndices(indexBuffer, baseVertex);
}

// The application's bindings SetOverlayState replaces, each holding the
// reference its Get* call took. The viewport is here too: switching render
// targets resets it, so it is restored after the target.
class DX8SavedBindings {
public:
    DX8SavedBindings() = default;
    DX8SavedBindings(const DX8SavedBindings&) = delete;
    DX8SavedBindings& operator=(const DX8SavedBindings&) = delete;
    ~DX8SavedBindings() {
        Release();
    }

    bool Save(IDirect3DDevice8* device) {
        return SUCCEEDED(device->GetTexture(0, &textures[0])) && SUCCEEDED(device->GetTexture(1, &textures[1])) &&
               SUCCEEDED(device->GetVertexShader(&vertexShader)) && SUCCEEDED(device->GetPixelShader(&pixelShader)) &&
               SUCCEEDED(device->GetStreamSource(0, &stream, &streamStride)) &&
               SUCCEEDED(device->GetIndices(&indices, &baseVertexIndex)) && SUCCEEDED(device->GetViewport(&viewport));
    }

    void Restore(IDirect3DDevice8* device) {
        device->SetViewport(&viewport);
        device->SetVertexShader(vertexShader);
        device->SetPixelShader(pixelShader);
        device->SetStreamSource(0, stream, streamStride);
        device->SetIndices(indices, baseVertexIndex);
        device->SetTexture(0, textures[0]);
        device->SetTexture(1, textures[1]);
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
        ReleaseOne(textures[0]);
        ReleaseOne(textures[1]);
        ReleaseOne(stream);
        ReleaseOne(indices);
    }

    IDirect3DBaseTexture8* textures[2] = {};
    DWORD vertexShader = 0;
    DWORD pixelShader = 0;
    IDirect3DVertexBuffer8* stream = nullptr;
    UINT streamStride = 0;
    IDirect3DIndexBuffer8* indices = nullptr;
    UINT baseVertexIndex = 0;
    D3DVIEWPORT8 viewport = {};
};

}  // namespace

DX8Backend::DX8Backend(IDirect3DDevice8* dev) : device(dev) {}

DX8Backend::~DX8Backend() {
    Shutdown();
}

bool DX8Backend::Initialize(int fontTextureWidth, int fontTextureHeight, const uint8_t* fontTextureData) {
    if (initialized || !device) {
        return false;
    }

    D3DDEVICE_CREATION_PARAMETERS creation = {};
    pureDevice = SUCCEEDED(device->GetCreationParameters(&creation)) &&
                 (creation.BehaviorFlags & D3DCREATE_PUREDEVICE) != 0;
    HookLogImportant("[Overlay] DX8 backend saves %s around its draw (pure=%d)",
                     pureDevice ? "the whole device with a D3DSBT_ALL block" : "only the states it changes",
                     pureDevice ? 1 : 0);

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
    if (stateBlock && device) {
        device->DeleteStateBlock(stateBlock);
        stateBlock = 0;
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
    geometryUpload.MarkBufferRecreated();
    vertexRing.Invalidate();
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
    geometryUpload.MarkBufferRecreated();
    indexUpload.Invalidate();
    return true;
}

void DX8Backend::Render(const std::vector<DrawVertex>& vertices, const std::vector<uint16_t>& indices,
                        const std::vector<DrawCommand>& commands, int viewportWidth, int viewportHeight) {
    if (!initialized || !device || vertices.empty()) {
        return;
    }

    const size_t vbSize = vertices.size() * sizeof(DX8Vertex);
    const size_t ibSize = indices.size() * sizeof(uint16_t);
    HRESULT hr = D3D_OK;
    if (geometryUpload.NeedsUpload()) {
        if (vbSize > vertexBufferSize && !ResizeVertexBuffer(vbSize)) {
            geometryUpload.MarkUploadFailed();
            return;
        }
        if (ibSize > indexBufferSize && !ResizeIndexBuffer(ibSize)) {
            geometryUpload.MarkUploadFailed();
            return;
        }

        // Appended behind the last frame; discarded only when full
        // (legacy_overlay_cache.h).
        const LegacyVertexRing::Slot slot = vertexRing.Next(
            vbSize, vertexBufferSize, static_cast<size_t>(geometryUpload.UnchangedVertices()) * sizeof(DX8Vertex));
        void* vbPtr = nullptr;
        hr = vertexBuffer->Lock(static_cast<UINT>(slot.offset), static_cast<UINT>(vbSize - slot.keptBytes),
                                reinterpret_cast<BYTE**>(&vbPtr), slot.discard ? D3DLOCK_DISCARD : D3DLOCK_NOOVERWRITE);
        if (FAILED(hr) || !vbPtr) {
            geometryUpload.MarkUploadFailed();
            vertexRing.Invalidate();
            return;
        }

        DX8Vertex* dst = static_cast<DX8Vertex*>(vbPtr);
        for (size_t i = slot.keptBytes / sizeof(DX8Vertex); i < vertices.size(); ++i) {
            const DrawVertex& v = vertices[i];
            dst->x = v.x - 0.5f;
            dst->y = v.y - 0.5f;
            dst->z = 0.0f;
            dst->rhw = 1.0f;
            const uint8_t r = (v.color >> 0) & 0xFF;
            const uint8_t g = (v.color >> 8) & 0xFF;
            const uint8_t b = (v.color >> 16) & 0xFF;
            const uint8_t a = (v.color >> 24) & 0xFF;
            dst->color =
                (static_cast<DWORD>(a) << 24) | (static_cast<DWORD>(r) << 16) | (static_cast<DWORD>(g) << 8) | b;
            dst->u = v.u;
            dst->v = v.v;
            ++dst;
        }
        if (FAILED(vertexBuffer->Unlock())) {
            geometryUpload.MarkUploadFailed();
            vertexRing.Invalidate();
            return;
        }
        frameLayout = LegacyFrameLayout::For(slot, sizeof(DX8Vertex), static_cast<uint32_t>(vertices.size()),
                                             static_cast<uint32_t>(indices.size()), geometryUpload.UnchangedIndices());

        if (indexUpload.NeedsUpload(indices)) {
            void* ibPtr = nullptr;
            hr = indexBuffer->Lock(0, 0, reinterpret_cast<BYTE**>(&ibPtr), D3DLOCK_DISCARD);
            if (FAILED(hr) || !ibPtr) {
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
        }
        geometryUpload.MarkUploadSucceeded();
    }

    IDirect3DSurface8* oldRT = nullptr;
    IDirect3DSurface8* oldDS = nullptr;
    IDirect3DSurface8* backBuffer = nullptr;
    device->GetRenderTarget(&oldRT);
    device->GetDepthStencilSurface(&oldDS);

    // A pure device answers no Get* call for block-held state, so there the
    // whole device is saved with a D3DSBT_ALL block; everywhere else exactly
    // the states and bindings the overlay changes (legacy_overlay_state_saver.h).
    DX8States states(device, !pureDevice);
    DX8SavedBindings bindings;
    if (pureDevice) {
        if (stateBlock == 0)
            hr = device->CreateStateBlock(D3DSBT_ALL, &stateBlock);
        if (SUCCEEDED(hr) && stateBlock != 0)
            hr = device->CaptureStateBlock(stateBlock);
    } else if (!bindings.Save(device)) {
        hr = D3DERR_INVALIDCALL;
    }
    if (FAILED(hr) || (pureDevice && stateBlock == 0)) {
        static int stateCaptureFailureCount = 0;
        if (stateCaptureFailureCount < 4) {
            HookLogImportant("[Overlay] DX8::Render: saving the application's state failed hr=0x%08X pure=%d",
                             (unsigned)hr, pureDevice ? 1 : 0);
            stateCaptureFailureCount++;
        }
        if (stateBlock) {
            device->DeleteStateBlock(stateBlock);
            stateBlock = 0;
        }
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
        // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization) - zero-initialized placeholder; enum fields are assigned before use
        D3DSURFACE_DESC desc = {};
        if (SUCCEEDED(backBuffer->GetDesc(&desc))) {
            viewportWidth = static_cast<int>(desc.Width);
            viewportHeight = static_cast<int>(desc.Height);
        }
        backBuffer->Release();
        backBuffer = nullptr;
    }

    const D3DVIEWPORT8 viewport = {0, 0, static_cast<DWORD>(viewportWidth), static_cast<DWORD>(viewportHeight),
                                   0.0f, 1.0f};
    const bool weStartedScene = SUCCEEDED(device->BeginScene());
    SetOverlayState(device, states, viewport, fontTexture, vertexBuffer, indexBuffer, frameLayout.headBaseVertex);

    bool lastUseTexture = true;
    lastTexture = fontTexture;
    uint32_t boundBaseVertex = frameLayout.headBaseVertex;
    for (const auto& cmd : commands) {
        if (cmd.useTexture != lastUseTexture) {
            SetStage0Mode(states, cmd.useTexture);
            lastUseTexture = cmd.useTexture;
        }

        IDirect3DBaseTexture8* targetTexture = cmd.useTexture ? fontTexture : nullptr;
        if (targetTexture != lastTexture) {
            device->SetTexture(0, targetTexture);
            lastTexture = targetTexture;
        }

        frameLayout.ForEachRun(
            cmd.indexOffset, cmd.indexCount, static_cast<uint32_t>(vertices.size()),
            [&](uint32_t base, uint32_t firstVertex, uint32_t vertexCount, uint32_t firstIndex, uint32_t indexCount) {
                if (base != boundBaseVertex) {
                    device->SetIndices(indexBuffer, base);
                    boundBaseVertex = base;
                }
                device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, firstVertex, vertexCount, firstIndex, indexCount / 3);
            });
    }

    if (weStartedScene) {
        device->EndScene();
    }

    // Render target first: switching it resets the viewport, which is
    // restored after it.
    if (oldRT) {
        device->SetRenderTarget(oldRT, oldDS);
        oldRT->Release();
    }
    if (oldDS) {
        oldDS->Release();
    }
    if (!pureDevice) {
        bindings.Restore(device);
        const size_t savedStates = states.Saved();
        states.Restore();
        static int restoreLogCount = 0;
        static int unreadLogCount = 0;
        const bool unread = states.Unread() != 0;
        if (restoreLogCount < 2 || (unread && unreadLogCount < 4)) {
            HookLogImportant("[Overlay] DX8::Render: restored the application's state (states=%zu unread=%u)",
                             savedStates, states.Unread());
            restoreLogCount++;
            unreadLogCount += unread ? 1 : 0;
        }
        return;
    }
    const HRESULT applyHr = device->ApplyStateBlock(stateBlock);
    if (FAILED(applyHr)) {
        static int stateApplyFailureCount = 0;
        if (stateApplyFailureCount < 4) {
            HookLogImportant("[Overlay] DX8::Render: state-block apply failed hr=0x%08X", (unsigned)applyHr);
            stateApplyFailureCount++;
        }
        device->DeleteStateBlock(stateBlock);
        stateBlock = 0;
    }
}

}  // namespace CustomOverlay
