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

    // Liveness bound for WaitForSlotGpuComplete (see comment there).  Must stay
    // below the ~2s GPU TDR; a normal Alt+Tab mode-switch pause resumes the GPU
    // in well under this, so the wait returns as soon as the slot is free.
    static constexpr DWORD kSlotWaitTimeoutMs = 1000;

    static constexpr int kPoolSize = 4;
    static constexpr size_t kInitVBBytes = 4096 * 20;  // 4096 vertices * 20 bytes
    static constexpr size_t kInitIBBytes = 8192 * 2;   // 8192 indices * 2 bytes

    ID3D12Device* device_ = nullptr;
    DXGI_FORMAT rtvFormat_ = DXGI_FORMAT_R8G8B8A8_UNORM;
    bool deviceReady_ = false;

    ID3D12RootSignature* rootSig_ = nullptr;
    ID3D12PipelineState* psoTextured_ = nullptr;
    ID3D12PipelineState* psoSolid_ = nullptr;

    ID3D12Resource* fontBuffer_ = nullptr;
    ID3D12Resource* fontUploadBuffer_ = nullptr;
    D3D12_GPU_VIRTUAL_ADDRESS fontGpuAddr_ = 0;
    size_t fontBufferSize_ = 0;
    bool fontUploadPending_ = false;
    int fontWidth_ = 0;
    int fontHeight_ = 0;

    ID3D12Resource* vb_[kPoolSize] = {};
    ID3D12Resource* ib_[kPoolSize] = {};
    void* vbPtr_[kPoolSize] = {};
    void* ibPtr_[kPoolSize] = {};
    size_t vbSize_[kPoolSize] = {};
    size_t ibSize_[kPoolSize] = {};
    ID3D12Fence* slotFence_ = nullptr;
    UINT64 slotFenceValue_[kPoolSize] = {};
    int frameIdx_ = 0;
};

static DX12DescFreeBackend* g_DescFreeBackend = nullptr;

// --- DX12 Overlay State Management ---
struct DX12OverlayState {
    // Large pool size ensures we never need to wait for GPU.
    // Even at 60fps with 100ms GPU latency, only 6 allocators are in flight.
    // 16 provides 2.5x headroom - allocator is always ready, zero waiting.
    static const int ALLOC_POOL_SIZE = 16;
    std::vector<ID3D12CommandAllocator*> allocators;
    ID3D12GraphicsCommandList* cmdList = nullptr;
    int allocIndex = 0;
    ID3D12Fence* fence = nullptr;
    HANDLE fenceEvent = nullptr;
    UINT64 currentFenceValue = 0;
    std::vector<UINT64> fenceValues;
    ID3D12DescriptorHeap* rtvDescHeap = nullptr;
    ID3D12DescriptorHeap* srvDescHeap = nullptr;
    UINT rtvDescriptorSize = 0;
    std::vector<ID3D12Resource*> backBuffers;
    bool overlayInit = false;
    bool syncInit = false;
    int cachedWidth = 0;
    int cachedHeight = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    UINT bufferCount = 0;
    IDXGISwapChain* cachedSwapChain = nullptr;

    // Offscreen render target for overlay compositing.
    // Avoids OMSetRenderTargets(swapchain) + SetDescriptorHeaps on the same ECL,
    // which causes GPU pipeline stalls (~40% utilization loss) in some games.
    // Flow: copy backbuffer → offscreen, render overlay → offscreen, copy back.
    ID3D12Resource* offscreenRT = nullptr;
    ID3D12DescriptorHeap* offscreenRtvHeap = nullptr;
    UINT offscreenWidth = 0;
    UINT offscreenHeight = 0;
    DXGI_FORMAT offscreenFormat = DXGI_FORMAT_UNKNOWN;

    // Dedicated overlay command queue for FG-safe rendering.
    // When FG is active, overlay commands execute on this queue with CPU-side
    // fence synchronization to avoid interfering with Streamline's game queue
    // management.  When FG is not active, overlay commands go on the game queue
    // directly (zero CPU waits).
    ID3D12CommandQueue* overlayQueue = nullptr;

