/**
 * Custom Overlay - DX12 Backend Implementation
 */

#include "custom_overlay_dx12.h"
#include "hook_common.h"
#include <cstring>
#include <d3dcompiler.h>

namespace CustomOverlay {

// Same shader source as DX11 (compatible with SM 5.0)
static const char *g_VertexShaderSrc = R"(
cbuffer Constants : register(b0) {
    float2 viewportSize;
    float2 padding;
};

struct VS_INPUT {
    float2 pos : POSITION;
    float2 uv : TEXCOORD0;
    float4 col : COLOR0;
};

struct PS_INPUT {
    float4 pos : SV_POSITION;
    float2 uv : TEXCOORD0;
    float4 col : COLOR0;
};

PS_INPUT main(VS_INPUT input) {
    PS_INPUT output;
    output.pos.x = (input.pos.x / viewportSize.x) * 2.0 - 1.0;
    output.pos.y = 1.0 - (input.pos.y / viewportSize.y) * 2.0;
    output.pos.z = 0.0;
    output.pos.w = 1.0;
    output.uv = input.uv;
    output.col = input.col;
    return output;
}
)";

static const char *g_PixelShaderSrc = R"(
Texture2D fontTexture : register(t0);
SamplerState fontSampler : register(s0);

struct PS_INPUT {
    float4 pos : SV_POSITION;
    float2 uv : TEXCOORD0;
    float4 col : COLOR0;
};

float4 main(PS_INPUT input) : SV_Target {
    float4 texColor = fontTexture.Sample(fontSampler, input.uv);
    return float4(input.col.rgb, input.col.a * texColor.a);
}
)";

static const char *g_PixelShaderSolidSrc = R"(
struct PS_INPUT {
    float4 pos : SV_POSITION;
    float2 uv : TEXCOORD0;
    float4 col : COLOR0;
};

float4 main(PS_INPUT input) : SV_Target {
    return input.col;
}
)";

DX12Backend::DX12Backend(ID3D12Device *dev, ID3D12CommandQueue *queue,
                         DXGI_FORMAT format)
    : device(dev), commandQueue(queue), rtvFormat(format) {}

DX12Backend::~DX12Backend() { Shutdown(); }

bool DX12Backend::Initialize(int fontTextureWidth, int fontTextureHeight,
                             const uint8_t *fontTextureData) {
  if (initialized || !device) {
    HookLog("DX12 Overlay: Initialize - Already initialized or no device");
    return false;
  }

  if (!CreateRootSignature()) {
    HookLog("DX12 Overlay: Initialize - CreateRootSignature failed");
    return false;
  }
  if (!CreatePipelineState()) {
    HookLog("DX12 Overlay: Initialize - CreatePipelineState failed");
    return false;
  }
  if (!CreateBuffers()) {
    HookLog("DX12 Overlay: Initialize - CreateBuffers failed");
    return false;
  }
  if (!CreateFontTexture(fontTextureWidth, fontTextureHeight,
                         fontTextureData)) {
    HookLog("DX12 Overlay: Initialize - CreateFontTexture failed");
    return false;
  }

  initialized = true;
  return true;
}

void DX12Backend::Shutdown() {
  // Unmap buffers
  if (vertexBuffer && vertexBufferPtr) {
    vertexBuffer->Unmap(0, nullptr);
    vertexBufferPtr = nullptr;
  }
  if (indexBuffer && indexBufferPtr) {
    indexBuffer->Unmap(0, nullptr);
    indexBufferPtr = nullptr;
  }

  rootSignature.Reset();
  pipelineState.Reset();
  pipelineStateSolid.Reset();
  srvHeap.Reset();
  fontTexture.Reset();
  vertexBuffer.Reset();
  indexBuffer.Reset();
  uploadBuffer.Reset();
  initialized = false;
}

