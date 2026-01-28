#include "cached_overlay_renderer.h"
#include "backends/imgui_impl_dx12.h"
#include <cstring>

namespace overlay {

// Static helpers for D3D12 resource creation
static bool CreateUploadBuffer(ID3D12Device* device, 
                                size_t size, 
                                ID3D12Resource** outResource,
                                void** outCpuPtr) {
    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;
    heapProps.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    heapProps.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    
    D3D12_RESOURCE_DESC resourceDesc = {};
    resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    resourceDesc.Width = size;
    resourceDesc.Height = 1;
    resourceDesc.DepthOrArraySize = 1;
    resourceDesc.MipLevels = 1;
    resourceDesc.Format = DXGI_FORMAT_UNKNOWN;
    resourceDesc.SampleDesc.Count = 1;
    resourceDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    resourceDesc.Flags = D3D12_RESOURCE_FLAG_NONE;
    
    HRESULT hr = device->CreateCommittedResource(
        &heapProps,
        D3D12_HEAP_FLAG_NONE,
        &resourceDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(outResource));
    
    if (FAILED(hr)) {
        return false;
    }
    
    D3D12_RANGE readRange(0, 0);
    hr = (*outResource)->Map(0, &readRange, outCpuPtr);
    if (FAILED(hr)) {
        (*outResource)->Release();
        *outResource = nullptr;
        return false;
    }
    
    // Zero initialize
    memset(*outCpuPtr, 0, size);
    
    return true;
}

// FrameTimeGraphData implementation
void FrameTimeGraphData::InitGpuBuffer(ID3D12Device* device) {
    if (gpuHistoryBuffer) return;
    
    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;
    
    D3D12_RESOURCE_DESC resourceDesc = {};
    resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    resourceDesc.Width = HISTORY_SIZE * sizeof(float);
    resourceDesc.Height = 1;
    resourceDesc.DepthOrArraySize = 1;
    resourceDesc.MipLevels = 1;
    resourceDesc.Format = DXGI_FORMAT_UNKNOWN;
    resourceDesc.SampleDesc.Count = 1;
    resourceDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    
    device->CreateCommittedResource(
        &heapProps,
        D3D12_HEAP_FLAG_NONE,
        &resourceDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(&gpuHistoryBuffer));
    
    if (gpuHistoryBuffer) {
        D3D12_RANGE readRange(0, 0);
        gpuHistoryBuffer->Map(0, &readRange, (void**)&gpuHistoryCpuPtr);
    }
}

// CachedOverlayRenderer implementation
CachedOverlayRenderer::CachedOverlayRenderer() = default;

CachedOverlayRenderer::~CachedOverlayRenderer() {
    Shutdown();
}

bool CachedOverlayRenderer::Initialize(ID3D12Device* device, 
                                        ID3D12CommandQueue* queue, 
                                        uint32_t bufferCount) {
    if (!device || !queue || bufferCount == 0) {
        return false;
    }
    
    this->device = device;
    this->commandQueue = queue;
    device->AddRef();
    queue->AddRef();
    
    // Create fence for synchronization
    HRESULT hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    if (FAILED(hr)) {
        return false;
    }
    
    fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (!fenceEvent) {
        return false;
    }
    
    // Create per-frame resources
    if (!CreateFrameResources(bufferCount)) {
        return false;
    }
    
    // Initialize frame time graph GPU buffer
    frameTimeGraph.InitGpuBuffer(device);
    
    // Create SRV heap for ImGui
    D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc = {};
    srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srvHeapDesc.NumDescriptors = 64;
    srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    
    hr = device->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&srvHeap));
    if (FAILED(hr)) {
        return false;
    }
    
    srvDescriptorSize = device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    
    numBackBuffers = bufferCount;
    
    return true;
}

void CachedOverlayRenderer::Shutdown() {
    // Wait for GPU
    if (fence && fenceEvent) {
        fence->SetEventOnCompletion(nextFenceValue - 1, fenceEvent);
        WaitForSingleObject(fenceEvent, 100);
    }
    
    DestroyFrameResources();
    DestroyGpuBuffers();
    
    if (srvHeap) {
        srvHeap->Release();
        srvHeap = nullptr;
    }
    
    if (fenceEvent) {
        CloseHandle(fenceEvent);
        fenceEvent = nullptr;
    }
    
    if (fence) {
        fence->Release();
        fence = nullptr;
    }
    
    if (commandQueue) {
        commandQueue->Release();
        commandQueue = nullptr;
    }
    
    if (device) {
        device->Release();
        device = nullptr;
    }
    
    numBackBuffers = 0;
}