    // Cross-queue fence: game queue signals to mark work completion, then
    // CPU-side wait before submitting overlay work on the overlay queue.
    // GPU-side CommandQueue::Wait was removed (NVIDIA WaitImpl Alt+Tab hang).
    ID3D12Fence* crossQueueFence = nullptr;
    UINT64 crossQueueFenceValue = 0;
    HANDLE crossQueueFenceEvent = nullptr;

    IDXGISwapChain3* cachedSC3 = nullptr;  // cached from first successful QI

    // The device used to create sync resources (allocators, command list, fence).
    // Must match the submission queue's device — cross-device submission = DEVICE_REMOVED.
    ID3D12Device* syncDevice = nullptr;

    // D3D11On12 overlay bridge: renders overlay via D3D11 on top of the D3D12
    // backbuffer.  D3D11 doesn't use descriptor heaps, avoiding the NVIDIA
    // driver stall triggered by SetDescriptorHeaps + OMSetRenderTargets(swapchain).
    ID3D11Device* d3d11on12Device = nullptr;
    ID3D11DeviceContext* d3d11on12Context = nullptr;
    ID3D11On12Device* d3d11on12 = nullptr;
    std::vector<ID3D11Resource*> d3d11WrappedBBs;
    std::vector<ID3D11RenderTargetView*> d3d11RTVs;
    bool d3d11on12Init = false;

    void Cleanup() {
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
};

static DX12OverlayState g_State;

// ============================================================
// Steam ECL deferred overlay submission state
// ============================================================
// Set by DetourPresent (dxgi_shared.cpp) before invoking Steam's overlay handler.
// When true, ProcessFrame records overlay commands into g_State.cmdList and closes
// the list, but skips the actual ECL submission.  The submission is deferred to
// DetourExecuteCommandLists, which fires the CE overlay ECL AFTER Steam's overlay
// ECL has been submitted to the queue.  This ensures CE's overlay renders on top
// of Steam's cleared backbuffer instead of being cleared by Steam.
static bool g_deferOverlaySubmitToSteamECL = false;

struct SteamDeferredOverlaySubmitState {
    ID3D12CommandList* cmdList = nullptr;
    int allocIdx = -1;
    ID3D12CommandQueue* eclQueue = nullptr;
    bool pending = false;
};
static SteamDeferredOverlaySubmitState g_steamDeferredOverlay;

extern "C" __declspec(dllexport) void DX12_SetDeferOverlaySubmitToSteamECL(bool defer) {
    g_deferOverlaySubmitToSteamECL = defer;
    if (!defer) {
        // Clear any stale deferred state
        g_steamDeferredOverlay.pending = false;
        g_steamDeferredOverlay.cmdList = nullptr;
        g_steamDeferredOverlay.allocIdx = -1;
        g_steamDeferredOverlay.eclQueue = nullptr;
    }
}

// Check if deferred overlay is still pending (not consumed by ECL hook).
extern "C" __declspec(dllexport) bool DX12_IsDeferOverlaySubmitPending() {
    return g_steamDeferredOverlay.pending;
}

