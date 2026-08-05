#include "dx12_hook_internal.h"

DX12DescFreeBackend::~DX12DescFreeBackend()  {
    Shutdown();
}

bool DX12DescFreeBackend::InitDevice(ID3D12Device* dev, DXGI_FORMAT rtvFormat) {
    if (deviceReady_)
        return true;
    device_ = dev;
    rtvFormat_ = rtvFormat;
    if (!CreateRootSignature() || !CreatePSOs()) {
        Shutdown();
        return false;
    }
    if (!CreateBuffers()) {
        Shutdown();
        return false;
    }
    deviceReady_ = true;
    return true;
}

bool DX12DescFreeBackend::Initialize(int fontWidth, int fontHeight, const uint8_t* fontData)  {
    if (!device_ || !fontData)
        return false;
    fontWidth_ = fontWidth;
    fontHeight_ = fontHeight;

    const size_t dataSize = (size_t)fontWidth * fontHeight * 4;  // RGBA8
    fontBufferSize_ = dataSize;

    // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization) - zero-initialized placeholder; enum fields are assigned before use
    D3D12_HEAP_PROPERTIES defaultHeap = {};
    defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = dataSize;
    rd.Height = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_UNKNOWN;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    // D3D12 ignores non-COMMON initial states for buffers.  Start in the
    // real state and record explicit transitions around the one-time copy;
    // relying on implicit COMMON promotion before sampling the same command
    // list was fragile on the x86 NVIDIA path.
    HRESULT hr = device_->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &rd,
                                                  D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&fontBuffer_));
    if (FAILED(hr)) {
        HookLogImportant("DescFree: font default buffer create failed hr=0x%08X", hr);
        return false;
    }
    fontBuffer_->SetName(L"CE_DescFreeFontDefaultBuffer");

    // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization) - zero-initialized placeholder; enum fields are assigned before use
    D3D12_HEAP_PROPERTIES uploadHeap = {};
    uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
    hr = device_->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_GENERIC_READ,
                                          nullptr, IID_PPV_ARGS(&fontUploadBuffer_));
    if (FAILED(hr)) {
        HookLogImportant("DescFree: font upload buffer create failed hr=0x%08X", hr);
        fontBuffer_->Release();
        fontBuffer_ = nullptr;
        return false;
    }
    fontUploadBuffer_->SetName(L"CE_DescFreeFontUploadBuffer");

    void* mapped = nullptr;
    D3D12_RANGE readRange = {0, 0};
    hr = fontUploadBuffer_->Map(0, &readRange, &mapped);
    if (FAILED(hr)) {
        HookLogImportant("DescFree: font upload buffer map failed hr=0x%08X", hr);
        fontUploadBuffer_->Release();
        fontUploadBuffer_ = nullptr;
        fontBuffer_->Release();
        fontBuffer_ = nullptr;
        return false;
    }
    memcpy(mapped, fontData, dataSize);
    fontUploadBuffer_->Unmap(0, nullptr);

    fontGpuAddr_ = fontBuffer_->GetGPUVirtualAddress();
    fontUploadPending_ = true;
    HookLogImportant("DescFree: font structured buffer ready (%dx%d, %zu bytes, gpu=0x%llX)", fontWidth, fontHeight,
                     dataSize, (unsigned long long)fontGpuAddr_);
    return true;
}

