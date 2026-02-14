/**
 * Custom Overlay - DX11 Backend Implementation
 */

#include "custom_overlay_dx11.h"
#include "overlay_shader_bytecode.h"
#include <cstring>

namespace CustomOverlay {

// Shader bytecode is in overlay_shader_bytecode.h (pre-compiled via
// tools/compile_shaders.py)

DX11Backend::DX11Backend(ID3D11Device *dev, ID3D11DeviceContext *ctx)
    : device(dev), context(ctx) {
  // CRITICAL FIX: Do NOT AddRef device/context
  // During app shutdown, the game destroys its D3D device before our DLL
  // unloads If we AddRef, our destructor tries to Release on already-destroyed
  // objects causing crashes. Just store raw pointers and never Release them.
  // The OS reclaims all memory when the process exits anyway.
}

DX11Backend::~DX11Backend() {
  // CRITICAL: During process exit (skipDeviceRelease=true), the D3D device is
  // already destroyed. We must Detach ALL ComPtrs to prevent their destructors
  // from calling Release() on destroyed objects.
  if (skipDeviceRelease) {
    // Detach all ComPtrs - this prevents their destructors from calling
    // Release()
    vertexShader.Detach();
    pixelShader.Detach();
    pixelShaderSolid.Detach();
    inputLayout.Detach();
    vertexBuffer.Detach();
    indexBuffer.Detach();
    constantBuffer.Detach();
    fontTexture.Detach();
    fontTextureSRV.Detach();
    sampler.Detach();
    blendState.Detach();
    rasterState.Detach();
    depthState.Detach();
    // Don't touch device/context - they're already destroyed
    device = nullptr;
    context = nullptr;
    initialized = false;
    return;
  }
  Shutdown();
}

bool DX11Backend::Initialize(int fontTextureWidth, int fontTextureHeight,
                             const uint8_t *fontTextureData) {
  if (initialized || !device || !context)
    return false;

  // Create font texture
  D3D11_TEXTURE2D_DESC texDesc = {};
  texDesc.Width = fontTextureWidth;
  texDesc.Height = fontTextureHeight;
  texDesc.MipLevels = 1;
  texDesc.ArraySize = 1;
  texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  texDesc.SampleDesc.Count = 1;
  texDesc.Usage = D3D11_USAGE_DEFAULT;
  texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

  D3D11_SUBRESOURCE_DATA initData = {};
  initData.pSysMem = fontTextureData;
  initData.SysMemPitch = fontTextureWidth * 4;

  HRESULT hr = device->CreateTexture2D(&texDesc, &initData, &fontTexture);
  if (FAILED(hr))
    return false;

  // Create SRV
  hr = device->CreateShaderResourceView(fontTexture.Get(), nullptr,
                                        &fontTextureSRV);
  if (FAILED(hr))
    return false;

  if (!CreateShaders())
    return false;
  if (!CreateBuffers())
    return false;
  if (!CreateStates())
    return false;

  initialized = true;
  return true;
}

void DX11Backend::Shutdown() {
  if (!initialized)
    return; // Guard against double-shutdown (renderer calls Shutdown, then
            // destructor)

  // CRITICAL FIX: Do NOT Release device/context - we don't AddRef them anymore
  // During app shutdown, the game destroys its D3D device before our DLL
  // unloads Releasing would crash on already-destroyed objects. Just release
  // our own created resources (shaders, buffers, etc.) The OS reclaims all
  // memory when the process exits anyway.
  vertexShader.Reset();
  pixelShader.Reset();
  pixelShaderSolid.Reset();
  inputLayout.Reset();
  vertexBuffer.Reset();
  indexBuffer.Reset();
  constantBuffer.Reset();
  fontTexture.Reset();
  fontTextureSRV.Reset();
  sampler.Reset();
  blendState.Reset();
  rasterState.Reset();
  depthState.Reset();

  // Just clear pointers - don't Release (we never AddRef'd)
  context = nullptr;
  device = nullptr;

  initialized = false;
}

bool DX11Backend::CreateShaders() {
  // Use pre-compiled shader bytecode (no runtime D3DCompile needed)
  HRESULT hr = device->CreateVertexShader(g_VS_4_0, sizeof(g_VS_4_0), nullptr,
                                          &vertexShader);
  if (FAILED(hr))
    return false;

  hr = device->CreatePixelShader(g_PS_Textured_4_0, sizeof(g_PS_Textured_4_0),
                                 nullptr, &pixelShader);
  if (FAILED(hr))
    return false;

  hr = device->CreatePixelShader(g_PS_Solid_4_0, sizeof(g_PS_Solid_4_0),
                                 nullptr, &pixelShaderSolid);
  if (FAILED(hr))
    return false;

  // Create input layout using pre-compiled VS bytecode
  D3D11_INPUT_ELEMENT_DESC layout[] = {
      {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0,
       D3D11_INPUT_PER_VERTEX_DATA, 0},
      {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8,
       D3D11_INPUT_PER_VERTEX_DATA, 0},
      {"COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 16,
       D3D11_INPUT_PER_VERTEX_DATA, 0},
  };

  hr = device->CreateInputLayout(layout, 3, g_VS_4_0, sizeof(g_VS_4_0),
                                 &inputLayout);
  if (FAILED(hr))
    return false;

  return true;
}

bool DX11Backend::CreateBuffers() {
  // Constant buffer for viewport size
  D3D11_BUFFER_DESC cbDesc = {};
  cbDesc.ByteWidth = 16; // float2 viewportSize + float2 padding
  cbDesc.Usage = D3D11_USAGE_DYNAMIC;
  cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

  HRESULT hr = device->CreateBuffer(&cbDesc, nullptr, &constantBuffer);
  if (FAILED(hr))
    return false;

  // Vertex buffer (will be resized as needed)
  vertexBufferSize = 4096;
  D3D11_BUFFER_DESC vbDesc = {};
  vbDesc.ByteWidth = (UINT)(vertexBufferSize * sizeof(DrawVertex));
  vbDesc.Usage = D3D11_USAGE_DYNAMIC;
  vbDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
  vbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

  hr = device->CreateBuffer(&vbDesc, nullptr, &vertexBuffer);
  if (FAILED(hr))
    return false;

  // Index buffer
  indexBufferSize = 8192;
  D3D11_BUFFER_DESC ibDesc = {};
  ibDesc.ByteWidth = (UINT)(indexBufferSize * sizeof(uint16_t));
  ibDesc.Usage = D3D11_USAGE_DYNAMIC;
  ibDesc.BindFlags = D3D11_BIND_INDEX_BUFFER;
  ibDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

  hr = device->CreateBuffer(&ibDesc, nullptr, &indexBuffer);
  if (FAILED(hr))
    return false;

  return true;
}

bool DX11Backend::CreateStates() {
  // Sampler
  D3D11_SAMPLER_DESC sampDesc = {};
  sampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
  sampDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
  sampDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
  sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;

  HRESULT hr = device->CreateSamplerState(&sampDesc, &sampler);
  if (FAILED(hr))
    return false;

  // Blend state (alpha blending)
  D3D11_BLEND_DESC blendDesc = {};
  blendDesc.RenderTarget[0].BlendEnable = TRUE;
  blendDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
  blendDesc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
  blendDesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
  blendDesc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
  blendDesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
  blendDesc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
  blendDesc.RenderTarget[0].RenderTargetWriteMask =
      D3D11_COLOR_WRITE_ENABLE_ALL;

  hr = device->CreateBlendState(&blendDesc, &blendState);
  if (FAILED(hr))
    return false;

  // Rasterizer state
  D3D11_RASTERIZER_DESC rasterDesc = {};
  rasterDesc.FillMode = D3D11_FILL_SOLID;
  rasterDesc.CullMode = D3D11_CULL_NONE;
  rasterDesc.ScissorEnable = FALSE;
  rasterDesc.DepthClipEnable = TRUE;

  hr = device->CreateRasterizerState(&rasterDesc, &rasterState);
  if (FAILED(hr))
    return false;

  // Depth stencil state (disable depth)
  D3D11_DEPTH_STENCIL_DESC depthDesc = {};
  depthDesc.DepthEnable = FALSE;
  depthDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
  depthDesc.DepthFunc = D3D11_COMPARISON_ALWAYS;

  hr = device->CreateDepthStencilState(&depthDesc, &depthState);
  if (FAILED(hr))
    return false;

  return true;
}

void DX11Backend::Render(const std::vector<DrawVertex> &vertices,
                         const std::vector<uint16_t> &indices,
                         const std::vector<DrawCommand> &commands,
                         int viewportWidth, int viewportHeight) {
  if (!initialized || !device || !context || vertices.empty() ||
      commands.empty())
    return;

  // Resize buffers if needed
  if (vertices.size() > vertexBufferSize) {
    vertexBufferSize = vertices.size() * 2;
    D3D11_BUFFER_DESC vbDesc = {};
    vbDesc.ByteWidth = (UINT)(vertexBufferSize * sizeof(DrawVertex));
    vbDesc.Usage = D3D11_USAGE_DYNAMIC;
    vbDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    vbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    vertexBuffer.Reset();
    device->CreateBuffer(&vbDesc, nullptr, &vertexBuffer);
  }

  if (indices.size() > indexBufferSize) {
    indexBufferSize = indices.size() * 2;
    D3D11_BUFFER_DESC ibDesc = {};
    ibDesc.ByteWidth = (UINT)(indexBufferSize * sizeof(uint16_t));
    ibDesc.Usage = D3D11_USAGE_DYNAMIC;
    ibDesc.BindFlags = D3D11_BIND_INDEX_BUFFER;
    ibDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    indexBuffer.Reset();
    device->CreateBuffer(&ibDesc, nullptr, &indexBuffer);
  }

  // Update vertex buffer
  D3D11_MAPPED_SUBRESOURCE mapped;
  HRESULT hr =
      context->Map(vertexBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
  if (SUCCEEDED(hr)) {
    memcpy(mapped.pData, vertices.data(), vertices.size() * sizeof(DrawVertex));
    context->Unmap(vertexBuffer.Get(), 0);
  }

  // Update index buffer
  hr = context->Map(indexBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
  if (SUCCEEDED(hr)) {
    memcpy(mapped.pData, indices.data(), indices.size() * sizeof(uint16_t));
    context->Unmap(indexBuffer.Get(), 0);
  }

  // Update constant buffer
  hr = context->Map(constantBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0,
                    &mapped);
  if (SUCCEEDED(hr)) {
    float *cb = (float *)mapped.pData;
    cb[0] = (float)viewportWidth;
    cb[1] = (float)viewportHeight;
    cb[2] = 0.0f;
    cb[3] = 0.0f;
    context->Unmap(constantBuffer.Get(), 0);
  }

  // Save full pipeline state using raw pointers to avoid ComPtr
  // AddRef/Release overhead. Each Get* call increments refcount once;
  // we release manually after restore.
  ID3D11RasterizerState *oldRasterState = nullptr;
  ID3D11BlendState *oldBlendState = nullptr;
  ID3D11DepthStencilState *oldDepthState = nullptr;
  FLOAT oldBlendFactor[4];
  UINT oldSampleMask, oldStencilRef;
  context->RSGetState(&oldRasterState);
  context->OMGetBlendState(&oldBlendState, oldBlendFactor, &oldSampleMask);
  context->OMGetDepthStencilState(&oldDepthState, &oldStencilRef);

  // Save shaders
  ID3D11VertexShader *oldVS = nullptr;
  ID3D11PixelShader *oldPS = nullptr;
  context->VSGetShader(&oldVS, nullptr, nullptr);
  context->PSGetShader(&oldPS, nullptr, nullptr);

  // Save IA state
  ID3D11InputLayout *oldInputLayout = nullptr;
  D3D11_PRIMITIVE_TOPOLOGY oldTopology;
  ID3D11Buffer *oldVB = nullptr;
  UINT oldVBStride = 0, oldVBOffset = 0;
  ID3D11Buffer *oldIB = nullptr;
  DXGI_FORMAT oldIBFormat = DXGI_FORMAT_UNKNOWN;
  UINT oldIBOffset = 0;
  context->IAGetInputLayout(&oldInputLayout);
  context->IAGetPrimitiveTopology(&oldTopology);
  context->IAGetVertexBuffers(0, 1, &oldVB, &oldVBStride, &oldVBOffset);
  context->IAGetIndexBuffer(&oldIB, &oldIBFormat, &oldIBOffset);

  // Save VS constant buffer and PS resources
  ID3D11Buffer *oldVSCB = nullptr;
  ID3D11ShaderResourceView *oldPSSRV = nullptr;
  ID3D11SamplerState *oldPSSampler = nullptr;
  context->VSGetConstantBuffers(0, 1, &oldVSCB);
  context->PSGetShaderResources(0, 1, &oldPSSRV);
  context->PSGetSamplers(0, 1, &oldPSSampler);

  // Save viewport
  UINT oldNumViewports = 1;
  D3D11_VIEWPORT oldViewport = {};
  context->RSGetViewports(&oldNumViewports, &oldViewport);

  // Set state
  context->RSSetState(rasterState.Get());
  float blendFactor[4] = {0, 0, 0, 0};
  context->OMSetBlendState(blendState.Get(), blendFactor, 0xFFFFFFFF);
  context->OMSetDepthStencilState(depthState.Get(), 0);

  // Set shaders and resources
  context->VSSetShader(vertexShader.Get(), nullptr, 0);
  context->VSSetConstantBuffers(0, 1, constantBuffer.GetAddressOf());
  context->PSSetSamplers(0, 1, sampler.GetAddressOf());
  context->PSSetShaderResources(0, 1, fontTextureSRV.GetAddressOf());
  context->IASetInputLayout(inputLayout.Get());

  UINT stride = sizeof(DrawVertex);
  UINT offset = 0;
  context->IASetVertexBuffers(0, 1, vertexBuffer.GetAddressOf(), &stride,
                              &offset);
  context->IASetIndexBuffer(indexBuffer.Get(), DXGI_FORMAT_R16_UINT, 0);
  context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

  // Set viewport
  D3D11_VIEWPORT vp = {};
  vp.Width = (float)viewportWidth;
  vp.Height = (float)viewportHeight;
  vp.MaxDepth = 1.0f;
  context->RSSetViewports(1, &vp);

  // Draw each command
  for (const auto &cmd : commands) {
    if (cmd.useTexture) {
      context->PSSetShader(pixelShader.Get(), nullptr, 0);
    } else {
      context->PSSetShader(pixelShaderSolid.Get(), nullptr, 0);
    }
    // vertexOffset should be 0 since indices are already absolute
    context->DrawIndexed(cmd.indexCount, cmd.indexOffset, 0);
  }

  // Restore full pipeline state and release raw pointers
  context->RSSetState(oldRasterState);
  context->OMSetBlendState(oldBlendState, oldBlendFactor, oldSampleMask);
  context->OMSetDepthStencilState(oldDepthState, oldStencilRef);
  context->VSSetShader(oldVS, nullptr, 0);
  context->PSSetShader(oldPS, nullptr, 0);
  context->IASetInputLayout(oldInputLayout);
  context->IASetPrimitiveTopology(oldTopology);
  context->IASetVertexBuffers(0, 1, &oldVB, &oldVBStride, &oldVBOffset);
  context->IASetIndexBuffer(oldIB, oldIBFormat, oldIBOffset);
  context->VSSetConstantBuffers(0, 1, &oldVSCB);
  context->PSSetShaderResources(0, 1, &oldPSSRV);
  context->PSSetSamplers(0, 1, &oldPSSampler);
  if (oldNumViewports > 0) {
    context->RSSetViewports(oldNumViewports, &oldViewport);
  }

  // Release saved state references
  if (oldRasterState)
    oldRasterState->Release();
  if (oldBlendState)
    oldBlendState->Release();
  if (oldDepthState)
    oldDepthState->Release();
  if (oldVS)
    oldVS->Release();
  if (oldPS)
    oldPS->Release();
  if (oldInputLayout)
    oldInputLayout->Release();
  if (oldVB)
    oldVB->Release();
  if (oldIB)
    oldIB->Release();
  if (oldVSCB)
    oldVSCB->Release();
  if (oldPSSRV)
    oldPSSRV->Release();
  if (oldPSSampler)
    oldPSSampler->Release();
}

} // namespace CustomOverlay
