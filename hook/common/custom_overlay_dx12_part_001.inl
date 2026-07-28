/**
 * Custom Overlay - DX12 Backend Implementation
 */

#include "custom_overlay_dx12.h"
#include <algorithm>
#include <cstring>
#include "../apis/dx12_hook.h"
#include "dx12_overlay_policy.h"
#include "hook_common.h"
#include "overlay_shader_bytecode.h"

namespace CustomOverlay {

static uint64_t s_FrameCounter = 0;
static uint64_t s_RenderCounter = 0;
static std::atomic<int> s_dx12RenderProbeMode{static_cast<int>(DX12RenderProbeMode::kNone)};

void SetDX12RenderProbeMode(DX12RenderProbeMode mode) {
    s_dx12RenderProbeMode.store(static_cast<int>(mode), std::memory_order_release);
}

DX12RenderProbeMode GetDX12RenderProbeMode() {
    return static_cast<DX12RenderProbeMode>(s_dx12RenderProbeMode.load(std::memory_order_acquire));
}

DX12Backend::DX12Backend(ID3D12Device* dev, ID3D12CommandQueue* queue, DXGI_FORMAT format)
    : device(dev),
      commandQueue(queue),
      rtvFormat(format) {
    DX12_DEBUG_STEP("Constructor", "device=%p, queue=%p, format=%d", dev, queue, format);
}

DX12Backend::~DX12Backend() {
    DX12_DEBUG_STEP("Destructor", "Shutting down backend");
    Shutdown();
}

bool DX12Backend::Initialize(int fontTextureWidth, int fontTextureHeight, const uint8_t* fontTextureData) {
    DX12_DEBUG_STEP("Initialize", "START - fontTex=%dx%d, initialized=%d, device=%p", fontTextureWidth,
                    fontTextureHeight, initialized, device);

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
    if (PreferSolidTextGeometry()) {
        fontUploaded.store(true, std::memory_order_release);
        DX12_DEBUG_STEP("Initialize", "Step 4/4: CreateFontTexture - SKIPPED for solid text geometry");
    } else {
        if (!CreateFontTexture(fontTextureWidth, fontTextureHeight, fontTextureData)) {
            DX12_DEBUG_STEP("Initialize", "FAILED - CreateFontTexture");
            HookLog("DX12 Overlay: Initialize - CreateFontTexture failed");
            return false;
        }
        DX12_DEBUG_STEP("Initialize", "Step 4/4: CreateFontTexture - SUCCESS");
    }

    initialized = true;
    if (PreferSolidTextGeometry()) {
        HookLogImportant(
            "DX12 Overlay: x86 solid-span text path enabled (native solid geometry, font SRV upload skipped)");
    }
    DX12_DEBUG_STEP("Initialize", "COMPLETE - initialized=true");
    return true;
}

bool DX12Backend::PreferSolidTextGeometry() const {
    return ce::dx12_overlay_policy::ShouldUseSolidDx12TextGeometryForProcess(sizeof(void*) == 4);
}

void DX12Backend::Shutdown() {
    DX12_DEBUG_STEP("Shutdown", "START - initialized=%d", initialized);

    // During process exit, skip all GPU resource cleanup. Other DLLs (e.g. the
    // NVIDIA driver nvwgf2umx.dll) may already be partially torn down, causing
    // Unmap/Release calls to crash. The OS reclaims all GPU resources on exit.
    if (IsProcessTerminating()) {
        for (int i = 0; i < kFramePoolSize; i++) {
            vertexBufferPtr[i] = nullptr;
            indexBufferPtr[i] = nullptr;
            (void)vertexBuffer[i].Detach();
            (void)indexBuffer[i].Detach();
        }
        (void)rootSignature.Detach();
        (void)pipelineState.Detach();
        (void)pipelineStateTexturedSdr.Detach();
        (void)pipelineStateSolid.Detach();
        (void)srvHeap.Detach();
        (void)fontTexture.Detach();
        (void)uploadBuffer.Detach();
        currentCmdList = nullptr;
        commandQueue = nullptr;
        device = nullptr;
        initialized = false;
        return;
    }

    if (vertexBuffer[0] && vertexBufferPtr[0]) {
        DX12_DEBUG_STEP("Shutdown", "Unmapping vertex buffers");
    }
    if (indexBuffer[0] && indexBufferPtr[0]) {
        DX12_DEBUG_STEP("Shutdown", "Unmapping index buffers");
    }
    for (int i = 0; i < kFramePoolSize; i++) {
        if (vertexBuffer[i] && vertexBufferPtr[i]) {
            vertexBuffer[i]->Unmap(0, nullptr);
            vertexBufferPtr[i] = nullptr;
        }
        if (indexBuffer[i] && indexBufferPtr[i]) {
            indexBuffer[i]->Unmap(0, nullptr);
            indexBufferPtr[i] = nullptr;
        }
        vertexBuffer[i].Reset();
        indexBuffer[i].Reset();
    }

    rootSignature.Reset();
    pipelineState.Reset();
    pipelineStateTexturedSdr.Reset();
    pipelineStateSolid.Reset();
    srvHeap.Reset();
    fontTexture.Reset();
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
    DX12_DEBUG_STEP("CreateRootSignature", "SRV range configured: reg=%d, num=%d", srvRange.BaseShaderRegister,
                    srvRange.NumDescriptors);

    D3D12_ROOT_PARAMETER params[2] = {};

    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.ShaderRegister = 0;
    params[0].Constants.RegisterSpace = 0;
    params[0].Constants.Num32BitValues = 4;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    DX12_DEBUG_STEP("CreateRootSignature", "Param 0: 32bit constants, reg=%d, numValues=%d",
                    params[0].Constants.ShaderRegister, params[0].Constants.Num32BitValues);

    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 1;
    params[1].DescriptorTable.pDescriptorRanges = &srvRange;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    DX12_DEBUG_STEP("CreateRootSignature", "Param 1: Descriptor table, ranges=%d",
                    params[1].DescriptorTable.NumDescriptorRanges);

    D3D12_STATIC_SAMPLER_DESC sampler = {};
    sampler.Filter = D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT;
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
    HRESULT hr = D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error);
    DX12_DEBUG_STEP("CreateRootSignature", "Serialization result: hr=0x%08X (%s)", hr, SUCCEEDED(hr) ? "OK" : "FAILED");
    if (FAILED(hr)) {
        if (error) {
            DX12_DEBUG_STEP("CreateRootSignature", "Serialization error message: %s", (char*)error->GetBufferPointer());
            HookLog(
                "DX12 Overlay: CreateRootSignature - D3D12SerializeRootSignature "
                "failed, hr=0x%08X, error=%s",
                hr, (char*)error->GetBufferPointer());
        } else {
            HookLog(
                "DX12 Overlay: CreateRootSignature - D3D12SerializeRootSignature "
                "failed, hr=0x%08X",
                hr);
        }
        return false;
    }
    DX12_DEBUG_STEP("CreateRootSignature", "Blob size=%zu", blob->GetBufferSize());