void DX12DescFreeBackend::Render(const std::vector<CustomOverlay::DrawVertex>& vertices, const std::vector<uint16_t>& indices,
            const std::vector<CustomOverlay::DrawCommand>& commands, int vpW, int vpH)  {
    auto* cmdList = dx12_hook_s_descFreeCmdList;
    if (!cmdList || !deviceReady_ || !fontBuffer_ || vertices.empty())
        return;
    if (fontUploadPending_ && !fontUploadBuffer_)
        return;

    // Rebind the per-slot GPU-completion fence.  If the fence object changed
    // (overlay reinit recreates g_State.fence), the recorded guard values
    // belong to a dead fence — discard them so we never wait on a stale or
    // released fence.
    if (dx12_hook_s_descFreeSlotFence != slotFence_) {
        for (int i = 0; i < kPoolSize; ++i)
            slotFenceValue_[i] = 0;
        slotFence_ = dx12_hook_s_descFreeSlotFence;
    }

    // Upload vertex data
    int slot = frameIdx_ % kPoolSize;
    frameIdx_++;

    // If a caller published a slot guard, block until the GPU has finished
    // the previous frame that used this ring slot before overwriting it.
    if (!WaitForSlotGpuComplete(slot)) {
        return;
    }

    size_t vbBytes = vertices.size() * sizeof(CustomOverlay::DrawVertex);
    if (vbBytes > vbSize_[slot]) {
        if (!ResizeBuffer(vb_[slot], vbPtr_[slot], vbSize_[slot], vbBytes))
            return;
    }
    memcpy(vbPtr_[slot], vertices.data(), vbBytes);

    // Upload index data
    size_t ibBytes = indices.size() * sizeof(uint16_t);
    if (ibBytes > ibSize_[slot]) {
        if (!ResizeBuffer(ib_[slot], ibPtr_[slot], ibSize_[slot], ibBytes))
            return;
    }
    memcpy(ibPtr_[slot], indices.data(), ibBytes);

    // Set pipeline — NO SetDescriptorHeaps!
    cmdList->SetGraphicsRootSignature(rootSig_);

    if (ce::dx12_overlay_policy::ShouldRecordDescFreeFontUpload(fontUploadPending_, fontBuffer_ != nullptr,
                                                                fontUploadBuffer_ != nullptr)) {
        D3D12_RESOURCE_BARRIER fontToCopy = {};
        fontToCopy.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        fontToCopy.Transition.pResource = fontBuffer_;
        fontToCopy.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
        fontToCopy.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        fontToCopy.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cmdList->ResourceBarrier(1, &fontToCopy);

        cmdList->CopyBufferRegion(fontBuffer_, 0, fontUploadBuffer_, 0, fontBufferSize_);

        D3D12_RESOURCE_BARRIER fontBarrier = {};
        fontBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        fontBarrier.Transition.pResource = fontBuffer_;
        fontBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        fontBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        fontBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cmdList->ResourceBarrier(1, &fontBarrier);

        fontUploadPending_ = false;
        HookLogImportant("DescFree: font upload recorded to default buffer (%zu bytes, gpu=0x%llX)",
                         fontBufferSize_, (unsigned long long)fontGpuAddr_);
    }

    // Root constants: viewportW, viewportH, hdrMode, paperWhiteNits, fontW, fontH
    float constants[6] = {(float)vpW,     (float)vpH,        (float)hdrMode,
                          paperWhiteNits, (float)fontWidth_, (float)fontHeight_};
    cmdList->SetGraphicsRoot32BitConstants(0, 6, constants, 0);

    // Root SRV: font buffer (StructuredBuffer<uint> at t0)
    cmdList->SetGraphicsRootShaderResourceView(1, fontGpuAddr_);

    // Render target + viewport
    cmdList->OMSetRenderTargets(1, &dx12_hook_s_descFreeRtv, FALSE, nullptr);
    D3D12_VIEWPORT vp = {0, 0, (float)vpW, (float)vpH, 0, 1};
    D3D12_RECT scissor = {0, 0, (LONG)vpW, (LONG)vpH};
    cmdList->RSSetViewports(1, &vp);
    cmdList->RSSetScissorRects(1, &scissor);

    // Vertex/index buffers
    D3D12_VERTEX_BUFFER_VIEW vbv = {};
    vbv.BufferLocation = vb_[slot]->GetGPUVirtualAddress();
    vbv.SizeInBytes = (UINT)vbBytes;
    vbv.StrideInBytes = sizeof(CustomOverlay::DrawVertex);
    cmdList->IASetVertexBuffers(0, 1, &vbv);

    D3D12_INDEX_BUFFER_VIEW ibv = {};
    ibv.BufferLocation = ib_[slot]->GetGPUVirtualAddress();
    ibv.SizeInBytes = (UINT)ibBytes;
    ibv.Format = DXGI_FORMAT_R16_UINT;
    cmdList->IASetIndexBuffer(&ibv);

    cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    // Draw
    ID3D12PipelineState* lastPSO = nullptr;
    {
        static std::atomic<int> s_commandDetailLog{0};
        const int logFrame = s_commandDetailLog.fetch_add(1, std::memory_order_relaxed);
        if (logFrame < 6) {
            for (size_t cmdIndex = 0; cmdIndex < commands.size(); ++cmdIndex) {
                const auto& cmd = commands[cmdIndex];
                HookLogImportant(
                    "DX12 DIAG: DescFree command frame=%d cmd=%zu textured=%d vtxOff=%u vtxCount=%u idxOff=%u "
                    "idxCount=%u",
                    logFrame, cmdIndex, cmd.useTexture ? 1 : 0, cmd.vertexOffset, cmd.vertexCount, cmd.indexOffset,
                    cmd.indexCount);
            }
        }
    }
    for (const auto& cmd : commands) {
        auto* pso = cmd.useTexture ? psoTextured_ : psoSolid_;
        if (pso != lastPSO) {
            cmdList->SetPipelineState(pso);
            lastPSO = pso;
        }
        cmdList->DrawIndexedInstanced(cmd.indexCount, 1, cmd.indexOffset, 0, 0);
    }
    slotFenceValue_[slot] = dx12_hook_s_descFreeSlotGuardValue;

    // DIAGNOSTIC: per-frame overlay GPU footprint (draw count + vertex/index bytes). This is
    // what CE submits to the app's queue each frame; compare 32-bit vs 64-bit hook_debug.log
    // (expected identical — confirming the freeze is WoW64 allocation speed, not a different
    // CE code path — and quantifying how much a cached-texture composite would save).
    {
        static std::atomic<int> s_overlayFootprintLog{0};
        const int n = s_overlayFootprintLog.fetch_add(1, std::memory_order_relaxed);
        if (n < 5 || (n % 600) == 0) {
            HookLogImportant("DX12 DIAG: overlay footprint draws=%zu vbBytes=%zu ibBytes=%zu slot=%d sample=%d",
                             commands.size(), vbBytes, ibBytes, slot, n);
        }
    }
}

