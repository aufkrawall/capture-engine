}

bool HookIsPostSLOverlayConfirmedButRuntimeStateStabilizing() {
    const bool extendRuntimeStateStabilization =
        g_PostSLExtendedRuntimeStateStabilizationForCurrentEpoch.load(std::memory_order_acquire);
    return ce::dx12_overlay_policy::ShouldTreatConfirmedPostSLRenderingAsRuntimeStateStabilizing(
        g_PostSLConfirmedRendering.load(std::memory_order_acquire),
        g_PostSLStableFrameCount.load(std::memory_order_acquire), extendRuntimeStateStabilization);
}

int HookGetPostSLRuntimeStateStabilizationLastFrame() {
    return ce::dx12_overlay_policy::GetConfirmedPostSLRuntimeStateStabilizationLastFrame(
        g_PostSLExtendedRuntimeStateStabilizationForCurrentEpoch.load(std::memory_order_acquire));
}

bool HookIsPostSLOverlayConfirmedButStaleOffWarmupProtected() {
    return ce::dx12_overlay_policy::ShouldDeferStaleOffDuringConfirmedPostSLWarmup(
        g_PostSLConfirmedRendering.load(std::memory_order_acquire),
        g_PostSLStableFrameCount.load(std::memory_order_acquire));
}

int HookGetPostSLStaleOffWarmupProtectionLastFrame() {
    return ce::dx12_overlay_policy::GetConfirmedPostSLStaleOffWarmupProtectionLastFrame();
}

bool HookIsPostSLOverlayConfirmedButGetStateOffWarmupProtected() {
    return HookIsPostSLOverlayConfirmedButStaleOffWarmupProtected();
}

int HookGetPostSLGetStateOffWarmupProtectionLastFrame() {
    return HookGetPostSLStaleOffWarmupProtectionLastFrame();
}

// Flag to reset the queue-change heuristic's internal state.  Set during FG
// transitions so that the heuristic starts fresh afterward. Most transitions
// recapture the "initial queue" from the next 5 frames; a proven normal
// swapchain return supplies its authoritative game queue directly. Without
// this, SL's leftover queue persists after FG OFF → immediate false FSR FG
// detection → wrong queue selection, blank overlay, or DEVICE_HUNG.
static std::atomic<bool> g_ResetQueueChangeHeuristic{false};

// Companion reset for the ECL-count-pattern FG heuristic in
// DX12_ProcessFrameExternal. Interpolated/real frame counts accumulated during
// a finished FG phase are stale evidence after any FG transition or swapchain
// recovery: session 20260612_215439 showed `FG detected via ECL count pattern
// (real=5, interp=70)` re-latching phantom FSR_FG on the game's fresh native
// swapchain right after FSR->OFF, and the resulting double mode flip armed two
// 60-frame draw cooldowns that blanked a healthy overlay for 61 presents.
// Set at every site that resets the queue-change heuristic.
static std::atomic<bool> g_ResetECLPatternHeuristic{false};
// Optional authoritative queue for the next queue-change heuristic epoch. A
// normal swapchain return supplies the proven original queue here so leftover
// Streamline ECL traffic cannot become the new baseline or a phantom FSR edge.
// g_OriginalGameQueue retains this COM object for the hook lifetime.
static std::atomic<ID3D12CommandQueue*> g_QueueChangeHeuristicAuthoritativeBaseline{nullptr};

static void RequestFGDetectionHeuristicReset(ID3D12CommandQueue* authoritativeBaseline = nullptr) {
    g_QueueChangeHeuristicAuthoritativeBaseline.store(authoritativeBaseline, std::memory_order_release);
    g_ResetQueueChangeHeuristic.store(true, std::memory_order_release);
    g_ResetECLPatternHeuristic.store(true, std::memory_order_release);
}

// Grace counter after SL FG turns OFF.  Set by the outer block, decremented each
// frame in CanUseFSRFGHeuristics().  While active, the queue-change heuristic is
// suppressed — the queue naturally switches from SL's internal queue back to
// origGame, which would otherwise false-positive as FSR FG.
static std::atomic<int> g_SLOffHeuristicGrace{0};

// Grace counter after Streamline FG turns OFF. Set when PostSL/Streamline
// ownership tears down, and consumed by ProcessFrame swapchain-change handling
// to keep the first replacement swapchain on the guarded transition path.
static std::atomic<int> g_SLOffSwapchainReinitGrace{0};

// Reset flag for per-reinit submit diagnostic counter.
static std::atomic<bool> g_ResetReinitSubmitCounter{false};