    DX12_DEBUG_STEP("CreateRootSignature", "Calling device->CreateRootSignature");
    hr = device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&rootSignature));
    DX12_DEBUG_STEP("CreateRootSignature", "CreateRootSignature result: hr=0x%08X (%s), rootSig=%p", hr,
                    SUCCEEDED(hr) ? "OK" : "FAILED", rootSignature.Get());
    if (FAILED(hr)) {
        HookLog(
            "DX12 Overlay: CreateRootSignature - CreateRootSignature failed, "
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
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 16, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    };
    DX12_DEBUG_STEP("CreatePipelineState", "Input layout: %zu elements", sizeof(inputLayout) / sizeof(inputLayout[0]));

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.pRootSignature = rootSignature.Get();
    psoDesc.VS = {g_VS_5_0, sizeof(g_VS_5_0)};
    psoDesc.PS = {g_PS_Textured_5_0, sizeof(g_PS_Textured_5_0)};
    DX12_DEBUG_STEP("CreatePipelineState", "VS size=%zu, PS_Textured size=%zu", sizeof(g_VS_5_0),
                    sizeof(g_PS_Textured_5_0));
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
    psoDesc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    DX12_DEBUG_STEP("CreatePipelineState", "Blend state configured (alpha blending)");

    DX12_DEBUG_STEP("CreatePipelineState", "Creating TEXTURED PSO");
    HRESULT hr = device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&pipelineState));
    DX12_DEBUG_STEP("CreatePipelineState", "Textured PSO result: hr=0x%08X (%s), pso=%p", hr,
                    SUCCEEDED(hr) ? "OK" : "FAILED", pipelineState.Get());
    if (FAILED(hr)) {
        HookLog("DX12 Overlay: CreatePipelineState failed, hr=0x%08X", hr);
        return false;
    }

    DX12_DEBUG_STEP("CreatePipelineState", "Creating TEXTURED SDR PSO");
    psoDesc.PS = {g_PS_Textured_SDR_5_0, sizeof(g_PS_Textured_SDR_5_0)};
    DX12_DEBUG_STEP("CreatePipelineState", "PS_Textured_SDR size=%zu", sizeof(g_PS_Textured_SDR_5_0));
    hr = device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&pipelineStateTexturedSdr));
    DX12_DEBUG_STEP("CreatePipelineState", "Textured SDR PSO result: hr=0x%08X (%s), pso=%p", hr,
                    SUCCEEDED(hr) ? "OK" : "FAILED", pipelineStateTexturedSdr.Get());
    if (FAILED(hr)) {
        HookLog("DX12 Overlay: CreatePipelineState (textured SDR) failed, hr=0x%08X", hr);
        return false;
    }

    DX12_DEBUG_STEP("CreatePipelineState", "Creating SOLID PSO");
    psoDesc.PS = {g_PS_Solid_5_0, sizeof(g_PS_Solid_5_0)};
    DX12_DEBUG_STEP("CreatePipelineState", "PS_Solid size=%zu", sizeof(g_PS_Solid_5_0));
    hr = device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&pipelineStateSolid));
    DX12_DEBUG_STEP("CreatePipelineState", "Solid PSO result: hr=0x%08X (%s), pso=%p", hr,
                    SUCCEEDED(hr) ? "OK" : "FAILED", pipelineStateSolid.Get());
    if (FAILED(hr)) {
        HookLog("DX12 Overlay: CreatePipelineState (solid) failed, hr=0x%08X", hr);
        return false;
    }
    DX12_DEBUG_STEP("CreatePipelineState", "SUCCESS - textured=%p texturedSdr=%p solid=%p", pipelineState.Get(),
                    pipelineStateTexturedSdr.Get(), pipelineStateSolid.Get());
    return true;
}