void DX12DescFreeBackend::Shutdown()  {
    // During process termination, D3D12/NVIDIA driver may be partially torn down.
    // Skip GPU resource cleanup to avoid access violations in driver code.
    if (IsProcessTerminating())
        return;
    for (int i = 0; i < kPoolSize; i++) {
        if (vb_[i]) {
            vb_[i]->Unmap(0, nullptr);
            vb_[i]->Release();
            vb_[i] = nullptr;
        }
        if (ib_[i]) {
            ib_[i]->Unmap(0, nullptr);
            ib_[i]->Release();
            ib_[i] = nullptr;
        }
        vbPtr_[i] = nullptr;
        ibPtr_[i] = nullptr;
        vbSize_[i] = 0;
        ibSize_[i] = 0;
    }
    if (fontBuffer_) {
        fontBuffer_->Release();
        fontBuffer_ = nullptr;
    }
    if (fontUploadBuffer_) {
        fontUploadBuffer_->Release();
        fontUploadBuffer_ = nullptr;
    }
    if (psoTextured_) {
        psoTextured_->Release();
        psoTextured_ = nullptr;
    }
    if (psoSolid_) {
        psoSolid_->Release();
        psoSolid_ = nullptr;
    }
    if (rootSig_) {
        rootSig_->Release();
        rootSig_ = nullptr;
    }
    fontGpuAddr_ = 0;
    fontBufferSize_ = 0;
    fontUploadPending_ = false;
    deviceReady_ = false;
    // Drop the (non-owning) slot fence binding and guards; a fresh InitDevice
    // rebinds via the published static, and the GPU work that referenced this
    // backend's ring is gone.
    slotFence_ = nullptr;
    for (int i = 0; i < kPoolSize; ++i)
        slotFenceValue_[i] = 0;
}