bool DX12Backend::CreateRootSignature() {
  // Root signature: 1 CBV (viewport), 1 SRV (font texture), 1 sampler
  D3D12_DESCRIPTOR_RANGE srvRange = {};
  srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  srvRange.NumDescriptors = 1;
  srvRange.BaseShaderRegister = 0;
  srvRange.RegisterSpace = 0;
  srvRange.OffsetInDescriptorsFromTableStart = 0;

  D3D12_ROOT_PARAMETER params[2] = {};

  // Param 0: Constants (inline)
  params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  params[0].Constants.ShaderRegister = 0;
  params[0].Constants.RegisterSpace = 0;
  params[0].Constants.Num32BitValues = 4; // float2 viewportSize + padding
  params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;

  // Param 1: SRV table
  params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  params[1].DescriptorTable.NumDescriptorRanges = 1;
  params[1].DescriptorTable.pDescriptorRanges = &srvRange;
  params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

  // Static sampler
  D3D12_STATIC_SAMPLER_DESC sampler = {};
  sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
  sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  sampler.MaxLOD = D3D12_FLOAT32_MAX;
  sampler.ShaderRegister = 0;
  sampler.RegisterSpace = 0;
  sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

  D3D12_ROOT_SIGNATURE_DESC rsDesc = {};
  rsDesc.NumParameters = 2;
  rsDesc.pParameters = params;
  rsDesc.NumStaticSamplers = 1;
  rsDesc.pStaticSamplers = &sampler;
  rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

  ComPtr<ID3DBlob> blob, error;
  HRESULT hr = D3D12SerializeRootSignature(
      &rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error);
  if (FAILED(hr)) {
    if (error) {
      HookLog("DX12 Overlay: CreateRootSignature - D3D12SerializeRootSignature "
              "failed, hr=0x%08X, error=%s",
              hr, (char *)error->GetBufferPointer());
    } else {
      HookLog("DX12 Overlay: CreateRootSignature - D3D12SerializeRootSignature "
              "failed, hr=0x%08X",
              hr);
    }
    return false;
  }

  hr = device->CreateRootSignature(0, blob->GetBufferPointer(),
                                   blob->GetBufferSize(),
                                   IID_PPV_ARGS(&rootSignature));
  if (FAILED(hr)) {
    HookLog("DX12 Overlay: CreateRootSignature - CreateRootSignature failed, "
            "hr=0x%08X",
            hr);
    return false;
  }
  return true;
}

