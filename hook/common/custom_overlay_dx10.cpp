/**
 * Custom Overlay - DX10 Backend Implementation
 */

#include "custom_overlay_dx10.h"
#include "overlay_shader_bytecode.h"
#include <cstring>

namespace CustomOverlay {

DX10Backend::DX10Backend(ID3D10Device *dev) : device(dev) {}

DX10Backend::~DX10Backend() { Shutdown(); }

bool DX10Backend::Initialize(int fontTextureWidth, int fontTextureHeight,
                             const uint8_t *fontTextureData) {
  if (initialized || !device)
    return false;

  // Create font texture - convert RGBA to BGRA for D3D10
  std::vector<uint8_t> bgraData(fontTextureWidth * fontTextureHeight * 4);
  for (int i = 0; i < fontTextureWidth * fontTextureHeight; i++) {
    bgraData[i * 4 + 0] = fontTextureData[i * 4 + 2]; // B
    bgraData[i * 4 + 1] = fontTextureData[i * 4 + 1]; // G
    bgraData[i * 4 + 2] = fontTextureData[i * 4 + 0]; // R
    bgraData[i * 4 + 3] = fontTextureData[i * 4 + 3]; // A
  }

  D3D10_TEXTURE2D_DESC texDesc = {};
  texDesc.Width = fontTextureWidth;
  texDesc.Height = fontTextureHeight;
  texDesc.MipLevels = 1;
  texDesc.ArraySize = 1;
  texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  texDesc.SampleDesc.Count = 1;
  texDesc.Usage = D3D10_USAGE_DEFAULT;
  texDesc.BindFlags = D3D10_BIND_SHADER_RESOURCE;

  D3D10_SUBRESOURCE_DATA initData = {};
  initData.pSysMem = bgraData.data();
  initData.SysMemPitch = fontTextureWidth * 4;

  HRESULT hr = device->CreateTexture2D(&texDesc, &initData, &fontTexture);
  if (FAILED(hr))
    return false;

  // Create SRV
  hr = device->CreateShaderResourceView(fontTexture, nullptr, &fontTextureView);
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

void DX10Backend::Shutdown() {
  if (!initialized)
    return;

  if (fontTexture)
    fontTexture->Release();
  if (fontTextureView)
    fontTextureView->Release();
  if (vertexBuffer)
    vertexBuffer->Release();
  if (indexBuffer)
    indexBuffer->Release();
  if (constantBuffer)
    constantBuffer->Release();
  if (inputLayout)
    inputLayout->Release();
  if (vertexShader)
    vertexShader->Release();
  if (pixelShader)
    pixelShader->Release();
  if (pixelShaderSolid)
    pixelShaderSolid->Release();
  if (blendState)
    blendState->Release();
  if (samplerState)
    samplerState->Release();
  if (rasterizerState)
    rasterizerState->Release();
  if (depthStencilState)
    depthStencilState->Release();

  fontTexture = nullptr;
  fontTextureView = nullptr;
  vertexBuffer = nullptr;
  indexBuffer = nullptr;
  constantBuffer = nullptr;
  inputLayout = nullptr;
  vertexShader = nullptr;
  pixelShader = nullptr;
  pixelShaderSolid = nullptr;
  blendState = nullptr;
  samplerState = nullptr;
  rasterizerState = nullptr;
  depthStencilState = nullptr;
  device = nullptr;

  initialized = false;
}

bool DX10Backend::CreateShaders() {
  // Use pre-compiled shader bytecode (shared with DX11 - same shader model 4.0)
  HRESULT hr =
      device->CreateVertexShader(g_VS_4_0, sizeof(g_VS_4_0), &vertexShader);
  if (FAILED(hr))
    return false;

  hr = device->CreatePixelShader(g_PS_Textured_4_0, sizeof(g_PS_Textured_4_0),
                                 &pixelShader);
  if (FAILED(hr))
    return false;

  hr = device->CreatePixelShader(g_PS_Solid_4_0, sizeof(g_PS_Solid_4_0),
                                 &pixelShaderSolid);
  if (FAILED(hr))
    return false;

  // Create input layout
  D3D10_INPUT_ELEMENT_DESC layout[] = {
      {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0,
       D3D10_INPUT_PER_VERTEX_DATA, 0},
      {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8,
       D3D10_INPUT_PER_VERTEX_DATA, 0},
      {"COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 16,
       D3D10_INPUT_PER_VERTEX_DATA, 0},
  };

  hr = device->CreateInputLayout(layout, 3, g_VS_4_0, sizeof(g_VS_4_0),
                                 &inputLayout);
  if (FAILED(hr))
    return false;

  return true;
}

bool DX10Backend::CreateBuffers() {
  // Constant buffer for viewport size (required by vertex shader)
  D3D10_BUFFER_DESC cbDesc = {};
  cbDesc.ByteWidth = 16; // float2 viewportSize + float2 padding
  cbDesc.Usage = D3D10_USAGE_DYNAMIC;
  cbDesc.BindFlags = D3D10_BIND_CONSTANT_BUFFER;
  cbDesc.CPUAccessFlags = D3D10_CPU_ACCESS_WRITE;

  HRESULT hr = device->CreateBuffer(&cbDesc, nullptr, &constantBuffer);
  if (FAILED(hr))
    return false;

  // Vertex buffer (will be resized as needed)
  vertexBufferSize = 4096;
  D3D10_BUFFER_DESC vbDesc = {};
  vbDesc.ByteWidth = (UINT)(vertexBufferSize * sizeof(DrawVertex));
  vbDesc.Usage = D3D10_USAGE_DYNAMIC;
  vbDesc.BindFlags = D3D10_BIND_VERTEX_BUFFER;
  vbDesc.CPUAccessFlags = D3D10_CPU_ACCESS_WRITE;

  hr = device->CreateBuffer(&vbDesc, nullptr, &vertexBuffer);
  if (FAILED(hr))
    return false;

  // Index buffer
  indexBufferSize = 8192;
  D3D10_BUFFER_DESC ibDesc = {};
  ibDesc.ByteWidth = (UINT)(indexBufferSize * sizeof(uint16_t));
  ibDesc.Usage = D3D10_USAGE_DYNAMIC;
  ibDesc.BindFlags = D3D10_BIND_INDEX_BUFFER;
  ibDesc.CPUAccessFlags = D3D10_CPU_ACCESS_WRITE;

  hr = device->CreateBuffer(&ibDesc, nullptr, &indexBuffer);
  if (FAILED(hr))
    return false;

  return true;
}

bool DX10Backend::CreateStates() {
  // Sampler
  D3D10_SAMPLER_DESC sampDesc = {};
  sampDesc.Filter = D3D10_FILTER_MIN_MAG_MIP_LINEAR;
  sampDesc.AddressU = D3D10_TEXTURE_ADDRESS_CLAMP;
  sampDesc.AddressV = D3D10_TEXTURE_ADDRESS_CLAMP;
  sampDesc.AddressW = D3D10_TEXTURE_ADDRESS_CLAMP;
  sampDesc.MaxLOD = D3D10_FLOAT32_MAX;

  HRESULT hr = device->CreateSamplerState(&sampDesc, &samplerState);
  if (FAILED(hr))
    return false;

  // Blend state (alpha blending)
  D3D10_BLEND_DESC blendDesc = {};
  blendDesc.BlendEnable[0] = TRUE;
  blendDesc.SrcBlend = D3D10_BLEND_SRC_ALPHA;
  blendDesc.DestBlend = D3D10_BLEND_INV_SRC_ALPHA;
  blendDesc.BlendOp = D3D10_BLEND_OP_ADD;
  blendDesc.SrcBlendAlpha = D3D10_BLEND_ONE;
  blendDesc.DestBlendAlpha = D3D10_BLEND_INV_SRC_ALPHA;
  blendDesc.BlendOpAlpha = D3D10_BLEND_OP_ADD;
  blendDesc.RenderTargetWriteMask[0] = D3D10_COLOR_WRITE_ENABLE_ALL;

  hr = device->CreateBlendState(&blendDesc, &blendState);
  if (FAILED(hr))
    return false;

  // Rasterizer state
  D3D10_RASTERIZER_DESC rasterDesc = {};
  rasterDesc.FillMode = D3D10_FILL_SOLID;
  rasterDesc.CullMode = D3D10_CULL_NONE;
  rasterDesc.ScissorEnable = FALSE;
  rasterDesc.DepthClipEnable = TRUE;

  hr = device->CreateRasterizerState(&rasterDesc, &rasterizerState);
  if (FAILED(hr))
    return false;

  // Depth stencil state (disable depth)
  D3D10_DEPTH_STENCIL_DESC depthDesc = {};
  depthDesc.DepthEnable = FALSE;
  depthDesc.DepthWriteMask = D3D10_DEPTH_WRITE_MASK_ALL;
  depthDesc.DepthFunc = D3D10_COMPARISON_ALWAYS;

  hr = device->CreateDepthStencilState(&depthDesc, &depthStencilState);
  if (FAILED(hr))
    return false;

  return true;
}

void DX10Backend::Render(const std::vector<DrawVertex> &vertices,
                         const std::vector<uint16_t> &indices,
                         const std::vector<DrawCommand> &commands,
                         int viewportWidth, int viewportHeight) {
  if (!initialized || !device || vertices.empty() || commands.empty())
    return;

  // Resize buffers if needed
  if (vertices.size() > vertexBufferSize) {
    vertexBufferSize = vertices.size() * 2;
    D3D10_BUFFER_DESC vbDesc = {};
    vbDesc.ByteWidth = (UINT)(vertexBufferSize * sizeof(DrawVertex));
    vbDesc.Usage = D3D10_USAGE_DYNAMIC;
    vbDesc.BindFlags = D3D10_BIND_VERTEX_BUFFER;
    vbDesc.CPUAccessFlags = D3D10_CPU_ACCESS_WRITE;
    if (vertexBuffer)
      vertexBuffer->Release();
    device->CreateBuffer(&vbDesc, nullptr, &vertexBuffer);
  }

  if (indices.size() > indexBufferSize) {
    indexBufferSize = indices.size() * 2;
    D3D10_BUFFER_DESC ibDesc = {};
    ibDesc.ByteWidth = (UINT)(indexBufferSize * sizeof(uint16_t));
    ibDesc.Usage = D3D10_USAGE_DYNAMIC;
    ibDesc.BindFlags = D3D10_BIND_INDEX_BUFFER;
    ibDesc.CPUAccessFlags = D3D10_CPU_ACCESS_WRITE;
    if (indexBuffer)
      indexBuffer->Release();
    device->CreateBuffer(&ibDesc, nullptr, &indexBuffer);
  }

  // Update vertex buffer
  void *mapped = nullptr;
  HRESULT hr = vertexBuffer->Map(D3D10_MAP_WRITE_DISCARD, 0, &mapped);
  if (SUCCEEDED(hr)) {
    memcpy(mapped, vertices.data(), vertices.size() * sizeof(DrawVertex));
    vertexBuffer->Unmap();
  }

  // Update index buffer
  mapped = nullptr;
  hr = indexBuffer->Map(D3D10_MAP_WRITE_DISCARD, 0, &mapped);
  if (SUCCEEDED(hr)) {
    memcpy(mapped, indices.data(), indices.size() * sizeof(uint16_t));
    indexBuffer->Unmap();
  }

  // Update constant buffer with viewport size
  mapped = nullptr;
  hr = constantBuffer->Map(D3D10_MAP_WRITE_DISCARD, 0, &mapped);
  if (SUCCEEDED(hr)) {
    float *cb = (float *)mapped;
    cb[0] = (float)viewportWidth;
    cb[1] = (float)viewportHeight;
    cb[2] = 0.0f;
    cb[3] = 0.0f;
    constantBuffer->Unmap();
  }

  // Save pipeline state
  ID3D10RasterizerState *oldRasterState = nullptr;
  ID3D10BlendState *oldBlendState = nullptr;
  ID3D10DepthStencilState *oldDepthState = nullptr;
  FLOAT oldBlendFactor[4];
  UINT oldSampleMask = 0;
  UINT oldStencilRef = 0;
  device->RSGetState(&oldRasterState);
  device->OMGetBlendState(&oldBlendState, oldBlendFactor, &oldSampleMask);
  device->OMGetDepthStencilState(&oldDepthState, &oldStencilRef);

  // Save shaders
  ID3D10VertexShader *oldVS = nullptr;
  ID3D10PixelShader *oldPS = nullptr;
  device->VSGetShader(&oldVS);
  device->PSGetShader(&oldPS);

  // Save IA state
  ID3D10InputLayout *oldInputLayout = nullptr;
  D3D10_PRIMITIVE_TOPOLOGY oldTopology;
  ID3D10Buffer *oldVB = nullptr;
  UINT oldVBStride = 0, oldVBOffset = 0;
  ID3D10Buffer *oldIB = nullptr;
  DXGI_FORMAT oldIBFormat = DXGI_FORMAT_UNKNOWN;
  UINT oldIBOffset = 0;
  device->IAGetInputLayout(&oldInputLayout);
  device->IAGetPrimitiveTopology(&oldTopology);
  device->IAGetVertexBuffers(0, 1, &oldVB, &oldVBStride, &oldVBOffset);
  device->IAGetIndexBuffer(&oldIB, &oldIBFormat, &oldIBOffset);

  // Save PS resources
  ID3D10ShaderResourceView *oldPSSRV = nullptr;
  ID3D10SamplerState *oldPSSampler = nullptr;
  device->PSGetShaderResources(0, 1, &oldPSSRV);
  device->PSGetSamplers(0, 1, &oldPSSampler);

  // Save VS constant buffer
  ID3D10Buffer *oldVSCB = nullptr;
  device->VSGetConstantBuffers(0, 1, &oldVSCB);

  // Save viewport
  UINT oldNumViewports = 1;
  D3D10_VIEWPORT oldViewport = {};
  device->RSGetViewports(&oldNumViewports, &oldViewport);

  // Set state
  device->RSSetState(rasterizerState);
  float blendFactor[4] = {0, 0, 0, 0};
  device->OMSetBlendState(blendState, blendFactor, 0xFFFFFFFF);
  device->OMSetDepthStencilState(depthStencilState, 0);

  // Set shaders and resources
  device->VSSetShader(vertexShader);
  device->VSSetConstantBuffers(0, 1, &constantBuffer);
  device->PSSetSamplers(0, 1, &samplerState);
  device->PSSetShaderResources(0, 1, &fontTextureView);
  device->IASetInputLayout(inputLayout);

  UINT stride = sizeof(DrawVertex);
  UINT offset = 0;
  device->IASetVertexBuffers(0, 1, &vertexBuffer, &stride, &offset);
  device->IASetIndexBuffer(indexBuffer, DXGI_FORMAT_R16_UINT, 0);
  device->IASetPrimitiveTopology(D3D10_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

  // Set viewport
  D3D10_VIEWPORT vp = {};
  vp.Width = viewportWidth;
  vp.Height = viewportHeight;
  vp.MaxDepth = 1.0f;
  device->RSSetViewports(1, &vp);

  // Draw each command
  for (const auto &cmd : commands) {
    if (cmd.useTexture) {
      device->PSSetShader(pixelShader);
    } else {
      device->PSSetShader(pixelShaderSolid);
    }
    device->DrawIndexed(cmd.indexCount, cmd.indexOffset, 0);
  }

  // Restore pipeline state
  device->RSSetState(oldRasterState);
  device->OMSetBlendState(oldBlendState, oldBlendFactor, oldSampleMask);
  device->OMSetDepthStencilState(oldDepthState, oldStencilRef);
  device->VSSetShader(oldVS);
  device->VSSetConstantBuffers(0, 1, &oldVSCB);
  device->PSSetShader(oldPS);
  device->IASetInputLayout(oldInputLayout);
  device->IASetPrimitiveTopology(oldTopology);
  device->IASetVertexBuffers(0, 1, &oldVB, &oldVBStride, &oldVBOffset);
  device->IASetIndexBuffer(oldIB, oldIBFormat, oldIBOffset);
  device->PSSetShaderResources(0, 1, &oldPSSRV);
  device->PSSetSamplers(0, 1, &oldPSSampler);
  if (oldNumViewports > 0) {
    device->RSSetViewports(oldNumViewports, &oldViewport);
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