bool DX12DescFreeBackend::CreateRootSignature() {
    // Parameter 0: 6 root constants at b0
    //   [0-1] viewportSize, [2] hdrMode, [3] paperWhiteNits, [4-5] fontTexSize
    D3D12_ROOT_PARAMETER params[2] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.ShaderRegister = 0;
    params[0].Constants.RegisterSpace = 0;
    params[0].Constants.Num32BitValues = 6;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    // Parameter 1: root SRV at t0 (font StructuredBuffer<uint>)
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    params[1].Descriptor.ShaderRegister = 0;
    params[1].Descriptor.RegisterSpace = 0;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC rsDesc = {};
    rsDesc.NumParameters = 2;
    rsDesc.pParameters = params;
    rsDesc.NumStaticSamplers = 0;  // No sampler needed — manual bilinear in shader
    rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ID3DBlob* blob = nullptr;
    ID3DBlob* err = nullptr;
    HRESULT hr = D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err);
    if (FAILED(hr)) {
        HookLogImportant("DescFree: SerializeRootSignature failed hr=0x%08X", hr);
        if (err)
            err->Release();
        return false;
    }
    hr = device_->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&rootSig_));
    blob->Release();
    if (err)
        err->Release();
    if (FAILED(hr)) {
        HookLogImportant("DescFree: CreateRootSignature failed hr=0x%08X", hr);
        return false;
    }
    return true;
}

bool DX12DescFreeBackend::CreatePSOs() {
    D3D12_INPUT_ELEMENT_DESC inputLayout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 16, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    };

    // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization) - zero-initialized placeholder; enum fields are assigned before use
    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.InputLayout = {inputLayout, 3};
    psoDesc.pRootSignature = rootSig_;
    psoDesc.VS = {g_VS_5_0, sizeof(g_VS_5_0)};
    psoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    psoDesc.RasterizerState.DepthClipEnable = TRUE;

    // Alpha blending
    D3D12_RENDER_TARGET_BLEND_DESC& blendRT = psoDesc.BlendState.RenderTarget[0];
    blendRT.BlendEnable = TRUE;
    blendRT.SrcBlend = D3D12_BLEND_SRC_ALPHA;
    blendRT.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    blendRT.BlendOp = D3D12_BLEND_OP_ADD;
    blendRT.SrcBlendAlpha = D3D12_BLEND_ONE;
    blendRT.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
    blendRT.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    blendRT.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    psoDesc.DepthStencilState.DepthEnable = FALSE;
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = rtvFormat_;
    psoDesc.SampleDesc.Count = 1;

    // Textured PSO — uses StructuredBuffer<uint> (descriptor-free)
    psoDesc.PS = {g_PS_Textured_DescFree_5_0, sizeof(g_PS_Textured_DescFree_5_0)};
    HRESULT hr = device_->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&psoTextured_));
    if (FAILED(hr)) {
        HookLogImportant("DescFree: CreatePSO(textured) failed hr=0x%08X", hr);
        return false;
    }

    // Solid PSO — no texture, uses same root sig (t0 unused)
    psoDesc.PS = {g_PS_Solid_5_0, sizeof(g_PS_Solid_5_0)};
    hr = device_->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&psoSolid_));
    if (FAILED(hr)) {
        HookLogImportant("DescFree: CreatePSO(solid) failed hr=0x%08X", hr);
        return false;
    }

    HookLogImportant("DescFree: PSOs created (fmt=%d)", rtvFormat_);
    return true;
}

bool DX12DescFreeBackend::CreateBuffers() {
    // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization) - zero-initialized placeholder; enum fields are assigned before use
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Height = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_UNKNOWN;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    D3D12_RANGE readRange = {0, 0};
    for (int i = 0; i < kPoolSize; i++) {
        rd.Width = kInitVBBytes;
        HRESULT hr = device_->CreateCommittedResource(
            &hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&vb_[i]));
        if (FAILED(hr))
            return false;
        vb_[i]->Map(0, &readRange, &vbPtr_[i]);
        vbSize_[i] = kInitVBBytes;

        rd.Width = kInitIBBytes;
        hr = device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_GENERIC_READ,
                                              nullptr, IID_PPV_ARGS(&ib_[i]));
        if (FAILED(hr))
            return false;
        ib_[i]->Map(0, &readRange, &ibPtr_[i]);
        ibSize_[i] = kInitIBBytes;
    }
    return true;
}

