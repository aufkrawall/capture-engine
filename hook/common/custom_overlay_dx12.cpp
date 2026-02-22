/**
 * Custom Overlay - DX12 Backend Implementation
 */

#include "custom_overlay_dx12.h"
#include "hook_common.h"
#include "overlay_shader_bytecode.h"
#include "../apis/dx12_hook.h"
#include <cstring>

namespace CustomOverlay {

static uint64_t s_FrameCounter = 0;
static uint64_t s_RenderCounter = 0;

DX12Backend::DX12Backend(ID3D12Device *dev, ID3D12CommandQueue *queue,
                         DXGI_FORMAT format)
    : device(dev), commandQueue(queue), rtvFormat(format) {
    DX12_DEBUG_STEP("Constructor", "device=%p, queue=%p, format=%d", dev, queue, format);
}

DX12Backend::~DX12Backend() { 
    DX12_DEBUG_STEP("Destructor", "Shutting down backend");
    Shutdown(); 
}

bool DX12Backend::Initialize(int fontTextureWidth, int fontTextureHeight,
                             const uint8_t *fontTextureData) {
    DX12_DEBUG_STEP("Initialize", "START - fontTex=%dx%d, initialized=%d, device=%p", 
                    fontTextureWidth, fontTextureHeight, initialized, device);
    
    if (initialized || !device) {
        DX12_DEBUG_STEP("Initialize", "FAILED - Already initialized or no device");
        HookLog("DX12 Overlay: Initialize - Already initialized or no device");
        return false;
    }

    DX12_DEBUG_STEP("Initialize", "Step 1/4: CreateRootSignature");
    if (!CreateRootSignature()) {
        DX12_DEBUG_STEP("Initialize", "FAILED - CreateRootSignature");
        HookLog("DX12 Overlay: Initialize - CreateRootSignature failed");
        return false;
    }
    DX12_DEBUG_STEP("Initialize", "Step 1/4: CreateRootSignature - SUCCESS");
    
    DX12_DEBUG_STEP("Initialize", "Step 2/4: CreatePipelineState");
    if (!CreatePipelineState()) {
        DX12_DEBUG_STEP("Initialize", "FAILED - CreatePipelineState");
        HookLog("DX12 Overlay: Initialize - CreatePipelineState failed");
        return false;
    }
    DX12_DEBUG_STEP("Initialize", "Step 2/4: CreatePipelineState - SUCCESS");
    
    DX12_DEBUG_STEP("Initialize", "Step 3/4: CreateBuffers");
    if (!CreateBuffers()) {
        DX12_DEBUG_STEP("Initialize", "FAILED - CreateBuffers");
        HookLog("DX12 Overlay: Initialize - CreateBuffers failed");
        return false;
    }
    DX12_DEBUG_STEP("Initialize", "Step 3/4: CreateBuffers - SUCCESS");
    
    DX12_DEBUG_STEP("Initialize", "Step 4/4: CreateFontTexture");
    if (!CreateFontTexture(fontTextureWidth, fontTextureHeight,
                           fontTextureData)) {
        DX12_DEBUG_STEP("Initialize", "FAILED - CreateFontTexture");
        HookLog("DX12 Overlay: Initialize - CreateFontTexture failed");
        return false;
    }
    DX12_DEBUG_STEP("Initialize", "Step 4/4: CreateFontTexture - SUCCESS");

    initialized = true;
    DX12_DEBUG_STEP("Initialize", "COMPLETE - initialized=true");
    return true;
}

void DX12Backend::Shutdown() {
    DX12_DEBUG_STEP("Shutdown", "START - initialized=%d", initialized);
    
    if (vertexBuffer && vertexBufferPtr) {
        DX12_DEBUG_STEP("Shutdown", "Unmapping vertex buffer");
        vertexBuffer->Unmap(0, nullptr);
        vertexBufferPtr = nullptr;
    }
    if (indexBuffer && indexBufferPtr) {
        DX12_DEBUG_STEP("Shutdown", "Unmapping index buffer");
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
    DX12_DEBUG_STEP("Shutdown", "COMPLETE");
}

bool DX12Backend::CreateRootSignature() {
    DX12_DEBUG_STEP("CreateRootSignature", "START");
    
    D3D12_DESCRIPTOR_RANGE srvRange = {};
    srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange.NumDescriptors = 1;
    srvRange.BaseShaderRegister = 0;
    srvRange.RegisterSpace = 0;
    srvRange.OffsetInDescriptorsFromTableStart = 0;
    DX12_DEBUG_STEP("CreateRootSignature", "SRV range configured: reg=%d, num=%d", 
                    srvRange.BaseShaderRegister, srvRange.NumDescriptors);

    D3D12_ROOT_PARAMETER params[2] = {};

    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.ShaderRegister = 0;
    params[0].Constants.RegisterSpace = 0;
    params[0].Constants.Num32BitValues = 4;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    DX12_DEBUG_STEP("CreateRootSignature", "Param 0: 32bit constants, reg=%d, numValues=%d",
                    params[0].Constants.ShaderRegister, params[0].Constants.Num32BitValues);

    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 1;
    params[1].DescriptorTable.pDescriptorRanges = &srvRange;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    DX12_DEBUG_STEP("CreateRootSignature", "Param 1: Descriptor table, ranges=%d",
                    params[1].DescriptorTable.NumDescriptorRanges);

    D3D12_STATIC_SAMPLER_DESC sampler = {};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderRegister = 0;
    sampler.RegisterSpace = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    DX12_DEBUG_STEP("CreateRootSignature", "Static sampler configured: reg=%d", sampler.ShaderRegister);

    D3D12_ROOT_SIGNATURE_DESC rsDesc = {};
    rsDesc.NumParameters = 2;
    rsDesc.pParameters = params;
    rsDesc.NumStaticSamplers = 1;
    rsDesc.pStaticSamplers = &sampler;
    rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> blob, error;
    DX12_DEBUG_STEP("CreateRootSignature", "Calling D3D12SerializeRootSignature");
    HRESULT hr = D3D12SerializeRootSignature(
        &rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error);
    DX12_DEBUG_STEP("CreateRootSignature", "Serialization result: hr=0x%08X (%s)", 
                    hr, SUCCEEDED(hr) ? "OK" : "FAILED");
    if (FAILED(hr)) {
        if (error) {
            DX12_DEBUG_STEP("CreateRootSignature", "Serialization error message: %s", 
                            (char *)error->GetBufferPointer());
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
    DX12_DEBUG_STEP("CreateRootSignature", "Blob size=%zu", blob->GetBufferSize());

    DX12_DEBUG_STEP("CreateRootSignature", "Calling device->CreateRootSignature");
    hr = device->CreateRootSignature(0, blob->GetBufferPointer(),
                                     blob->GetBufferSize(),
                                     IID_PPV_ARGS(&rootSignature));
    DX12_DEBUG_STEP("CreateRootSignature", "CreateRootSignature result: hr=0x%08X (%s), rootSig=%p",
                    hr, SUCCEEDED(hr) ? "OK" : "FAILED", rootSignature.Get());
    if (FAILED(hr)) {
        HookLog("DX12 Overlay: CreateRootSignature - CreateRootSignature failed, "
                "hr=0x%08X",
              hr);
        return false;
    }
    DX12_DEBUG_STEP("CreateRootSignature", "SUCCESS");
    return true;
}

bool DX12Backend::CreatePipelineState() {
    DX12_DEBUG_STEP("CreatePipelineState", "START");
    
    D3D12_INPUT_ELEMENT_DESC inputLayout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 16,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    };
    DX12_DEBUG_STEP("CreatePipelineState", "Input layout: %zu elements", 
                    sizeof(inputLayout) / sizeof(inputLayout[0]));

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.pRootSignature = rootSignature.Get();
    psoDesc.VS = {g_VS_5_0, sizeof(g_VS_5_0)};
    psoDesc.PS = {g_PS_Textured_5_0, sizeof(g_PS_Textured_5_0)};
    DX12_DEBUG_STEP("CreatePipelineState", "VS size=%zu, PS_Textured size=%zu",
                    sizeof(g_VS_5_0), sizeof(g_PS_Textured_5_0));
    psoDesc.InputLayout = {inputLayout, 3};
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = rtvFormat;
    DX12_DEBUG_STEP("CreatePipelineState", "RTV format=%d", rtvFormat);
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.SampleDesc.Count = 1;
    psoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    psoDesc.RasterizerState.DepthClipEnable = TRUE;
    psoDesc.DepthStencilState.DepthEnable = FALSE;

    psoDesc.BlendState.RenderTarget[0].BlendEnable = TRUE;
    psoDesc.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
    psoDesc.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    psoDesc.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
    psoDesc.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
    psoDesc.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
    psoDesc.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
    psoDesc.BlendState.RenderTarget[0].RenderTargetWriteMask =
        D3D12_COLOR_WRITE_ENABLE_ALL;
    DX12_DEBUG_STEP("CreatePipelineState", "Blend state configured (alpha blending)");

    DX12_DEBUG_STEP("CreatePipelineState", "Creating TEXTURED PSO");
    HRESULT hr = device->CreateGraphicsPipelineState(
        &psoDesc, IID_PPV_ARGS(&pipelineState));
    DX12_DEBUG_STEP("CreatePipelineState", "Textured PSO result: hr=0x%08X (%s), pso=%p",
                    hr, SUCCEEDED(hr) ? "OK" : "FAILED", pipelineState.Get());
    if (FAILED(hr)) {
        HookLog("DX12 Overlay: CreatePipelineState failed, hr=0x%08X", hr);
        return false;
    }

    DX12_DEBUG_STEP("CreatePipelineState", "Creating SOLID PSO");
    psoDesc.PS = {g_PS_Solid_5_0, sizeof(g_PS_Solid_5_0)};
    DX12_DEBUG_STEP("CreatePipelineState", "PS_Solid size=%zu", sizeof(g_PS_Solid_5_0));
    hr = device->CreateGraphicsPipelineState(&psoDesc,
                                             IID_PPV_ARGS(&pipelineStateSolid));
    DX12_DEBUG_STEP("CreatePipelineState", "Solid PSO result: hr=0x%08X (%s), pso=%p",
                    hr, SUCCEEDED(hr) ? "OK" : "FAILED", pipelineStateSolid.Get());
    if (FAILED(hr)) {
        HookLog("DX12 Overlay: CreatePipelineState (solid) failed, hr=0x%08X", hr);
        return false;
    }
    DX12_DEBUG_STEP("CreatePipelineState", "SUCCESS - textured=%p, solid=%p",
                    pipelineState.Get(), pipelineStateSolid.Get());
    return true;
}

bool DX12Backend::CreateBuffers() {
    DX12_DEBUG_STEP("CreateBuffers", "START");
    
    vertexBufferSize = 4096 * sizeof(DrawVertex);
    indexBufferSize = 8192 * sizeof(uint16_t);
    DX12_DEBUG_STEP("CreateBuffers", "Target sizes: vertex=%zu bytes, index=%zu bytes",
                    vertexBufferSize, indexBufferSize);

    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;
    DX12_DEBUG_STEP("CreateBuffers", "Heap type: UPLOAD");

    D3D12_RESOURCE_DESC bufferDesc = {};
    bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufferDesc.Width = vertexBufferSize;
    bufferDesc.Height = 1;
    bufferDesc.DepthOrArraySize = 1;
    bufferDesc.MipLevels = 1;
    bufferDesc.SampleDesc.Count = 1;
    bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    DX12_DEBUG_STEP("CreateBuffers", "Creating vertex buffer (%zu bytes)", vertexBufferSize);
    HRESULT hr = device->CreateCommittedResource(
        &heapProps, D3D12_HEAP_FLAG_NONE, &bufferDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&vertexBuffer));
    DX12_DEBUG_STEP("CreateBuffers", "Vertex buffer result: hr=0x%08X (%s), resource=%p",
                    hr, SUCCEEDED(hr) ? "OK" : "FAILED", vertexBuffer.Get());
    if (FAILED(hr)) {
        HookLog("DX12 Overlay: CreateBuffers - Vertex buffer creation failed, "
                "hr=0x%08X",
              hr);
        return false;
    }

    bufferDesc.Width = indexBufferSize;
    DX12_DEBUG_STEP("CreateBuffers", "Creating index buffer (%zu bytes)", indexBufferSize);
    hr = device->CreateCommittedResource(
        &heapProps, D3D12_HEAP_FLAG_NONE, &bufferDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&indexBuffer));
    DX12_DEBUG_STEP("CreateBuffers", "Index buffer result: hr=0x%08X (%s), resource=%p",
                    hr, SUCCEEDED(hr) ? "OK" : "FAILED", indexBuffer.Get());
    if (FAILED(hr)) {
        HookLog(
            "DX12 Overlay: CreateBuffers - Index buffer creation failed, hr=0x%08X",
          hr);
        return false;
    }

    DX12_DEBUG_STEP("CreateBuffers", "Mapping buffers persistently");
    D3D12_RANGE readRange = {0, 0};
    vertexBuffer->Map(0, &readRange, &vertexBufferPtr);
    indexBuffer->Map(0, &readRange, &indexBufferPtr);
    DX12_DEBUG_STEP("CreateBuffers", "Mapped: vertexPtr=%p, indexPtr=%p",
                    vertexBufferPtr, indexBufferPtr);

    DX12_DEBUG_STEP("CreateBuffers", "SUCCESS");
    return true;
}

bool DX12Backend::CreateFontTexture(int width, int height,
                                    const uint8_t *data) {
    DX12_DEBUG_STEP("CreateFontTexture", "START - size=%dx%d, data=%p", width, height, data);
    
    DX12_DEBUG_STEP("CreateFontTexture", "Step 1: Creating SRV descriptor heap");
    D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.NumDescriptors = 1;
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

    HRESULT hr = device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&srvHeap));
    DX12_DEBUG_STEP("CreateFontTexture", "SRV heap result: hr=0x%08X (%s), heap=%p",
                    hr, SUCCEEDED(hr) ? "OK" : "FAILED", srvHeap.Get());
    if (FAILED(hr)) {
        HookLog(
            "DX12 Overlay: CreateFontTexture - SRV heap creation failed, hr=0x%08X",
          hr);
        return false;
    }

    DX12_DEBUG_STEP("CreateFontTexture", "Step 2: Creating texture resource");
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
    DX12_DEBUG_STEP("CreateFontTexture", "Texture desc: %llux%u, format=%d, mipLevels=%d",
                    texDesc.Width, texDesc.Height, texDesc.Format, texDesc.MipLevels);

    hr = device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE,
                                         &texDesc, D3D12_RESOURCE_STATE_COPY_DEST,
                                         nullptr, IID_PPV_ARGS(&fontTexture));
    DX12_DEBUG_STEP("CreateFontTexture", "Texture resource result: hr=0x%08X (%s), texture=%p",
                    hr, SUCCEEDED(hr) ? "OK" : "FAILED", fontTexture.Get());
    if (FAILED(hr)) {
        HookLog("DX12 Overlay: CreateFontTexture - Font texture creation failed, "
                "hr=0x%08X",
              hr);
        return false;
    }

    DX12_DEBUG_STEP("CreateFontTexture", "Step 3: Creating upload buffer");
    UINT64 uploadSize = 0;
    device->GetCopyableFootprints(&texDesc, 0, 1, 0, nullptr, nullptr, nullptr,
                                  &uploadSize);
    DX12_DEBUG_STEP("CreateFontTexture", "Required upload buffer size: %llu bytes", uploadSize);

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
    DX12_DEBUG_STEP("CreateFontTexture", "Upload buffer result: hr=0x%08X (%s), buffer=%p",
                    hr, SUCCEEDED(hr) ? "OK" : "FAILED", uploadBuffer.Get());
    if (FAILED(hr)) {
        HookLog("DX12 Overlay: CreateFontTexture - Upload buffer creation failed, "
                "hr=0x%08X",
              hr);
        return false;
    }

    DX12_DEBUG_STEP("CreateFontTexture", "Step 4: Copying font data to upload buffer");
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
    device->GetCopyableFootprints(&texDesc, 0, 1, 0, &footprint, nullptr, nullptr,
                                  nullptr);
    DX12_DEBUG_STEP("CreateFontTexture", "Footprint: rowPitch=%u, width=%u, height=%u",
                    footprint.Footprint.RowPitch, footprint.Footprint.Width, footprint.Footprint.Height);

    void *uploadPtr;
    uploadBuffer->Map(0, nullptr, &uploadPtr);
    DX12_DEBUG_STEP("CreateFontTexture", "Upload buffer mapped: ptr=%p", uploadPtr);

    uint8_t *dst = (uint8_t *)uploadPtr;
    for (int y = 0; y < height; y++) {
        memcpy(dst + y * footprint.Footprint.RowPitch, data + y * width * 4,
               width * 4);
    }
    uploadBuffer->Unmap(0, nullptr);
    DX12_DEBUG_STEP("CreateFontTexture", "Font data copied (%d rows), upload buffer unmapped", height);

    DX12_DEBUG_STEP("CreateFontTexture", "Step 5: Creating SRV");
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MipLevels = 1;

    device->CreateShaderResourceView(
        fontTexture.Get(), &srvDesc,
        srvHeap->GetCPUDescriptorHandleForHeapStart());
    DX12_DEBUG_STEP("CreateFontTexture", "SRV created on heap");

    fontTextureFootprint = footprint;
    fontTextureDesc = texDesc;
    fontUploaded = false;
    DX12_DEBUG_STEP("CreateFontTexture", "SUCCESS - fontUploaded=false (deferred upload)");

    return true;
}