bool DX12Backend::CreateBuffers() {
    DX12_DEBUG_STEP("CreateBuffers", "START - creating %d buffer pool slots", kFramePoolSize);

    const size_t initVBSize = 4096 * sizeof(DrawVertex);
    const size_t initIBSize = 8192 * sizeof(uint16_t);
    DX12_DEBUG_STEP("CreateBuffers", "Per-slot sizes: vertex=%zu bytes, index=%zu bytes", initVBSize, initIBSize);

    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC bufferDesc = {};
    bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufferDesc.Height = 1;
    bufferDesc.DepthOrArraySize = 1;
    bufferDesc.MipLevels = 1;
    bufferDesc.SampleDesc.Count = 1;
    bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    D3D12_RANGE readRange = {0, 0};
    for (int i = 0; i < kFramePoolSize; i++) {
        vertexBufferSize[i] = initVBSize;
        bufferDesc.Width = initVBSize;
        HRESULT hr =
            device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &bufferDesc,
                                            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&vertexBuffer[i]));
        if (FAILED(hr)) {
            HookLog(
                "DX12 Overlay: CreateBuffers - Vertex buffer[%d] creation failed, "
                "hr=0x%08X",
                i, hr);
            return false;
        }
        vertexBuffer[i]->Map(0, &readRange, &vertexBufferPtr[i]);

        indexBufferSize[i] = initIBSize;
        bufferDesc.Width = initIBSize;
        hr = device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &bufferDesc,
                                             D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&indexBuffer[i]));
        if (FAILED(hr)) {
            HookLog(
                "DX12 Overlay: CreateBuffers - Index buffer[%d] creation failed, "
                "hr=0x%08X",
                i, hr);
            return false;
        }
        indexBuffer[i]->Map(0, &readRange, &indexBufferPtr[i]);
    }

    DX12_DEBUG_STEP("CreateBuffers", "SUCCESS - %d VB/IB pairs created and mapped", kFramePoolSize);
    return true;
}