// Outer SL transition epoch — incremented each time the outer FG state management
// block processes an SL FG ON/OFF transition.  The inner transition handler checks
// this to avoid redundant transition processing (double cooldowns, duplicate drain).
static std::atomic<uint32_t> g_OuterSLTransitionEpoch{0};

// Last Streamline FG signal observed by the outer ProcessFrame transition block.
// Kept at file scope so direct Streamline teardown paths can synchronize it when
// they invalidate overlay state before ProcessFrame reaches the outer tracker.
static std::atomic<bool> g_OuterTrackedSLFGRunning{false};

// Locked queue for PostSL overlay — stays on the first successful queue
// (game's render queue) instead of following SL's FG worker queue changes.
// Reset when PostSL rendering is disabled (FG transition off).
// Protected by atomic exchange in writer; readers must load via the
// dedicated helper or raw pointer access when no concurrent write is possible.
static ID3D12CommandQueue* g_PostSLLockedQueue = nullptr;

// Tracks whether FSR FG was ever active during this session.
// Once set, origGame is assumed corrupted (NVIDIA driver internal state broken
// after FSR FG phase) and PostSL uses g_CommandQueue (SL's wrapper) instead.
static std::atomic<bool> g_HadFSRFGPhase{false};
// Durable process-lifetime proof that PostSL completed at least one real,
// device-healthy submit. Unlike current-route confirmation, this survives an
// authoritative DLSS OFF/native return so a later pure-DLSS proxy can prewarm
// before its FG-off passthrough Present. Exact device/queue/backend guards are
// still revalidated at every handoff.
static std::atomic<bool> g_HadSuccessfulPostSLPhase{false};

bool HookHasFSRFGHistory() {
    return g_HadFSRFGPhase;
}

bool HookHasExplicitStreamlineSetOptionsActivation() {
    return StreamlineHook::HasExplicitSetOptionsActivationForCurrentComeback();
}

// After FSR→DLSS→OFF: the swapchain's backbuffers have indeterminate GPU
// resource state from the FG pipeline teardown.  Direct rendering with
// explicit PRESENT→RENDER_TARGET barriers causes DEVICE_REMOVED when the
// barrier's StateBefore doesn't match the actual backbuffer state.  The
// offscreen compositing path (CopyTextureRegion + implicit state promotion)
// avoids ALL explicit barriers on the backbuffer, making it safe regardless
// of actual state.  Cleared on clean swapchain transition (non-FG).
static std::atomic<bool> g_NeedOffscreenOverlayAfterPostFSRNonFG{false};

// GPU drain: flush all in-flight GPU work before first overlay render after
// FSR→DLSS transition.  SL's FG pipeline may have concurrent backbuffer
// access that causes DEVICE_HUNG if we draw simultaneously.
static bool g_NeedGPUDrainBeforeRender = false;
static ID3D12Fence* g_DrainFence = nullptr;
static HANDLE g_DrainEvent = nullptr;
static UINT64 g_DrainFenceValue = 0;

// PostSL ECL diagnostic counter — reset on each PostSL reactivation epoch.
std::atomic<int> g_PostSLECLDiagCount{0};

// Post-FSR graduated probe system: after FSR→DLSS transition, incrementally test
// what rendering operations are safe before committing to full overlay render.
static std::atomic<int> g_PostFSRProbeLevel{0};  // 0=scratch, 1=reserved, 2=offscreen-copy-only, 3=full allowed
static std::atomic<int> g_PostFSRProbeFrames{0};
static constexpr int kPostFSRProbeFramesPerLevel = 3;
static bool g_PostFSRDescFreeRecreated = false;

// Dedicated queue created specifically for PostSL overlay rendering.
// After FG transitions (FSR→DLSS), the NVIDIA driver's internal state for
// the game's original queue can become corrupted (null pointer dereference
// in nvwgf2umx during ECL).  A fresh queue bypasses this corruption.
// Created once on first PostSL need, re-created after FG transitions.
static ID3D12CommandQueue* g_PostSLDedicatedQueue = nullptr;

// Last queue that PostSL successfully submitted to without DEVICE_REMOVED.
// Survives FG transitions — used as preferred queue when PostSL re-activates
// after FSR→DLSS switch (where g_CommandQueue or scQueue might be wrong).
// AddRef'd to prevent use-after-free when g_CommandQueue is updated.
// NOTE: this pointer is read by the ECL detour concurrently with FG-mode
// switches that write it.  The writer (SetPostSLLastWorkingQueue) uses
// exchange + AddRef/Release so the new pointer is alive before the old is
// dropped.  ECL readers should grab a local AddRef'd copy; the PostSL path
// runs under the lifecycle epoch guard which prevents concurrent release.
static ID3D12CommandQueue* g_PostSLLastWorkingQueue = nullptr;