    // NOLINTNEXTLINE(bugprone-throwing-static-initialization) - static object default construction is non-allocating (members are trivial or empty)
static SharedCaptureD3D12 g_SharedCaptureD3D12;
    // NOLINTNEXTLINE(bugprone-throwing-static-initialization) - static object default construction is non-allocating (members are trivial or empty)
static OverlayAdapter g_D3D11On12Adapter;
// Separate overlay adapter for D3D11On12 rendering during Streamline FG.
// Uses the DX11 backend via D3D11On12 bridge to properly manage cross-queue
// resource transitions, which SL's FG pipeline can track.
    // NOLINTNEXTLINE(bugprone-throwing-static-initialization) - static object default construction is non-allocating (members are trivial or empty)
static OverlayAdapter g_SLFGAdapter;
// Native/runtime-owned FSR present-callback rendering must not inherit stale
// normal/DLSS DX12 backend state across later FFX-owned swapchain/device handoffs.
    // NOLINTNEXTLINE(bugprone-throwing-static-initialization) - static object default construction is non-allocating (members are trivial or empty)
static OverlayAdapter g_FFXPresentOverlayAdapter;
static ID3D12Device* g_FFXPresentOverlayDevice = nullptr;
static DXGI_FORMAT g_FFXPresentOverlayFormat = DXGI_FORMAT_UNKNOWN;

// Live backbuffer geometry cached from the FG-enable ffxConfigure swapchain (see
// DX12_TryCacheRuntimeOwnedCallbackHDRStateFromSwapchain). Used to size CE's substitute UI texture and to
// size-classify the game's registered no-callback FSR FG UI texture (usable HUD surface vs degenerate
// placeholder). Declared here (before that helper) so the cache write can reference them.
static std::atomic<uint32_t> g_NoCallbackBackbufferWidth{0};
static std::atomic<uint32_t> g_NoCallbackBackbufferHeight{0};
static std::atomic<uint32_t> g_NoCallbackBackbufferFormat{0};  // DXGI_FORMAT

// Device/format the descriptor-free backend was built for. The backend is
// DEVICE-scoped (PSOs, root signature, font buffer, vb/ib upload pool; the
// backbuffer is fetched per frame) and intentionally survives swapchain
// teardown so the first present of a new swapchain can draw without a backend
// rebuild. These trackers gate the only rebuild triggers: device or RTV
// format change.
static ID3D12Device* g_DescFreeBackendDevice = nullptr;
static DXGI_FORMAT g_DescFreeBackendFormat = DXGI_FORMAT_UNKNOWN;

static void ShutdownDescFreeBackend(const char* reason, bool shutdownMode = false) {
    g_DescFreeBackendDevice = nullptr;
    g_DescFreeBackendFormat = DXGI_FORMAT_UNKNOWN;
    DX12DescFreeBackend* backend = g_DescFreeBackend;
    const bool adapterInitialized = g_D3D11On12Adapter.IsInitialized();
    if (!backend && !adapterInitialized) {
        return;
    }

    CustomOverlay::RendererBackend* adapterBackend = adapterInitialized ? g_D3D11On12Adapter.GetBackend() : nullptr;
    const OverlayBackendType adapterType =
        adapterInitialized ? g_D3D11On12Adapter.GetBackendType() : OverlayBackendType::None;
    const bool adapterOwnsBackend = backend && adapterBackend == backend;

    if (ce::dx12_overlay_policy::ShouldShutdownDescFreeBackendViaOverlayAdapter(backend != nullptr, adapterInitialized,
                                                                                adapterOwnsBackend)) {
        HookLogImportant(
            "DX12: Shutting down adapter-owned DescFree backend (reason=%s backend=%p adapterBackend=%p "
            "adapterType=%d shutdownMode=%d)",
            reason ? reason : "unknown", backend, adapterBackend, (int)adapterType, shutdownMode ? 1 : 0);
        if (shutdownMode) {
            g_D3D11On12Adapter.SetShutdownMode(true);
        }
        g_D3D11On12Adapter.Shutdown();
        g_DescFreeBackend = nullptr;
        return;
    }

    if (adapterInitialized) {
        HookLogImportant(
            "DX12: Shutting down DX12 overlay adapter without tracked DescFree ownership "
            "(reason=%s backend=%p adapterBackend=%p adapterType=%d shutdownMode=%d)",
            reason ? reason : "unknown", backend, adapterBackend, (int)adapterType, shutdownMode ? 1 : 0);
        if (shutdownMode) {
            g_D3D11On12Adapter.SetShutdownMode(true);
        }
        g_D3D11On12Adapter.Shutdown();
    }

    if (backend) {
        HookLogImportant("DX12: Shutting down standalone DescFree backend (reason=%s backend=%p)",
                         reason ? reason : "unknown", backend);
        backend->Shutdown();
        delete backend;
        if (g_DescFreeBackend == backend) {
            g_DescFreeBackend = nullptr;
        }
    }
}

// Lazily (re)builds the device-scoped descriptor-free overlay backend for the
// requested device/format pair. A live backend is reused as-is when both
// match (the warm path that closes the first-present blank after FG
// transitions); a device or format change is the only rebuild trigger.
// Returns true when a ready backend is bound to (device, format).
static bool EnsureDescFreeBackendForDeviceAndFormat(ID3D12Device* dev, DXGI_FORMAT format, const char* context) {
    if (!dev) {
        return g_DescFreeBackend != nullptr && g_D3D11On12Adapter.IsInitialized();
    }
    if (g_DescFreeBackend && (g_DescFreeBackendDevice != dev || g_DescFreeBackendFormat != format)) {
        HookLogImportant("DX12: DescFree backend stale (device %p->%p fmt %d->%d) — rebuilding (%s)",
                         g_DescFreeBackendDevice, dev, static_cast<int>(g_DescFreeBackendFormat),
                         static_cast<int>(format), context ? context : "unknown");
        ShutdownDescFreeBackend(context);
    }
    if (!g_DescFreeBackend) {
        auto* backend = new DX12DescFreeBackend();
        if (backend->InitDevice(dev, format)) {
            g_DescFreeBackend = backend;
            g_DescFreeBackendDevice = dev;
            g_DescFreeBackendFormat = format;
            g_D3D11On12Adapter.InitCustom(g_DescFreeBackend, OverlayBackendType::DX12);
            HookLogImportant("DX12: Descriptor-free overlay backend ready (%s, device=%p fmt=%d)",
                             context ? context : "unknown", dev, static_cast<int>(format));
        } else {
            delete backend;
            HookLogImportant("DX12: Descriptor-free backend init FAILED (%s, fmt=%d)", context ? context : "unknown",
                             static_cast<int>(format));
        }
    }
    return g_DescFreeBackend != nullptr && g_D3D11On12Adapter.IsInitialized();
}

// CRITICAL FIX: Use atomic pointers for thread-safe access
// These are read/written from multiple threads (hook thread, present thread, etc.)
std::atomic<ID3D12Device*> g_Device{nullptr};
std::atomic<ID3D12CommandQueue*> g_CommandQueue{nullptr};
    // NOLINTNEXTLINE(bugprone-throwing-static-initialization) - std::mutex-family constructors are noexcept on this toolchain
std::recursive_mutex g_CommandQueueMutex;

ID3D12CommandQueue* DX12_AcquireOriginalGameQueueForOverlay() {
    std::lock_guard<std::recursive_mutex> lock(g_CommandQueueMutex);
    ID3D12CommandQueue* queue = g_OriginalGameQueue;
    if (queue) {
        queue->AddRef();
    }
    return queue;
}

// Called by the freeze watchdog when it captures a freeze dump. If the D3D12
// device is in a removed/hung state, emit DRED auto-breadcrumbs + page-fault info
// so a device-hung freeze (e.g. the x86 DX12 focus-loss transition) is recorded
// with the exact faulting GPU op. No-op when the device is healthy or DRED is off.
void DX12_DumpDredIfDeviceRemoved(const char* reason) {
    ID3D12Device* dev = g_Device.load(std::memory_order_acquire);
    if (dev && FAILED(dev->GetDeviceRemovedReason())) {
        ce::dx12_dred::DumpOnDeviceRemoved(dev, reason ? reason : "freeze watchdog");
    }
}

// --- Overlay GPU breadcrumbs (native-FSR ffxQuery-wedge diagnosis) -------------------------------------
// A GPU-writable / CPU-readable marker buffer. CE records WriteBufferImmediate(MARKER_OUT) markers INTO the
// overlay command list around each op (start / after RT barrier / after draw / before close). When the
// FSR-FG overlay submit on AMD's runtime queue wedges AMD's ffxQuery, the freeze watchdog reads these
// markers: the highest op that reached the latest sequence value is the last GPU op CE's command list
// completed before the GPU stalled. This distinguishes "CE's GPU op stalled the queue" (markers stop
// mid-list) from "CE's list finished — the deadlock is a fence/CPU issue or AMD's own subsequent work"
// (all markers reached). Works without any device removal (the freeze is a pure hang).
enum OverlayGpuBreadcrumbOp : uint32_t {
    kOverlayBcStart = 1,       // command list reset, recording started
    kOverlayBcAfterRTBarrier,  // backbuffer transitioned to RENDER_TARGET
    kOverlayBcAfterDraw,       // overlay draw recorded
    kOverlayBcBeforeClose,     // all overlay commands recorded (about to Close)
    kOverlayBcSlotCount,
};
static ID3D12Resource* g_OverlayBcBuffer = nullptr;
static volatile uint32_t* g_OverlayBcMapped = nullptr;
static D3D12_GPU_VIRTUAL_ADDRESS g_OverlayBcGpuVA = 0;
static std::atomic<uint32_t> g_OverlayBcSeq{0};

static void EnsureOverlayBreadcrumbBuffer(ID3D12Device* device) {
    if (g_OverlayBcBuffer || !device) {
        return;
    }
    // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization) - zero-initialized placeholder; enum fields are assigned before use
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_CUSTOM;
    hp.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_WRITE_BACK;  // system memory, CPU-cached, GPU-writable
    hp.MemoryPoolPreference = D3D12_MEMORY_POOL_L0;
    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = static_cast<UINT64>(kOverlayBcSlotCount) * sizeof(uint32_t);
    rd.Height = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_UNKNOWN;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* buf = nullptr;
    HRESULT hr = device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                 IID_PPV_ARGS(&buf));
    if (FAILED(hr) || !buf) {
        static std::atomic<int> s_bcCreateFailLog{0};
        if (s_bcCreateFailLog.fetch_add(1, std::memory_order_relaxed) < 3) {
            HookLogImportant("DX12: Overlay GPU breadcrumb buffer create failed hr=0x%08X", (unsigned)hr);
        }
        return;
    }
    void* mapped = nullptr;
    if (FAILED(buf->Map(0, nullptr, &mapped)) || !mapped) {
        buf->Release();
        return;
    }
    memset(mapped, 0, static_cast<size_t>(rd.Width));
    g_OverlayBcMapped = static_cast<volatile uint32_t*>(mapped);
    g_OverlayBcGpuVA = buf->GetGPUVirtualAddress();
    g_OverlayBcBuffer = buf;
    HookLogImportant("DX12: Overlay GPU breadcrumb buffer armed (slots=%d gpuVA=0x%llX)",
                     static_cast<int>(kOverlayBcSlotCount), static_cast<unsigned long long>(g_OverlayBcGpuVA));
}