void DX12Backend::SetRenderTarget(ID3D12GraphicsCommandList *cmdList,
                                  D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle) {
    s_FrameCounter++;
    DX12_DEBUG_FRAME(s_FrameCounter, "SetRenderTarget: cmdList=%p, rtvHandle.ptr=%llx",
                     cmdList, rtvHandle.ptr);
    DX12_DEBUG_PTR("SetRenderTarget", "cmdList", cmdList);
    DX12_DEBUG_PTR("SetRenderTarget", "rtvHandle", (void*)rtvHandle.ptr);
    
    currentCmdList = cmdList;
    currentRTV = rtvHandle;
}

bool DX12Backend::ResizeVertexBuffer(size_t requiredBytes) {
    DX12_DEBUG_STEP("ResizeVertexBuffer", "START - required=%zu, current=%zu",
                    requiredBytes, vertexBufferSize);
    
    if (!device) {
        DX12_DEBUG_STEP("ResizeVertexBuffer", "FAILED - no device");
        return false;
    }

    if (vertexBuffer && vertexBufferPtr) {
        DX12_DEBUG_STEP("ResizeVertexBuffer", "Unmapping old vertex buffer");
        vertexBuffer->Unmap(0, nullptr);
        vertexBufferPtr = nullptr;
    }

    size_t newSize = vertexBufferSize * 2;
    while (newSize < requiredBytes) {
        newSize *= 2;
    }
    DX12_DEBUG_STEP("ResizeVertexBuffer", "New size: %zu bytes (old=%zu)",
                    newSize, vertexBufferSize);

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
    DX12_DEBUG_STEP("ResizeVertexBuffer", "Create result: hr=0x%08X (%s)",
                    hr, SUCCEEDED(hr) ? "OK" : "FAILED");
    if (FAILED(hr)) {
        HookLog("DX12 Overlay: ResizeVertexBuffer - Failed to create new buffer "
                "(size=%zu), hr=0x%08X",
              newSize, hr);
        return false;
    }

    vertexBuffer = newBuffer;
    vertexBufferSize = newSize;

    D3D12_RANGE readRange = {0, 0};
    vertexBuffer->Map(0, &readRange, &vertexBufferPtr);
    DX12_DEBUG_STEP("ResizeVertexBuffer", "SUCCESS - new buffer mapped at %p", vertexBufferPtr);

    HookLog("DX12 Overlay: Vertex buffer resized to %zu bytes", newSize);
    return true;
}