bool DX12Backend::CreatePipelineState() {
  UINT compileFlags = D3DCOMPILE_OPTIMIZATION_LEVEL3;
  ComPtr<ID3DBlob> vsBlob, psBlob, psSolidBlob, errorBlob;

  // Compile shaders
  HRESULT hr = D3DCompile(g_VertexShaderSrc, strlen(g_VertexShaderSrc), nullptr,
                          nullptr, nullptr, "main", "vs_5_0", compileFlags, 0,
                          &vsBlob, &errorBlob);
  if (FAILED(hr)) {
    HookLog("DX12 Overlay: CreatePipelineState - Vertex shader compile failed, "
            "hr=0x%08X%s%s",
            hr, errorBlob ? ", error=" : "",
            errorBlob ? (char *)errorBlob->GetBufferPointer() : "");
    return false;
  }

  hr = D3DCompile(g_PixelShaderSrc, strlen(g_PixelShaderSrc), nullptr, nullptr,
                  nullptr, "main", "ps_5_0", compileFlags, 0, &psBlob,
                  &errorBlob);
  if (FAILED(hr)) {
    HookLog("DX12 Overlay: CreatePipelineState - Pixel shader compile failed, "
            "hr=0x%08X%s%s",
            hr, errorBlob ? ", error=" : "",
            errorBlob ? (char *)errorBlob->GetBufferPointer() : "");
    return false;
  }

  hr = D3DCompile(g_PixelShaderSolidSrc, strlen(g_PixelShaderSolidSrc), nullptr,
                  nullptr, nullptr, "main", "ps_5_0", compileFlags, 0,
                  &psSolidBlob, &errorBlob);
  if (FAILED(hr)) {
    HookLog("DX12 Overlay: CreatePipelineState - Solid pixel shader compile "
            "failed, hr=0x%08X%s%s",
            hr, errorBlob ? ", error=" : "",
            errorBlob ? (char *)errorBlob->GetBufferPointer() : "");
    return false;
  }

  // Input layout
  D3D12_INPUT_ELEMENT_DESC inputLayout[] = {
      {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0,
       D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
      {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8,
       D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
      {"COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 16,
       D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
  };

  // Pipeline state desc
  D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
  psoDesc.pRootSignature = rootSignature.Get();
  psoDesc.VS = {vsBlob->GetBufferPointer(), vsBlob->GetBufferSize()};
  psoDesc.PS = {psBlob->GetBufferPointer(), psBlob->GetBufferSize()};
  psoDesc.InputLayout = {inputLayout, 3};
  psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  psoDesc.NumRenderTargets = 1;
  psoDesc.RTVFormats[0] = rtvFormat;
  psoDesc.SampleMask = UINT_MAX;
  psoDesc.SampleDesc.Count = 1;
  psoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
  psoDesc.RasterizerState.DepthClipEnable = TRUE;
  psoDesc.DepthStencilState.DepthEnable = FALSE;

  // Alpha blending
  psoDesc.BlendState.RenderTarget[0].BlendEnable = TRUE;
  psoDesc.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
  psoDesc.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
  psoDesc.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
  psoDesc.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
  psoDesc.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
  psoDesc.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
  psoDesc.BlendState.RenderTarget[0].RenderTargetWriteMask =
      D3D12_COLOR_WRITE_ENABLE_ALL;

  hr = device->CreateGraphicsPipelineState(&psoDesc,
                                           IID_PPV_ARGS(&pipelineState));
  if (FAILED(hr)) {
    HookLog("DX12 Overlay: CreatePipelineState - CreateGraphicsPipelineState "
            "failed, hr=0x%08X",
            hr);
    return false;
  }

  // Solid pixel shader pipeline
  psoDesc.PS = {psSolidBlob->GetBufferPointer(), psSolidBlob->GetBufferSize()};
  hr = device->CreateGraphicsPipelineState(&psoDesc,
                                           IID_PPV_ARGS(&pipelineStateSolid));
  if (FAILED(hr)) {
    HookLog("DX12 Overlay: CreatePipelineState - CreateGraphicsPipelineState "
            "(solid) failed, hr=0x%08X",
            hr);
    return false;
  }
  return true;
}

bool DX12Backend::CreateBuffers() {
  // Create upload heap buffers (CPU-accessible)
  vertexBufferSize = 4096 * sizeof(DrawVertex);
  indexBufferSize = 8192 * sizeof(uint16_t);

  D3D12_HEAP_PROPERTIES heapProps = {};
  heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

  D3D12_RESOURCE_DESC bufferDesc = {};
  bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  bufferDesc.Width = vertexBufferSize;
  bufferDesc.Height = 1;
  bufferDesc.DepthOrArraySize = 1;
  bufferDesc.MipLevels = 1;
  bufferDesc.SampleDesc.Count = 1;
  bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

  HRESULT hr = device->CreateCommittedResource(
      &heapProps, D3D12_HEAP_FLAG_NONE, &bufferDesc,
      D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&vertexBuffer));
  if (FAILED(hr)) {
    HookLog("DX12 Overlay: CreateBuffers - Vertex buffer creation failed, "
            "hr=0x%08X",
            hr);
    return false;
  }

  bufferDesc.Width = indexBufferSize;
  hr = device->CreateCommittedResource(
      &heapProps, D3D12_HEAP_FLAG_NONE, &bufferDesc,
      D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&indexBuffer));
  if (FAILED(hr)) {
    HookLog(
        "DX12 Overlay: CreateBuffers - Index buffer creation failed, hr=0x%08X",
        hr);
    return false;
  }

  // Map buffers persistently
  D3D12_RANGE readRange = {0, 0};
  vertexBuffer->Map(0, &readRange, &vertexBufferPtr);
  indexBuffer->Map(0, &readRange, &indexBufferPtr);

  return true;
}

bool DX12Backend::CreateFontTexture(int width, int height,
                                    const uint8_t *data) {
  // Create SRV heap
  D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
  heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  heapDesc.NumDescriptors = 1;
  heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

  HRESULT hr = device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&srvHeap));
  if (FAILED(hr)) {
    HookLog(
        "DX12 Overlay: CreateFontTexture - SRV heap creation failed, hr=0x%08X",
        hr);
    return false;
  }

  // Create texture resource
  D3D12_HEAP_PROPERTIES heapProps = {};
  heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

  D3D12_RESOURCE_DESC texDesc = {};
  texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  texDesc.Width = width;
  texDesc.Height = height;
  texDesc.DepthOrArraySize = 1;
  texDesc.MipLevels = 1;
  texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  texDesc.SampleDesc.Count = 1;
  texDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
  texDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

  hr = device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE,
                                       &texDesc, D3D12_RESOURCE_STATE_COPY_DEST,
                                       nullptr, IID_PPV_ARGS(&fontTexture));
  if (FAILED(hr)) {
    HookLog("DX12 Overlay: CreateFontTexture - Font texture creation failed, "
            "hr=0x%08X",
            hr);
    return false;
  }

  // Create upload buffer
  UINT64 uploadSize = 0;
  device->GetCopyableFootprints(&texDesc, 0, 1, 0, nullptr, nullptr, nullptr,
                                &uploadSize);

  D3D12_HEAP_PROPERTIES uploadHeapProps = {};
  uploadHeapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

  D3D12_RESOURCE_DESC uploadDesc = {};
  uploadDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  uploadDesc.Width = uploadSize;
  uploadDesc.Height = 1;
  uploadDesc.DepthOrArraySize = 1;
  uploadDesc.MipLevels = 1;
  uploadDesc.SampleDesc.Count = 1;
  uploadDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

  hr = device->CreateCommittedResource(
      &uploadHeapProps, D3D12_HEAP_FLAG_NONE, &uploadDesc,
      D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&uploadBuffer));
  if (FAILED(hr)) {
    HookLog("DX12 Overlay: CreateFontTexture - Upload buffer creation failed, "
            "hr=0x%08X",
            hr);
    return false;
  }

  // Copy data to upload buffer
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
  device->GetCopyableFootprints(&texDesc, 0, 1, 0, &footprint, nullptr, nullptr,
                                nullptr);

  void *uploadPtr;
  uploadBuffer->Map(0, nullptr, &uploadPtr);

  uint8_t *dst = (uint8_t *)uploadPtr;
  for (int y = 0; y < height; y++) {
    memcpy(dst + y * footprint.Footprint.RowPitch, data + y * width * 4,
           width * 4);
  }
  uploadBuffer->Unmap(0, nullptr);

  // Create SRV
  D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
  srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  srvDesc.Texture2D.MipLevels = 1;

  device->CreateShaderResourceView(
      fontTexture.Get(), &srvDesc,
      srvHeap->GetCPUDescriptorHandleForHeapStart());

  // Note: Texture upload and transition must be done via command list
  // This will be handled on first render — save footprint for deferred copy
  fontTextureFootprint = footprint;
  fontTextureDesc = texDesc;
  fontUploaded = false;

  return true;
}

