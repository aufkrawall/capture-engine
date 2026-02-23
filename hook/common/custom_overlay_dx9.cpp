/**
 * Custom Overlay - DX9 Backend Implementation
 *
 * Optimized version with:
 * - Texture caching to avoid redundant SetTexture calls
 * - Dynamic buffer resizing
 * - Efficient vertex conversion
 */

#include "custom_overlay_dx9.h"
#include "hook_common.h"
#include <cstring>

namespace CustomOverlay {

// Custom vertex format for DX9
struct DX9Vertex {
  float x, y, z, rhw;
  DWORD color;
  float u, v;
};

#define D3DFVF_CUSTOMVERTEX (D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1)

DX9Backend::DX9Backend(IDirect3DDevice9 *dev) : device(dev) {}

DX9Backend::~DX9Backend() { Shutdown(); }

bool DX9Backend::Initialize(int fontTextureWidth, int fontTextureHeight,
                            const uint8_t *fontTextureData) {
  if (initialized || !device)
    return false;

  HRESULT hr = device->CreateTexture(fontTextureWidth, fontTextureHeight, 1,
                                     D3DUSAGE_DYNAMIC, D3DFMT_A8R8G8B8,
                                     D3DPOOL_DEFAULT, &fontTexture, nullptr);
  if (FAILED(hr))
    return false;

  D3DLOCKED_RECT lockedRect;
  hr = fontTexture->LockRect(0, &lockedRect, nullptr, D3DLOCK_DISCARD);
  if (SUCCEEDED(hr)) {
    for (int y = 0; y < fontTextureHeight; y++) {
      uint8_t *dst = (uint8_t *)lockedRect.pBits + y * lockedRect.Pitch;
      const uint8_t *src = fontTextureData + y * fontTextureWidth * 4;
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
  hr = device->CreateVertexBuffer((UINT)vertexBufferSize,
                                  D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY,
                                  D3DFVF_CUSTOMVERTEX, D3DPOOL_DEFAULT,
                                  &vertexBuffer, nullptr);
  if (FAILED(hr))
    return false;

  indexBufferSize = DX9_INDEX_BUFFER_SIZE;
  hr = device->CreateIndexBuffer((UINT)indexBufferSize,
                                 D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY,
                                 D3DFMT_INDEX16, D3DPOOL_DEFAULT, &indexBuffer,
                                 nullptr);
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

  IDirect3DVertexBuffer9 *newBuffer = nullptr;
  HRESULT hr = device->CreateVertexBuffer((UINT)newSize,
                                          D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY,
                                          D3DFVF_CUSTOMVERTEX, D3DPOOL_DEFAULT,
                                          &newBuffer, nullptr);
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

  IDirect3DIndexBuffer9 *newBuffer = nullptr;
  HRESULT hr = device->CreateIndexBuffer((UINT)newSize,
                                         D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY,
                                         D3DFMT_INDEX16, D3DPOOL_DEFAULT,
                                         &newBuffer, nullptr);
  if (FAILED(hr))
    return false;

  if (indexBuffer)
    indexBuffer->Release();

  indexBuffer = newBuffer;
  indexBufferSize = newSize;

  return true;
}

void DX9Backend::Render(const std::vector<DrawVertex> &vertices,
                        const std::vector<uint16_t> &indices,
                        const std::vector<DrawCommand> &commands,
                        int viewportWidth, int viewportHeight) {
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
  void *vbPtr;
  if (SUCCEEDED(vertexBuffer->Lock(0, 0, &vbPtr, D3DLOCK_DISCARD))) {
    DX9Vertex *dst = (DX9Vertex *)vbPtr;
    for (const auto &v : vertices) {
      dst->x = v.x;
      dst->y = v.y;
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
  void *ibPtr;
  if (SUCCEEDED(indexBuffer->Lock(0, 0, &ibPtr, D3DLOCK_DISCARD))) {
    memcpy(ibPtr, indices.data(), ibSize);
    indexBuffer->Unlock();
  }

  // Save minimal state
  DWORD oldFVF;
  IDirect3DVertexBuffer9 *oldVB = nullptr;
  UINT oldVBOffset = 0, oldVBStride = 0;
  IDirect3DIndexBuffer9 *oldIB = nullptr;
  IDirect3DBaseTexture9 *oldTex = nullptr;
  DWORD oldAlphaBlend, oldSrcBlend, oldDestBlend;
  DWORD oldZEnable, oldCullMode, oldLighting;
  DWORD oldColorOp, oldColorArg1, oldColorArg2, oldAlphaOp, oldAlphaArg1, oldAlphaArg2;
  DWORD oldMinFilter, oldMagFilter;
  D3DVIEWPORT9 oldViewport;

  device->GetFVF(&oldFVF);
  device->GetStreamSource(0, &oldVB, &oldVBOffset, &oldVBStride);
  device->GetIndices(&oldIB);
  device->GetTexture(0, &oldTex);
  device->GetRenderState(D3DRS_ALPHABLENDENABLE, &oldAlphaBlend);
  device->GetRenderState(D3DRS_SRCBLEND, &oldSrcBlend);
  device->GetRenderState(D3DRS_DESTBLEND, &oldDestBlend);
  device->GetRenderState(D3DRS_ZENABLE, &oldZEnable);
  device->GetRenderState(D3DRS_CULLMODE, &oldCullMode);
  device->GetRenderState(D3DRS_LIGHTING, &oldLighting);
  device->GetTextureStageState(0, D3DTSS_COLOROP, &oldColorOp);
  device->GetTextureStageState(0, D3DTSS_COLORARG1, &oldColorArg1);
  device->GetTextureStageState(0, D3DTSS_COLORARG2, &oldColorArg2);
  device->GetTextureStageState(0, D3DTSS_ALPHAOP, &oldAlphaOp);
  device->GetTextureStageState(0, D3DTSS_ALPHAARG1, &oldAlphaArg1);
  device->GetTextureStageState(0, D3DTSS_ALPHAARG2, &oldAlphaArg2);
  device->GetSamplerState(0, D3DSAMP_MINFILTER, &oldMinFilter);
  device->GetSamplerState(0, D3DSAMP_MAGFILTER, &oldMagFilter);
  device->GetViewport(&oldViewport);

  // Set viewport
  D3DVIEWPORT9 vp = {0, 0, (DWORD)viewportWidth, (DWORD)viewportHeight, 0.0f, 1.0f};
  device->SetViewport(&vp);

  // BeginScene - track if we started it to avoid double-EndScene
  bool weStartedScene = SUCCEEDED(device->BeginScene());

  // Set render state
  device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
  device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
  device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
  device->SetRenderState(D3DRS_ZENABLE, FALSE);
  device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
  device->SetRenderState(D3DRS_LIGHTING, FALSE);

  device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
  device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
  device->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
  device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
  device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
  device->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);

  device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
  device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);

  device->SetStreamSource(0, vertexBuffer, 0, sizeof(DX9Vertex));
  device->SetIndices(indexBuffer);
  device->SetFVF(D3DFVF_CUSTOMVERTEX);

  // Draw with texture caching
  lastTexture = nullptr;
  for (const auto &cmd : commands) {
    IDirect3DBaseTexture9 *targetTex = cmd.useTexture ? fontTexture : nullptr;

    if (targetTex != lastTexture) {
      device->SetTexture(0, targetTex);
      lastTexture = targetTex;
    }

    device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0,
                                  (UINT)vertices.size(), cmd.indexOffset,
                                  cmd.indexCount / 3);
  }

  if (weStartedScene)
    device->EndScene();

  // Restore state
  device->SetFVF(oldFVF);
  device->SetStreamSource(0, oldVB, oldVBOffset, oldVBStride);
  device->SetIndices(oldIB);
  device->SetTexture(0, oldTex);
  device->SetRenderState(D3DRS_ALPHABLENDENABLE, oldAlphaBlend);
  device->SetRenderState(D3DRS_SRCBLEND, oldSrcBlend);
  device->SetRenderState(D3DRS_DESTBLEND, oldDestBlend);
  device->SetRenderState(D3DRS_ZENABLE, oldZEnable);
  device->SetRenderState(D3DRS_CULLMODE, oldCullMode);
  device->SetRenderState(D3DRS_LIGHTING, oldLighting);
  device->SetTextureStageState(0, D3DTSS_COLOROP, oldColorOp);
  device->SetTextureStageState(0, D3DTSS_COLORARG1, oldColorArg1);
  device->SetTextureStageState(0, D3DTSS_COLORARG2, oldColorArg2);
  device->SetTextureStageState(0, D3DTSS_ALPHAOP, oldAlphaOp);
  device->SetTextureStageState(0, D3DTSS_ALPHAARG1, oldAlphaArg1);
  device->SetTextureStageState(0, D3DTSS_ALPHAARG2, oldAlphaArg2);
  device->SetSamplerState(0, D3DSAMP_MINFILTER, oldMinFilter);
  device->SetSamplerState(0, D3DSAMP_MAGFILTER, oldMagFilter);
  device->SetViewport(&oldViewport);

  // Release saved state
  if (oldVB)
    oldVB->Release();
  if (oldIB)
    oldIB->Release();
  if (oldTex)
    oldTex->Release();
}

} // namespace CustomOverlay