// Call once per overlay submit (before recording) to bump the sequence the GPU will stamp into each slot.
static void BeginOverlayGpuBreadcrumbFrame(ID3D12Device* device) {
    EnsureOverlayBreadcrumbBuffer(device);
    if (g_OverlayBcMapped) {
        g_OverlayBcSeq.fetch_add(1, std::memory_order_relaxed);
    }
}

static void WriteOverlayGpuBreadcrumb(ID3D12GraphicsCommandList* list, OverlayGpuBreadcrumbOp op) {
    if (!list || !g_OverlayBcMapped || g_OverlayBcGpuVA == 0 || op == 0 || op >= kOverlayBcSlotCount) {
        return;
    }
    ID3D12GraphicsCommandList2* list2 = nullptr;
    if (FAILED(list->QueryInterface(IID_PPV_ARGS(&list2))) || !list2) {
        return;
    }
    D3D12_WRITEBUFFERIMMEDIATE_PARAMETER param = {};
    param.Dest = g_OverlayBcGpuVA + static_cast<UINT64>(op) * sizeof(uint32_t);
    param.Value = g_OverlayBcSeq.load(std::memory_order_relaxed);
    D3D12_WRITEBUFFERIMMEDIATE_MODE mode = D3D12_WRITEBUFFERIMMEDIATE_MODE_MARKER_OUT;
    list2->WriteBufferImmediate(1, &param, &mode);
    list2->Release();
}