bool DX12Backend::CreateFontTexture(int width, int height, const uint8_t* data) {
    DX12_DEBUG_STEP("CreateFontTexture", "START - size=%dx%d, data=%p", width, height, data);

    DX12_DEBUG_STEP("CreateFontTexture", "Step 1: Creating SRV descriptor heap");
    D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.NumDescriptors = 1;
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

    HRESULT hr = device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&srvHeap));
    DX12_DEBUG_STEP("CreateFontTexture", "SRV heap result: hr=0x%08X (%s), heap=%p", hr,
                    SUCCEEDED(hr) ? "OK" : "FAILED", srvHeap.Get());
    if (FAILED(hr)) {
        HookLog("DX12 Overlay: CreateFontTexture - SRV heap creation failed, hr=0x%08X", hr);
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
    DX12_DEBUG_STEP("CreateFontTexture", "Texture desc: %llux%u, format=%d, mipLevels=%d", texDesc.Width,
                    texDesc.Height, texDesc.Format, texDesc.MipLevels);

    hr = device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &texDesc, D3D12_RESOURCE_STATE_COPY_DEST,
                                         nullptr, IID_PPV_ARGS(&fontTexture));
    DX12_DEBUG_STEP("CreateFontTexture", "Texture resource result: hr=0x%08X (%s), texture=%p", hr,
                    SUCCEEDED(hr) ? "OK" : "FAILED", fontTexture.Get());
    if (FAILED(hr)) {
        HookLog(
            "DX12 Overlay: CreateFontTexture - Font texture creation failed, "
            "hr=0x%08X",
            hr);
        return false;
    }

    DX12_DEBUG_STEP("CreateFontTexture", "Step 3: Creating upload buffer");
    UINT64 uploadSize = 0;
    device->GetCopyableFootprints(&texDesc, 0, 1, 0, nullptr, nullptr, nullptr, &uploadSize);
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

    hr = device->CreateCommittedResource(&uploadHeapProps, D3D12_HEAP_FLAG_NONE, &uploadDesc,
                                         D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&uploadBuffer));
    DX12_DEBUG_STEP("CreateFontTexture", "Upload buffer result: hr=0x%08X (%s), buffer=%p", hr,
                    SUCCEEDED(hr) ? "OK" : "FAILED", uploadBuffer.Get());
    if (FAILED(hr)) {
        HookLog(
            "DX12 Overlay: CreateFontTexture - Upload buffer creation failed, "
            "hr=0x%08X",
            hr);
        return false;
    }

    DX12_DEBUG_STEP("CreateFontTexture", "Step 4: Copying font data to upload buffer");
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
    device->GetCopyableFootprints(&texDesc, 0, 1, 0, &footprint, nullptr, nullptr, nullptr);
    DX12_DEBUG_STEP("CreateFontTexture", "Footprint: rowPitch=%u, width=%u, height=%u", footprint.Footprint.RowPitch,
                    footprint.Footprint.Width, footprint.Footprint.Height);

    void* uploadPtr;
    uploadBuffer->Map(0, nullptr, &uploadPtr);
    DX12_DEBUG_STEP("CreateFontTexture", "Upload buffer mapped: ptr=%p", uploadPtr);

    uint8_t* dst = (uint8_t*)uploadPtr;
    for (int y = 0; y < height; y++) {
        memcpy(dst + y * footprint.Footprint.RowPitch, data + y * width * 4, width * 4);
    }
    uploadBuffer->Unmap(0, nullptr);
    DX12_DEBUG_STEP("CreateFontTexture", "Font data copied (%d rows), upload buffer unmapped", height);

    DX12_DEBUG_STEP("CreateFontTexture", "Step 5: Creating SRV");
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MipLevels = 1;

    device->CreateShaderResourceView(fontTexture.Get(), &srvDesc, srvHeap->GetCPUDescriptorHandleForHeapStart());
    DX12_DEBUG_STEP("CreateFontTexture", "SRV created on heap");

    fontTextureFootprint = footprint;
    fontTextureDesc = texDesc;
    fontUploaded.store(false, std::memory_order_relaxed);
    DX12_DEBUG_STEP("CreateFontTexture", "SUCCESS - fontUploaded=false (deferred upload)");

    return true;
}