bool CachedOverlayRenderer::CreateFrameResources(uint32_t bufferCount) {
    perFrameResources.resize(bufferCount);
    
    for (uint32_t i = 0; i < bufferCount; i++) {
        CachedFrameResources& frame = perFrameResources[i];
        
        // Create command allocator
        HRESULT hr = device->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT,
            IID_PPV_ARGS(&frame.commandAllocator));
        
        if (FAILED(hr)) {
            return false;
        }
        
        // Create command list
        hr = device->CreateCommandList(
            0,
            D3D12_COMMAND_LIST_TYPE_DIRECT,
            frame.commandAllocator,
            nullptr,
            IID_PPV_ARGS(&frame.commandList));
        
        if (FAILED(hr)) {
            return false;
        }
        
        // Initial close state
        frame.commandList->Close();
        frame.isRecording = false;
        
        // Create vertex buffer
        if (!CreateUploadBuffer(device, 
                                CachedFrameResources::VB_SIZE,
                                &frame.vertexBuffer,
                                &frame.vertexBufferCpuPtr)) {
            return false;
        }
        
        // Create index buffer
        if (!CreateUploadBuffer(device,
                                CachedFrameResources::IB_SIZE,
                                &frame.indexBuffer,
                                &frame.indexBufferCpuPtr)) {
            return false;
        }
        
        frame.isValid = false;
    }
    
    return true;
}

void CachedOverlayRenderer::DestroyFrameResources() {
    for (auto& frame : perFrameResources) {
        if (frame.indexBuffer) {
            frame.indexBuffer->Unmap(0, nullptr);
            frame.indexBuffer->Release();
            frame.indexBuffer = nullptr;
        }
        
        if (frame.vertexBuffer) {
            frame.vertexBuffer->Unmap(0, nullptr);
            frame.vertexBuffer->Release();
            frame.vertexBuffer = nullptr;
        }
        
        if (frame.commandList) {
            frame.commandList->Release();
            frame.commandList = nullptr;
        }
        
        if (frame.commandAllocator) {
            frame.commandAllocator->Release();
            frame.commandAllocator = nullptr;
        }
    }
    
    perFrameResources.clear();
}

void CachedOverlayRenderer::DestroyGpuBuffers() {
    if (frameTimeGraph.gpuHistoryBuffer) {
        frameTimeGraph.gpuHistoryBuffer->Unmap(0, nullptr);
        frameTimeGraph.gpuHistoryBuffer->Release();
        frameTimeGraph.gpuHistoryBuffer = nullptr;
        frameTimeGraph.gpuHistoryCpuPtr = nullptr;
    }
}

bool CachedOverlayRenderer::IsFrameAvailable(uint32_t frameIndex) {
    if (frameIndex >= perFrameResources.size()) {
        return false;
    }
    
    CachedFrameResources& frame = perFrameResources[frameIndex];
    if (frame.fenceValue == 0) {
        return true;  // Never used
    }
    
    uint64_t completedValue = fence->GetCompletedValue();
    return completedValue >= frame.fenceValue;
}

bool CachedOverlayRenderer::WaitForFrameResources(uint32_t frameIndex, 
                                                   uint32_t timeoutMs) {
    if (frameIndex >= perFrameResources.size()) {
        return false;
    }
    
    CachedFrameResources& frame = perFrameResources[frameIndex];
    if (frame.fenceValue == 0) {
        return true;
    }
    
    uint64_t completedValue = fence->GetCompletedValue();
    if (completedValue >= frame.fenceValue) {
        return true;
    }
    
    if (timeoutMs == 0) {
        return false;  // Don't wait
    }
    
    fence->SetEventOnCompletion(frame.fenceValue, fenceEvent);
    DWORD waitResult = WaitForSingleObject(fenceEvent, timeoutMs);
    
    return waitResult == WAIT_OBJECT_0;
}

bool CachedOverlayRenderer::ShouldUpdateContent() const {
    return contentDirty || (timeSinceLastUpdate >= updateIntervalMs);
}

void CachedOverlayRenderer::UpdateContent(PerformanceMetrics* metrics,
                                           float displayWidth,
                                           float displayHeight,
                                           float deltaTime) {
    timeSinceLastUpdate += deltaTime;
    
    if (!ShouldUpdateContent()) {
        return;
    }
    
    // Reset timer
    timeSinceLastUpdate = 0.0f;
    contentDirty = false;
    
    // Update frame time graph with new sample
    if (metrics) {
        float frameTime = 1000.0f / metrics->GetCurrentFPS();
        frameTimeGraph.AddSample(frameTime);
    }
    
    // Note: Actual ImGui UI construction happens outside this class
    // This is just the caching infrastructure
}