// Called from the freeze watchdog: read the markers and report the last GPU op CE's overlay list reached.
// Also dumps the FFX UI-composite fence state + timeline ring buffer (defined later in the file).
void DX12_LogFFXUiCompositeFreezeDiagnostics(const char* reason);  // forward decl — defined near UI-composite globals

void DX12_LogOverlayGpuBreadcrumbs(const char* reason) {
    if (g_OverlayBcMapped) {
        const uint32_t seq = g_OverlayBcSeq.load(std::memory_order_relaxed);
        uint32_t vals[kOverlayBcSlotCount] = {};
        for (uint32_t i = 0; i < kOverlayBcSlotCount; ++i) {
            vals[i] = g_OverlayBcMapped[i];
        }
        uint32_t lastCompletedOp = 0;
        for (uint32_t op = kOverlayBcStart; op < kOverlayBcSlotCount; ++op) {
            if (vals[op] == seq && seq != 0) {
                lastCompletedOp = op;
            }
        }
        const char* opName = "none (GPU never reached the overlay list this frame — CE's list is NOT the stall)";
        switch (lastCompletedOp) {
            case kOverlayBcStart:
                opName = "start(list reset) — GPU stalled BEFORE the RT barrier";
                break;
            case kOverlayBcAfterRTBarrier:
                opName = "after RT barrier — GPU stalled in the overlay DRAW";
                break;
            case kOverlayBcAfterDraw:
                opName = "after overlay draw — GPU stalled in the PRESENT-back barrier";
                break;
            case kOverlayBcBeforeClose:
                opName =
                    "before Close — CE's WHOLE overlay list completed; wedge is a fence/CPU deadlock or AMD's work";
                break;
            default:
                break;
        }
        HookLogImportant(
            "DX12: [overlay-gpu-breadcrumb] %s — latestSeq=%u lastCompletedOp=%u (%s) "
            "slots[start=%u rt=%u draw=%u close=%u]",
            reason ? reason : "freeze", seq, lastCompletedOp, opName, vals[kOverlayBcStart],
            vals[kOverlayBcAfterRTBarrier], vals[kOverlayBcAfterDraw], vals[kOverlayBcBeforeClose]);
    }
    // Also dump the FFX UI-composite fence state + timeline (works even if breadcrumb buffer isn't armed).
    DX12_LogFFXUiCompositeFreezeDiagnostics(reason);
}