bool DX12DescFreeBackend::ResizeBuffer(ID3D12Resource*& buf, void*& ptr, size_t& curSize, size_t needed) {
    size_t newSize = curSize;
    while (newSize < needed)
        newSize *= 2;

    buf->Unmap(0, nullptr);
    buf->Release();

    // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization) - zero-initialized placeholder; enum fields are assigned before use
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = newSize;
    rd.Height = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_UNKNOWN;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    HRESULT hr = device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_GENERIC_READ,
                                                  nullptr, IID_PPV_ARGS(&buf));
    if (FAILED(hr))
        return false;

    D3D12_RANGE readRange = {0, 0};
    buf->Map(0, &readRange, &ptr);
    curSize = newSize;
    return true;
}

bool DX12DescFreeBackend::WaitForSlotGpuComplete(int slot) {
    if (!slotFence_ || slot < 0 || slot >= kPoolSize) {
        return true;

    }
    const UINT64 guardValue = slotFenceValue_[slot];
    if (!ce::dx12_overlay_policy::ShouldWaitForOverlayUploadSlot(guardValue, slotFence_->GetCompletedValue())) {
        return true;
    }
    HANDLE eventHandle = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!eventHandle) {
        return false;
    }
    bool completed = false;
    if (SUCCEEDED(slotFence_->SetEventOnCompletion(guardValue, eventHandle))) {
        // The fence is the real synchronization that closes the CPU<->GPU
        // UPLOAD-buffer data race.  The bounded timeout is purely a liveness
        // safety net: a separate code path (FG transition / overlay reinit)
        // may legitimately discard the pending Signal for this guard value,
        // which would otherwise wedge the present thread forever.  On timeout
        // we skip this overlay draw; reusing the slot would corrupt in-flight
        // GPU reads and can turn a transient mode switch into DEVICE_HUNG.
        const DWORD waitResult = WaitForSingleObject(eventHandle, kSlotWaitTimeoutMs);
        completed = waitResult == WAIT_OBJECT_0;
        if (!completed) {
            static std::atomic<int> s_slotWaitTimeoutLog{0};
            const int logN = s_slotWaitTimeoutLog.fetch_add(1, std::memory_order_relaxed);
            if (logN < 40 || (logN % 200) == 0) {
                HookLogImportant(
                    "DescFree: slot %d GPU-completion wait %s (guard=%llu completed=%llu) — overlay upload ring "
                    "draw skipped to avoid reusing in-flight GPU data",
                    slot, waitResult == WAIT_TIMEOUT ? "timed out" : "failed", (unsigned long long)guardValue,
                    (unsigned long long)slotFence_->GetCompletedValue());
            }
        }
    }
    CloseHandle(eventHandle);
    return completed;
}

void DX12OverlayState::Cleanup() {
    // FG-SAFE: backBuffers no longer holds references
    backBuffers.clear();
    if (offscreenRT) {
        offscreenRT->Release();
        offscreenRT = nullptr;
    }
    if (offscreenRtvHeap) {
        offscreenRtvHeap->Release();
        offscreenRtvHeap = nullptr;
    }
    offscreenWidth = 0;
    offscreenHeight = 0;
    offscreenFormat = DXGI_FORMAT_UNKNOWN;
    if (rtvDescHeap) {
        rtvDescHeap->Release();
        rtvDescHeap = nullptr;
    }
    if (srvDescHeap) {
        srvDescHeap->Release();
        srvDescHeap = nullptr;
    }
    for (auto* alloc : allocators)
        if (alloc)
            alloc->Release();
    allocators.clear();
    if (cmdList) {
        cmdList->Release();
        cmdList = nullptr;
    }
    if (fence) {
        fence->Release();
        fence = nullptr;
    }
    if (fenceEvent) {
        CloseHandle(fenceEvent);
        fenceEvent = nullptr;
    }
    // CRITICAL: Release dedicated overlay queue
    if (overlayQueue) {
        overlayQueue->Release();
        overlayQueue = nullptr;
    }
    // Release cross-queue synchronization fence and event
    if (crossQueueFenceEvent) {
        CloseHandle(crossQueueFenceEvent);
        crossQueueFenceEvent = nullptr;
    }
    if (crossQueueFence) {
        crossQueueFence->Release();
        crossQueueFence = nullptr;
    }
    overlayInit = false;
    syncInit = false;
    crossQueueFenceValue = 0;
    cachedSC3 = nullptr;  // weak ref, no Release needed
    // D3D11On12 cleanup
    for (auto* rtv : d3d11RTVs)
        if (rtv)
            rtv->Release();
    d3d11RTVs.clear();
    for (auto* res : d3d11WrappedBBs)
        if (res)
            res->Release();
    d3d11WrappedBBs.clear();
    if (d3d11on12) {
        d3d11on12->Release();
        d3d11on12 = nullptr;
    }
    if (d3d11on12Context) {
        d3d11on12Context->Release();
        d3d11on12Context = nullptr;
    }
    if (d3d11on12Device) {
        d3d11on12Device->Release();
        d3d11on12Device = nullptr;
    }
    d3d11on12Init = false;
}