void CachedOverlayRenderer::RebuildCommandList(CachedFrameResources& frame,
                                                const ImDrawData* drawData) {
    if (!drawData || drawData->CmdListsCount == 0) {
        frame.isValid = false;
        return;
    }
    
    // Wait for GPU to finish with this frame's resources
    if (!WaitForFrameResources(&frame - &perFrameResources[0], 100)) {
        // GPU taking too long, skip this update
        return;
    }
    
    // Reset command allocator
    frame.commandAllocator->Reset();
    
    // Reset command list
    frame.commandList->Reset(frame.commandAllocator, nullptr);
    frame.isRecording = true;
    
    // Copy vertex/index data to upload buffers
    uint8_t* vbCpu = (uint8_t*)frame.vertexBufferCpuPtr;
    uint8_t* ibCpu = (uint8_t*)frame.indexBufferCpuPtr;
    
    size_t vbOffset = 0;
    size_t ibOffset = 0;
    
    for (int cmdListIdx = 0; cmdListIdx < drawData->CmdListsCount; cmdListIdx++) {
        const ImDrawList* cmdList = drawData->CmdLists[cmdListIdx];
        
        size_t vbSize = cmdList->VtxBuffer.Size * sizeof(ImDrawVert);
        size_t ibSize = cmdList->IdxBuffer.Size * sizeof(ImDrawIdx);
        
        // Bounds check
        if (vbOffset + vbSize > CachedFrameResources::VB_SIZE ||
            ibOffset + ibSize > CachedFrameResources::IB_SIZE) {
            break;  // Buffer full
        }
        
        // Copy data
        memcpy(vbCpu + vbOffset, cmdList->VtxBuffer.Data, vbSize);
        memcpy(ibCpu + ibOffset, cmdList->IdxBuffer.Data, ibSize);
        
        vbOffset += vbSize;
        ibOffset += ibSize;
    }
    
    // Set up render state
    frame.commandList->SetDescriptorHeaps(1, &srvHeap);
    
    // Record ImGui draw commands
    // This is a simplified version - full implementation would use ImGui's renderer
    ImGui_ImplDX12_RenderDrawData(const_cast<ImDrawData*>(drawData), frame.commandList);
    
    // Close command list
    frame.commandList->Close();
    frame.isRecording = false;
    frame.isValid = true;
}

void CachedOverlayRenderer::SubmitCachedCommandList(CachedFrameResources& frame,
                                                      ID3D12GraphicsCommandList* targetList) {
    if (!frame.isValid || !frame.commandList || !targetList) {
        return;
    }
    
    // CRITICAL FIX: Bundles cannot change render targets, so we can't use ExecuteBundle
    // for overlay rendering. Instead, we record the ImGui commands directly to the
    // target command list. The cached renderer approach needs to be reworked for
    // Frame Generation support without bundle limitations.
    
    // For now, just execute the cached command list directly
    // The command list was recorded with all necessary state (including render target)
    // in RebuildCommandList, so we just need to execute it.
    
    // Note: In D3D12, we can't execute a command list that was recorded for a different
    // render target. The proper fix is to either:
    // 1. Use secondary command lists with inheritance
    // 2. Record overlay commands per-frame (current approach in standard renderer)
    // 3. Use bundles only for state-independent commands
    
    // For now, this is a no-op - the actual rendering happens in DrawOverlay
    // which calls ImGui_ImplDX12_RenderDrawData directly.
    
    // Update fence value
    frame.fenceValue = nextFenceValue++;
}

void CachedOverlayRenderer::Render(ID3D12GraphicsCommandList* targetCommandList,
                                    uint32_t backBufferIndex,
                                    bool isRealFrame,
                                    const ImVec2& displaySize) {
    if (backBufferIndex >= perFrameResources.size()) {
        return;
    }
    
    CachedFrameResources& frame = perFrameResources[backBufferIndex];
    
    // Rebuild command list if content needs update
    // This happens on real frames, OR on any frame if we don't have valid cached content yet
    bool needsRebuild = ShouldUpdateContent();
    if (needsRebuild) {
        ImDrawData* drawData = ImGui::GetDrawData();
        if (drawData) {
            RebuildCommandList(frame, drawData);
        }
    }
    
    // On all frames: Submit cached command list (fast path for interpolated)
    if (frame.isValid) {
        SubmitCachedCommandList(frame, targetCommandList);
    }
    
    // Signal fence for this frame
    commandQueue->Signal(fence, frame.fenceValue);
}

// ImGuiRenderCache implementation
void ImGuiRenderCache::CacheDrawData(const ImDrawData* drawData) {
    if (!drawData) return;
    
    cachedData.displayPos = drawData->DisplayPos;
    cachedData.displaySize = drawData->DisplaySize;
    cachedData.frameCount++;
    
    // Clear previous data
    cachedData.vertices.clear();
    cachedData.indices.clear();
    cachedData.commands.clear();
    
    // Copy all draw list data
    for (int i = 0; i < drawData->CmdListsCount; i++) {
        const ImDrawList* cmdList = drawData->CmdLists[i];
        
        size_t vbStart = cachedData.vertices.size();
        
        // Copy vertices
        cachedData.vertices.insert(cachedData.vertices.end(),
                                   cmdList->VtxBuffer.Data,
                                   cmdList->VtxBuffer.Data + cmdList->VtxBuffer.Size);
        
        // Copy indices (with offset adjustment)
        for (int j = 0; j < cmdList->IdxBuffer.Size; j++) {
            cachedData.indices.push_back(cmdList->IdxBuffer.Data[j] + (ImDrawIdx)vbStart);
        }
        
        // Copy commands
        for (const auto& cmd : cmdList->CmdBuffer) {
            cachedData.commands.push_back(cmd);
        }
    }
}

} // namespace overlay