// CRITICAL FIX: Thread-safe accessors for g_Device and g_CommandQueue
// These functions acquire the mutex and return a reference-counted pointer
// to prevent use-after-free when the queue/device is destroyed on another
// thread
struct DX12Context {
    ID3D12Device* device = nullptr;
    ID3D12CommandQueue* queue = nullptr;

    DX12Context() = default;

    DX12Context(ID3D12Device* d, ID3D12CommandQueue* q) : device(d), queue(q) {
        if (device)
            device->AddRef();
        if (queue)
            queue->AddRef();
    }

    ~DX12Context() {
        if (device) {
            device->Release();
            device = nullptr;
        }
        if (queue) {
            queue->Release();
            queue = nullptr;
        }
    }

    // Disable copy to prevent accidental double-release
    DX12Context(const DX12Context&) = delete;
    DX12Context& operator=(const DX12Context&) = delete;

    // Enable move
    DX12Context(DX12Context&& other) noexcept : device(other.device), queue(other.queue) {
        other.device = nullptr;
        other.queue = nullptr;
    }

    DX12Context& operator=(DX12Context&& other) noexcept {
        if (this != &other) {
            if (device)
                device->Release();
            if (queue)
                queue->Release();
            device = other.device;
            queue = other.queue;
            other.device = nullptr;
            other.queue = nullptr;
        }
        return *this;
    }

    bool IsValid() const {
        return device != nullptr && queue != nullptr;
    }
};

// Thread-safe accessor - ALWAYS use this instead of direct
// g_Device/g_CommandQueue access
static DX12Context GetDX12Context() {
    std::lock_guard<std::recursive_mutex> lock(g_CommandQueueMutex);
    return DX12Context(g_Device.load(), g_CommandQueue.load());
}

static DX12Context GetDX12PrerenderContext(bool preferOriginalGameQueue, bool* usesOriginalGameQueue,
                                           ID3D12CommandQueue** currentQueueSnapshot) {
    std::lock_guard<std::recursive_mutex> lock(g_CommandQueueMutex);
    ID3D12CommandQueue* currentQueue = g_CommandQueue.load(std::memory_order_acquire);
    const bool useOriginalGameQueue = preferOriginalGameQueue && g_OriginalGameQueue != nullptr;
    ID3D12CommandQueue* selectedQueue = useOriginalGameQueue ? g_OriginalGameQueue : currentQueue;
    if (usesOriginalGameQueue) {
        *usesOriginalGameQueue = useOriginalGameQueue;
    }
    if (currentQueueSnapshot) {
        *currentQueueSnapshot = currentQueue;
    }
    if (!selectedQueue) {
        return {};
    }

    // Streamline can use a second D3D12 device. Derive the fence device from
    // the selected queue instead of pairing origGame with a volatile runtime
    // device pointer.
    ID3D12Device* queueDevice = nullptr;
    const HRESULT deviceHr = selectedQueue->GetDevice(IID_PPV_ARGS(&queueDevice));
    if (FAILED(deviceHr) || !queueDevice) {
        static std::atomic<int> s_prerenderQueueDeviceLogs{0};
        if (s_prerenderQueueDeviceLogs.fetch_add(1, std::memory_order_relaxed) < 20) {
            HookLog("DX12: Prerender queue GetDevice failed queue=%p hr=0x%08X", selectedQueue, deviceHr);
        }
        return {};
    }
    DX12Context context(queueDevice, selectedQueue);
    queueDevice->Release();
    return context;
}