DX12Context::DX12Context(ID3D12Device* d, ID3D12CommandQueue* q) : device(d), queue(q) {
    if (device)
        device->AddRef();
    if (queue)
        queue->AddRef();
}

DX12Context::~DX12Context() {
    if (device) {
        device->Release();
        device = nullptr;
    }
    if (queue) {
        queue->Release();
        queue = nullptr;
    }
}

DX12Context::DX12Context(DX12Context&& other) noexcept : device(other.device), queue(other.queue) {
    other.device = nullptr;
    other.queue = nullptr;
}

bool DX12Context::IsValid() const {
    return device != nullptr && queue != nullptr;
}

ScopedForwardedCreateSwapchainForHwndCallerContext::ScopedForwardedCreateSwapchainForHwndCallerContext(const void* callerAddress, const char* callerModulePath)
    : previousContext_(dx12_hook_s_forwardedCreateSwapchainForHwndCallerContext) {
    dx12_hook_s_forwardedCreateSwapchainForHwndCallerContext = {};
    dx12_hook_s_forwardedCreateSwapchainForHwndCallerContext.callerAddress = callerAddress;
    if (callerModulePath && *callerModulePath) {
        strncpy_s(dx12_hook_s_forwardedCreateSwapchainForHwndCallerContext.callerModulePath,
                  sizeof(dx12_hook_s_forwardedCreateSwapchainForHwndCallerContext.callerModulePath), callerModulePath,
                  _TRUNCATE);
    }
}

ScopedForwardedCreateSwapchainForHwndCallerContext::~ScopedForwardedCreateSwapchainForHwndCallerContext() {
    dx12_hook_s_forwardedCreateSwapchainForHwndCallerContext = previousContext_;
}

ScopedForwardedCreateSwapchainForHwndInlineSideEffectGuard::ScopedForwardedCreateSwapchainForHwndInlineSideEffectGuard()
    : previousDepth_(dx12_hook_s_forwardedCreateSwapchainForHwndInlineDepth),
      previousHandled_(dx12_hook_s_forwardedCreateSwapchainForHwndInlineHandled) {
    dx12_hook_s_forwardedCreateSwapchainForHwndInlineDepth = previousDepth_ + 1;
    dx12_hook_s_forwardedCreateSwapchainForHwndInlineHandled = false;
}

ScopedForwardedCreateSwapchainForHwndInlineSideEffectGuard::~ScopedForwardedCreateSwapchainForHwndInlineSideEffectGuard() {
    dx12_hook_s_forwardedCreateSwapchainForHwndInlineDepth = previousDepth_;
    dx12_hook_s_forwardedCreateSwapchainForHwndInlineHandled = previousHandled_;
}

bool ScopedForwardedCreateSwapchainForHwndInlineSideEffectGuard::InlineHandledForwardedCall() const {
    return dx12_hook_s_forwardedCreateSwapchainForHwndInlineHandled;
}

 ScopedCEOverlayECLSubmission::ScopedCEOverlayECLSubmission(const char* reason) : previousReason_(dx12_hook_s_insideCEOverlayECLReason) {
    ++dx12_hook_s_insideCEOverlayECLDepth;
    dx12_hook_s_insideCEOverlayECLReason = reason;
}

ScopedCEOverlayECLSubmission::~ScopedCEOverlayECLSubmission() {
    dx12_hook_s_insideCEOverlayECLReason = previousReason_;
    --dx12_hook_s_insideCEOverlayECLDepth;
}