void DX12Backend::SetRenderTarget(ID3D12GraphicsCommandList *cmdList,
                                  D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle) {
  currentCmdList = cmdList;
  currentRTV = rtvHandle;
}

bool DX12Backend::ResizeVertexBuffer(size_t requiredBytes) {
  if (!device)
    return false;

  // Unmap old buffer
  if (vertexBuffer && vertexBufferPtr) {
    vertexBuffer->Unmap(0, nullptr);
    vertexBufferPtr = nullptr;
  }

  // Calculate new size (2x growth factor)
  size_t newSize = vertexBufferSize * 2;
  while (newSize < requiredBytes) {
    newSize *= 2;
  }

  D3D12_HEAP_PROPERTIES heapProps = {};
  heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

  D3D12_RESOURCE_DESC bufferDesc = {};
  bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  bufferDesc.Width = newSize;
  bufferDesc.Height = 1;
  bufferDesc.DepthOrArraySize = 1;
  bufferDesc.MipLevels = 1;
  bufferDesc.SampleDesc.Count = 1;
  bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

  ComPtr<ID3D12Resource> newBuffer;
  HRESULT hr = device->CreateCommittedResource(
      &heapProps, D3D12_HEAP_FLAG_NONE, &bufferDesc,
      D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&newBuffer));
  if (FAILED(hr)) {
    HookLog("DX12 Overlay: ResizeVertexBuffer - Failed to create new buffer "
            "(size=%zu), hr=0x%08X",
            newSize, hr);
    return false;
  }

  vertexBuffer = newBuffer;
  vertexBufferSize = newSize;

  // Map persistently
  D3D12_RANGE readRange = {0, 0};
  vertexBuffer->Map(0, &readRange, &vertexBufferPtr);

  HookLog("DX12 Overlay: Vertex buffer resized to %zu bytes", newSize);
  return true;
}