void DX12Backend::SetRenderTarget(ID3D12GraphicsCommandList* cmdList, D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle) {
    s_FrameCounter++;
    DX12_DEBUG_FRAME(s_FrameCounter, "SetRenderTarget: cmdList=%p, rtvHandle.ptr=%llx", cmdList, rtvHandle.ptr);

    currentCmdList = cmdList;
    currentRTV = rtvHandle;
}

void DX12Backend::SetUploadSlotFence(ID3D12Fence* fence, uint64_t guardValue) {
    if (slotFence != fence) {
        for (int i = 0; i < kFramePoolSize; ++i) {
            slotFenceValue[i] = 0;
        }
        slotFence = fence;
    }
    nextSlotFenceValue = guardValue;
}

void DX12Backend::SetNextUploadSlot(int slot) {
    nextForcedUploadSlot.store(slot >= 0 && slot < kFramePoolSize ? slot : -1, std::memory_order_release);
}

bool DX12Backend::PrimeResources(ID3D12GraphicsCommandList* cmdList) {
    DX12_DEBUG_STEP("PrimeResources", "START - initialized=%d, cmdList=%p, pending=%d", initialized ? 1 : 0, cmdList,
                    HasPendingResources() ? 1 : 0);

    if (!initialized || !cmdList) {
        DX12_DEBUG_STEP("PrimeResources", "FAILED - initialized=%d, cmdList=%p", initialized ? 1 : 0, cmdList);
        return false;
    }

    return UploadFontTextureIfNeeded(cmdList);
}

bool DX12Backend::UploadFontTextureIfNeeded(ID3D12GraphicsCommandList* cmdList) {
    if (fontUploaded.load(std::memory_order_acquire) || !uploadBuffer || !fontTexture) {
        DX12_DEBUG_STEP("UploadFontTextureIfNeeded", "No deferred upload needed");
        return true;
    }
    if (!cmdList) {
        DX12_DEBUG_STEP("UploadFontTextureIfNeeded", "FAILED - no command list");
        return false;
    }

    DX12_DEBUG_STEP("UploadFontTextureIfNeeded", "Font upload: Copying texture to default heap");

    D3D12_TEXTURE_COPY_LOCATION srcLoc = {};
    srcLoc.pResource = uploadBuffer.Get();
    srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    srcLoc.PlacedFootprint = fontTextureFootprint;

    D3D12_TEXTURE_COPY_LOCATION dstLoc = {};
    dstLoc.pResource = fontTexture.Get();
    dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dstLoc.SubresourceIndex = 0;

    DX12_DEBUG_STEP("UploadFontTextureIfNeeded", "Font upload: Calling CopyTextureRegion");
    cmdList->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);
    DX12_DEBUG_STEP("UploadFontTextureIfNeeded", "Font upload: CopyTextureRegion complete");

    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = fontTexture.Get();
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

    DX12_DEBUG_STEP("UploadFontTextureIfNeeded", "Font upload: Transitioning COPY_DEST -> PIXEL_SHADER_RESOURCE");
    cmdList->ResourceBarrier(1, &barrier);
    DX12_DEBUG_STEP("UploadFontTextureIfNeeded", "Font upload: Barrier submitted");

    fontUploaded.store(true, std::memory_order_release);
    DX12_DEBUG_STEP("UploadFontTextureIfNeeded", "Font upload: COMPLETE - fontUploaded=true");
    return true;
}