static void SetPostSLLastWorkingQueue(ID3D12CommandQueue* queue) {
    if (queue == g_PostSLLastWorkingQueue)
        return;
    if (queue)
        queue->AddRef();
    if (g_PostSLLastWorkingQueue)
        g_PostSLLastWorkingQueue->Release();
    g_PostSLLastWorkingQueue = queue;
}

// File-scope scene transition cooldown.  Set by ProcessFrame when a large
// frametime gap is detected during FG, checked by PostSLOverlayRender to
// suppress overlay during scene loads.
static std::atomic<int> g_SceneTransitionCooldown{0};

// Captures ProcessFrame's resolved gameQueue right before FG transitions ON.
// When PostSL activates after FG, g_CommandQueue may have been polluted by
// SL's internal FG queues and g_SwapchainQueue may be null (e.g., Talos).
// This provides a reliable fallback to the game's real queue.
static ID3D12CommandQueue* g_PreFGGameQueue = nullptr;

// The game's very first command queue, captured before any FG ever activates.
// During FG transitions, ALL queue sources can get polluted by SL/FSR internal
// queues (g_CommandQueue from ECL hook, g_SwapchainQueue from CreateSwapChain
// called by SL).  The original queue is the only one guaranteed to be the game's
// own queue and works reliably for PostSL overlay during DLSS FG.
static ID3D12CommandQueue* g_OriginalGameQueue = nullptr;

// File-scope FG transition cooldown.  Counts down frames after any FG mode
// change (on/off, FSR→DLSS, SL signal change).  While >0, overlay reinit is
// suppressed so that SL / FSR FG can finish initializing without interference.
// Set by ProcessFrame's FG transition detection; checked by swapchain-change
// invalidation to prevent premature overlay reinit during FG mode switches.
static std::atomic<int> g_FGTransitionCooldown{0};

// Counts frames since FG was last active.  When a game switches FG modes
// (e.g., FSR FG → DLSS FG), FG heuristics may go inactive for many frames
// before the swapchain actually changes.  A simple "was active last frame"
// flag misses cases where the gap is >1 frame.  This counter lets the
// swapchain-change logic detect that FG was active recently (within a window)
// even if several "inactive" frames have elapsed.
static int g_FramesSinceFGActive = 9999;

class DX12DescFreeBackend : public CustomOverlay::RendererBackend {
public:
    ~DX12DescFreeBackend() override {
        Shutdown();
    }

    // Non-virtual: create device-dependent resources (root sig, PSOs)
    bool InitDevice(ID3D12Device* dev, DXGI_FORMAT rtvFormat) {
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

    // RendererBackend: stage font atlas for a descriptor-free structured uint buffer.
    // The pixel shader samples from a DEFAULT-heap buffer; reading a UPLOAD heap
    // directly in the text draw has proven fragile on the x86 NVIDIA path.
    bool Initialize(int fontWidth, int fontHeight, const uint8_t* fontData) override {
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

    void Render(const std::vector<CustomOverlay::DrawVertex>& vertices, const std::vector<uint16_t>& indices,
                const std::vector<CustomOverlay::DrawCommand>& commands, int vpW, int vpH) override {
        auto* cmdList = s_descFreeCmdList;
        if (!cmdList || !deviceReady_ || !fontBuffer_ || vertices.empty())
            return;
        if (fontUploadPending_ && !fontUploadBuffer_)
            return;

        // Rebind the per-slot GPU-completion fence.  If the fence object changed
        // (overlay reinit recreates g_State.fence), the recorded guard values
        // belong to a dead fence — discard them so we never wait on a stale or
        // released fence.
        if (s_descFreeSlotFence != slotFence_) {
            for (int i = 0; i < kPoolSize; ++i)
                slotFenceValue_[i] = 0;
            slotFence_ = s_descFreeSlotFence;
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
        cmdList->OMSetRenderTargets(1, &s_descFreeRtv, FALSE, nullptr);
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
        slotFenceValue_[slot] = s_descFreeSlotGuardValue;

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

    void Shutdown() override {
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

private:
    bool CreateRootSignature() {
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

    bool CreatePSOs() {
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

    bool CreateBuffers() {
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

    bool ResizeBuffer(ID3D12Resource*& buf, void*& ptr, size_t& curSize, size_t needed) {
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

    bool WaitForSlotGpuComplete(int slot) {
        if (!slotFence_ || slot < 0 || slot >= kPoolSize) {
            return true;