bool DX12Backend::ResizeIndexBuffer(size_t requiredBytes) {
  if (!device)
    return false;

  // Unmap old buffer
  if (indexBuffer && indexBufferPtr) {
    indexBuffer->Unmap(0, nullptr);
    indexBufferPtr = nullptr;
  }

  // Calculate new size (2x growth factor)
  size_t newSize = indexBufferSize * 2;
  while (newSize < requiredBytes) {
    newSize *= 2;
  }

  D3D12_HEAP_PROPERTIES heapProps = {};
  heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

  D3D12_RESOURCE_DESC bufferDesc = {};
  bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  bufferDesc.Width = newSize;
  bufferDesc.Height = 1;
  bufferDesc.DepthOrArraySize = 1;
  bufferDesc.MipLevels = 1;
  bufferDesc.SampleDesc.Count = 1;
  bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

  ComPtr<ID3D12Resource> newBuffer;
  HRESULT hr = device->CreateCommittedResource(
      &heapProps, D3D12_HEAP_FLAG_NONE, &bufferDesc,
      D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&newBuffer));
  if (FAILED(hr)) {
    HookLog("DX12 Overlay: ResizeIndexBuffer - Failed to create new buffer "
            "(size=%zu), hr=0x%08X",
            newSize, hr);
    return false;
  }

  indexBuffer = newBuffer;
  indexBufferSize = newSize;

  // Map persistently
  D3D12_RANGE readRange = {0, 0};
  indexBuffer->Map(0, &readRange, &indexBufferPtr);

  HookLog("DX12 Overlay: Index buffer resized to %zu bytes", newSize);
  return true;
}