// Keep native driver limiters in sync with the DX12 device we already discover
// from queue/swapchain hooks. Ownership remains with the existing DX12 globals.
static void DX12_PublishNativeLimiterDevice(ID3D12Device* device, ID3D12CommandQueue* queue, const char* source) {
    if (!device)
        return;

    g_ReflexLimiter.SetDevice(static_cast<IUnknown*>(device));

    bool ctxUpdated = false;
    bool ctxApiConflict = false;
    if (auto* ctx = ce::GetHookContext()) {
        std::lock_guard<std::mutex> ctxLock(ctx->initMutex);
        if (!ctx->shuttingDown.load(std::memory_order_acquire)) {
            if (ctx->activeAPI == ce::ActiveGraphicsAPI::None) {
                ctx->activeAPI = ce::ActiveGraphicsAPI::DX12;
            }
            if (ctx->activeAPI == ce::ActiveGraphicsAPI::DX12) {
                ctx->graphicsData.dx12.device = device;
                ctx->graphicsData.dx12.commandQueue = queue;
                ctxUpdated = true;
            } else {
                ctxApiConflict = true;
            }
        }
    }

    static std::atomic<ID3D12Device*> s_lastPublishedDevice{nullptr};
    static std::atomic<ID3D12CommandQueue*> s_lastPublishedQueue{nullptr};
    static std::atomic<uint64_t> s_nativeLimiterPublishChangeCount{0};
    ID3D12Device* previousDevice = s_lastPublishedDevice.exchange(device, std::memory_order_acq_rel);
    ID3D12CommandQueue* previousQueue = s_lastPublishedQueue.exchange(queue, std::memory_order_acq_rel);
    const bool deviceChanged = previousDevice != device;
    const bool queueChanged = previousQueue != queue;
    if (deviceChanged || queueChanged) {
        const uint64_t changeCount = s_nativeLimiterPublishChangeCount.fetch_add(1, std::memory_order_relaxed) + 1;
        if (deviceChanged || changeCount <= 32 || (changeCount % 512) == 0) {
            HookLogImportant(
                "DX12: Published native limiter device from %s (device=%p queue=%p ctxUpdated=%d ctxApiConflict=%d "
                "deviceChanged=%d queueChanged=%d changeCount=%llu)",
                source && source[0] ? source : "unknown", device, queue, ctxUpdated ? 1 : 0, ctxApiConflict ? 1 : 0,
                deviceChanged ? 1 : 0, queueChanged ? 1 : 0, static_cast<unsigned long long>(changeCount));
        }
    }
}

static std::atomic<uint64_t> g_FrameIndex{0};
static std::atomic<int> g_CommandListsExecutedThisFrame{0};
static std::atomic<uint64_t> g_FGDebugFrameCount{0};
static std::atomic<int> g_AuthoritativeFSRRealFrameOnlyStreak{0};
static std::atomic<int> g_StaleRuntimeOwnedStreamlineNoFGRealFrameOnlyStreak{0};
static std::atomic<bool> g_ClearedStaleRuntimeOwnedStreamlineNoFGAfterLongOrigGameRun{false};
static std::atomic<bool> g_ExplicitNativeFSROffPendingRuntimeOwnedTeardown{false};
// Set when native FSR contexts are destroyed while no fresh session replaced
// them. Together with the explicit-off latch this is the evidence that lets a
// later GAME-created swapchain creation end the runtime-owned teardown.
static std::atomic<bool> g_NativeFSRContextsDestroyedAwaitingGameSwapchain{false};
// Identity-only marker (raw pointer, never dereferenced) for the queue of the
// game-created swapchain that ended the runtime-owned native-FSR teardown.
// Lets the FG transition cooldown and the swapchain-change reinit guard skip
// blanking the overlay for the FSR->off recovery that already proved its
// present path.
static std::atomic<ID3D12CommandQueue*> g_PostNativeFSROffGameSwapchainRecoveryQueue{nullptr};