bool DX12Backend::ResizeIndexBuffer(size_t requiredBytes) {
    DX12_DEBUG_STEP("ResizeIndexBuffer", "START - required=%zu, current=%zu",
                    requiredBytes, indexBufferSize);
    
    if (!device) {
        DX12_DEBUG_STEP("ResizeIndexBuffer", "FAILED - no device");
        return false;
    }

    if (indexBuffer && indexBufferPtr) {
        DX12_DEBUG_STEP("ResizeIndexBuffer", "Unmapping old index buffer");
        indexBuffer->Unmap(0, nullptr);
        indexBufferPtr = nullptr;
    }

    size_t newSize = indexBufferSize * 2;
    while (newSize < requiredBytes) {
        newSize *= 2;
    }
    DX12_DEBUG_STEP("ResizeIndexBuffer", "New size: %zu bytes (old=%zu)",
                    newSize, indexBufferSize);

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
    DX12_DEBUG_STEP("ResizeIndexBuffer", "Create result: hr=0x%08X (%s)",
                    hr, SUCCEEDED(hr) ? "OK" : "FAILED");
    if (FAILED(hr)) {
        HookLog("DX12 Overlay: ResizeIndexBuffer - Failed to create new buffer "
                "(size=%zu), hr=0x%08X",
              newSize, hr);
        return false;
    }

    indexBuffer = newBuffer;
    indexBufferSize = newSize;

    D3D12_RANGE readRange = {0, 0};
    indexBuffer->Map(0, &readRange, &indexBufferPtr);
    DX12_DEBUG_STEP("ResizeIndexBuffer", "SUCCESS - new buffer mapped at %p", indexBufferPtr);

    HookLog("DX12 Overlay: Index buffer resized to %zu bytes", newSize);
    return true;
}