void DX12Backend::Render(const std::vector<DrawVertex> &vertices,
                         const std::vector<uint16_t> &indices,
                         const std::vector<DrawCommand> &commands,
                         int viewportWidth, int viewportHeight) {
  if (!initialized || !currentCmdList || vertices.empty())
    return;

  static int s_renderCount = 0;
  if (++s_renderCount <= 5) {
    HookLog("DX12Backend::Render - START (vertices=%zu, commands=%zu)",
            vertices.size(), commands.size());
  }

  // Deferred font texture upload: copy from upload heap to default heap on
  // first render
  if (!fontUploaded && uploadBuffer && fontTexture) {
    D3D12_TEXTURE_COPY_LOCATION srcLoc = {};
    srcLoc.pResource = uploadBuffer.Get();
    srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    srcLoc.PlacedFootprint = fontTextureFootprint;

    D3D12_TEXTURE_COPY_LOCATION dstLoc = {};
    dstLoc.pResource = fontTexture.Get();
    dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dstLoc.SubresourceIndex = 0;

    currentCmdList->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);

    // Transition from COPY_DEST to PIXEL_SHADER_RESOURCE
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = fontTexture.Get();
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    currentCmdList->ResourceBarrier(1, &barrier);

    fontUploaded = true;
    // Upload buffer can be released after the command list executes,
    // but keeping it alive until then is fine since it's a ComPtr
  }

  // Update vertex buffer with resize support
  size_t vbSize = vertices.size() * sizeof(DrawVertex);
  if (vbSize > vertexBufferSize) {
    if (!ResizeVertexBuffer(vbSize)) {
      HookLog("DX12 Overlay: Render - Failed to resize vertex buffer (needed "
              "%zu bytes)",
              vbSize);
      return;
    }
  }
  if (vertexBufferPtr) {
    memcpy(vertexBufferPtr, vertices.data(), vbSize);
  }

  // Update index buffer with resize support
  size_t ibSize = indices.size() * sizeof(uint16_t);
  if (ibSize > indexBufferSize) {
    if (!ResizeIndexBuffer(ibSize)) {
      HookLog("DX12 Overlay: Render - Failed to resize index buffer (needed "
              "%zu bytes)",
              ibSize);
      return;
    }
  }
  if (indexBufferPtr) {
    memcpy(indexBufferPtr, indices.data(), ibSize);
  }

  // Set state
  currentCmdList->SetGraphicsRootSignature(rootSignature.Get());

  ID3D12DescriptorHeap *heaps[] = {srvHeap.Get()};
  currentCmdList->SetDescriptorHeaps(1, heaps);

  // Set constants (viewport size)
  float constants[4] = {(float)viewportWidth, (float)viewportHeight, 0, 0};
  currentCmdList->SetGraphicsRoot32BitConstants(0, 4, constants, 0);

  // Set SRV
  currentCmdList->SetGraphicsRootDescriptorTable(
      1, srvHeap->GetGPUDescriptorHandleForHeapStart());

  // Set render target
  currentCmdList->OMSetRenderTargets(1, &currentRTV, FALSE, nullptr);

  // Set viewport and scissor
  D3D12_VIEWPORT vp = {0, 0, (float)viewportWidth, (float)viewportHeight, 0, 1};
  D3D12_RECT scissor = {0, 0, (LONG)viewportWidth, (LONG)viewportHeight};
  currentCmdList->RSSetViewports(1, &vp);
  currentCmdList->RSSetScissorRects(1, &scissor);

  // Set buffers
  D3D12_VERTEX_BUFFER_VIEW vbv = {};
  vbv.BufferLocation = vertexBuffer->GetGPUVirtualAddress();
  vbv.SizeInBytes = (UINT)vbSize;
  vbv.StrideInBytes = sizeof(DrawVertex);
  currentCmdList->IASetVertexBuffers(0, 1, &vbv);

  D3D12_INDEX_BUFFER_VIEW ibv = {};
  ibv.BufferLocation = indexBuffer->GetGPUVirtualAddress();
  ibv.SizeInBytes = (UINT)ibSize;
  ibv.Format = DXGI_FORMAT_R16_UINT;
  currentCmdList->IASetIndexBuffer(&ibv);

  currentCmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

  if (s_renderCount <= 5) {
    HookLog("DX12Backend::Render - State set, drawing %zu commands",
            commands.size());
  }

  for (const auto &cmd : commands) {
    if (cmd.useTexture) {
      currentCmdList->SetPipelineState(pipelineState.Get());
    } else {
      currentCmdList->SetPipelineState(pipelineStateSolid.Get());
    }
    currentCmdList->DrawIndexedInstanced(cmd.indexCount, 1, cmd.indexOffset,
                                         cmd.vertexOffset, 0);
  }

  if (s_renderCount <= 5) {
    HookLog("DX12Backend::Render - COMPLETE");
  }
}

} // namespace CustomOverlay