bool DX12Backend::WaitForSlotGpuComplete(int slot) {
    if (!slotFence || slot < 0 || slot >= kFramePoolSize) {
        return true;
    }

    const uint64_t guardValue = slotFenceValue[slot];
    const uint64_t completedBefore = slotFence->GetCompletedValue();
    if (!ce::dx12_overlay_policy::ShouldWaitForOverlayUploadSlot(guardValue, completedBefore)) {
        return true;
    }

    HANDLE eventHandle = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!eventHandle) {
        return false;
    }

    bool completed = false;
    if (SUCCEEDED(slotFence->SetEventOnCompletion(guardValue, eventHandle))) {
        constexpr DWORD kSlotWaitTimeoutMs = 1000;
        const DWORD waitResult = WaitForSingleObject(eventHandle, kSlotWaitTimeoutMs);
        completed = waitResult == WAIT_OBJECT_0;
        if (completed) {
            static std::atomic<int> s_slotWaitCompleteLog{0};
            const int logN = s_slotWaitCompleteLog.fetch_add(1, std::memory_order_relaxed);
            if (logN < 20 || (logN % 200) == 0) {
                HookLogImportant(
                    "DX12 Overlay: slot %d GPU-completion wait completed (guard=%llu completedBefore=%llu "
                    "completedAfter=%llu)",
                    slot, (unsigned long long)guardValue, (unsigned long long)completedBefore,
                    (unsigned long long)slotFence->GetCompletedValue());
            }
        } else {
            static std::atomic<int> s_slotWaitTimeoutLog{0};
            const int logN = s_slotWaitTimeoutLog.fetch_add(1, std::memory_order_relaxed);
            if (logN < 40 || (logN % 200) == 0) {
                HookLogImportant(
                    "DX12 Overlay: slot %d GPU-completion wait %s (guard=%llu completed=%llu) — upload ring may be "
                    "draw skipped to avoid reusing in-flight GPU data",
                    slot, waitResult == WAIT_TIMEOUT ? "timed out" : "failed", (unsigned long long)guardValue,
                    (unsigned long long)slotFence->GetCompletedValue());
            }
        }
    }

    CloseHandle(eventHandle);
    return completed;
}

bool DX12Backend::ResizeVertexBuffer(int slot, size_t requiredBytes) {
    DX12_DEBUG_STEP("ResizeVertexBuffer", "START - slot=%d, required=%zu, current=%zu", slot, requiredBytes,
                    vertexBufferSize[slot]);

    if (!device) {
        DX12_DEBUG_STEP("ResizeVertexBuffer", "FAILED - no device");
        return false;
    }

    if (vertexBuffer[slot] && vertexBufferPtr[slot]) {
        DX12_DEBUG_STEP("ResizeVertexBuffer", "Unmapping old vertex buffer[%d]", slot);
        vertexBuffer[slot]->Unmap(0, nullptr);
        vertexBufferPtr[slot] = nullptr;
    }

    size_t newSize = vertexBufferSize[slot] * 2;
    while (newSize < requiredBytes) {
        newSize *= 2;
    }
    DX12_DEBUG_STEP("ResizeVertexBuffer", "New size: %zu bytes (old=%zu, slot=%d)", newSize, vertexBufferSize[slot],
                    slot);

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
    HRESULT hr = device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &bufferDesc,
                                                 D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&newBuffer));
    DX12_DEBUG_STEP("ResizeVertexBuffer", "Create result: hr=0x%08X (%s)", hr, SUCCEEDED(hr) ? "OK" : "FAILED");
    if (FAILED(hr)) {
        HookLog(
            "DX12 Overlay: ResizeVertexBuffer - Failed to create new buffer "
            "(slot=%d, size=%zu), hr=0x%08X",
            slot, newSize, hr);
        return false;
    }

    vertexBuffer[slot] = newBuffer;
    vertexBufferSize[slot] = newSize;

    D3D12_RANGE readRange = {0, 0};
    vertexBuffer[slot]->Map(0, &readRange, &vertexBufferPtr[slot]);
    DX12_DEBUG_STEP("ResizeVertexBuffer", "SUCCESS - new buffer[%d] mapped at %p", slot, vertexBufferPtr[slot]);