void DX12Backend::Render(const std::vector<DrawVertex> &vertices,
                         const std::vector<uint16_t> &indices,
                         const std::vector<DrawCommand> &commands,
                         int viewportWidth, int viewportHeight) {
    s_RenderCounter++;
    DX12_DEBUG_FRAME(s_RenderCounter, "Render: vertices=%zu, indices=%zu, commands=%zu, viewport=%dx%d",
                     vertices.size(), indices.size(), commands.size(), viewportWidth, viewportHeight);
    
    if (!initialized || !currentCmdList || vertices.empty()) {
        DX12_DEBUG_STEP("Render", "EARLY RETURN - initialized=%d, cmdList=%p, verts=%zu",
                        initialized, currentCmdList, vertices.size());
        return;
    }

    if (!fontUploaded && uploadBuffer && fontTexture) {
        DX12_DEBUG_STEP("Render", "Font upload: Copying texture to default heap");
        
        D3D12_TEXTURE_COPY_LOCATION srcLoc = {};
        srcLoc.pResource = uploadBuffer.Get();
        srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        srcLoc.PlacedFootprint = fontTextureFootprint;

        D3D12_TEXTURE_COPY_LOCATION dstLoc = {};
        dstLoc.pResource = fontTexture.Get();
        dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dstLoc.SubresourceIndex = 0;

        DX12_DEBUG_STEP("Render", "Font upload: Calling CopyTextureRegion");
        currentCmdList->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);
        DX12_DEBUG_STEP("Render", "Font upload: CopyTextureRegion complete");

        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = fontTexture.Get();
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        
        DX12_DEBUG_STEP("Render", "Font upload: Transitioning COPY_DEST -> PIXEL_SHADER_RESOURCE");
        currentCmdList->ResourceBarrier(1, &barrier);
        DX12_DEBUG_STEP("Render", "Font upload: Barrier submitted");

        fontUploaded = true;
        DX12_DEBUG_STEP("Render", "Font upload: COMPLETE - fontUploaded=true");
    }

    size_t vbSize = vertices.size() * sizeof(DrawVertex);
    if (vbSize > vertexBufferSize) {
        DX12_DEBUG_STEP("Render", "Vertex buffer resize needed: %zu > %zu", vbSize, vertexBufferSize);
        if (!ResizeVertexBuffer(vbSize)) {
            HookLog("DX12 Overlay: Render - Failed to resize vertex buffer (needed "
                    "%zu bytes)",
                  vbSize);
            return;
        }
    }
    if (vertexBufferPtr) {
        memcpy(vertexBufferPtr, vertices.data(), vbSize);
        DX12_DEBUG_FRAME(s_RenderCounter, "Vertex data copied: %zu bytes", vbSize);
    }

    size_t ibSize = indices.size() * sizeof(uint16_t);
    if (ibSize > indexBufferSize) {
        DX12_DEBUG_STEP("Render", "Index buffer resize needed: %zu > %zu", ibSize, indexBufferSize);
        if (!ResizeIndexBuffer(ibSize)) {
            HookLog("DX12 Overlay: Render - Failed to resize index buffer (needed "
                    "%zu bytes)",
                  ibSize);
            return;
        }
    }
    if (indexBufferPtr) {
        memcpy(indexBufferPtr, indices.data(), ibSize);
        DX12_DEBUG_FRAME(s_RenderCounter, "Index data copied: %zu bytes", ibSize);
    }

    DX12_DEBUG_FRAME(s_RenderCounter, "Setting pipeline state");
    currentCmdList->SetGraphicsRootSignature(rootSignature.Get());

    ID3D12DescriptorHeap *heaps[] = {srvHeap.Get()};
    currentCmdList->SetDescriptorHeaps(1, heaps);

    float constants[4] = {(float)viewportWidth, (float)viewportHeight,
                          (float)hdrMode, paperWhiteNits};
    currentCmdList->SetGraphicsRoot32BitConstants(0, 4, constants, 0);

    currentCmdList->SetGraphicsRootDescriptorTable(
        1, srvHeap->GetGPUDescriptorHandleForHeapStart());

    currentCmdList->OMSetRenderTargets(1, &currentRTV, FALSE, nullptr);

    D3D12_VIEWPORT vp = {0, 0, (float)viewportWidth, (float)viewportHeight, 0, 1};
    D3D12_RECT scissor = {0, 0, (LONG)viewportWidth, (LONG)viewportHeight};
    currentCmdList->RSSetViewports(1, &vp);
    currentCmdList->RSSetScissorRects(1, &scissor);

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

    ID3D12PipelineState *lastPSO = nullptr;
    int drawCallCount = 0;
    for (const auto &cmd : commands) {
        ID3D12PipelineState *pso =
            cmd.useTexture ? pipelineState.Get() : pipelineStateSolid.Get();
        if (pso != lastPSO) {
            currentCmdList->SetPipelineState(pso);
            lastPSO = pso;
        }
        currentCmdList->DrawIndexedInstanced(cmd.indexCount, 1, cmd.indexOffset, 0,
                                             0);
        drawCallCount++;
    }
    DX12_DEBUG_FRAME(s_RenderCounter, "Render complete: %d draw calls", drawCallCount);
}

} // namespace CustomOverlay
