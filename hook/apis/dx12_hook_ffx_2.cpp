#include "dx12_hook_internal.h"
#include "dx12_hook_ffx_shared.h"

DXGI_FORMAT FFXUiCompositeRtvFormat(DXGI_FORMAT texFormat);  // forward decl — defined below

void RecordFFXUiCompositeTimelineEntry(const FFXUiCompositeTimelineEntry& entry) {
    const uint32_t idx = g_FFXUiCompositeTimelineIdx.fetch_add(1, std::memory_order_relaxed);
    g_FFXUiCompositeTimeline[idx % dx12_hook_kFFXUiCompositeTimelineSize] = entry;
}

static void DumpFFXUiCompositeTimeline(const char* reason) {
    const uint32_t idx = g_FFXUiCompositeTimelineIdx.load(std::memory_order_relaxed);
    const int count = (idx < static_cast<uint32_t>(dx12_hook_kFFXUiCompositeTimelineSize)) ? static_cast<int>(idx)
                                                                                 : dx12_hook_kFFXUiCompositeTimelineSize;
    if (count == 0) {
        HookLogImportant("DX12: [ffx-ui-composite-timeline] %s — no composite calls recorded yet", reason ?: "freeze");
        return;
    }
    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    const double freqMs = static_cast<double>(freq.QuadPart) / 1000.0;
    const uint32_t startIdx =
        (idx >= static_cast<uint32_t>(dx12_hook_kFFXUiCompositeTimelineSize)) ? (idx % dx12_hook_kFFXUiCompositeTimelineSize) : 0;
    HookLogImportant("DX12: [ffx-ui-composite-timeline] %s — %d entries (total=%u)", reason ?: "freeze", count, idx);
    for (int i = 0; i < count; ++i) {
        const uint32_t slotIdx = (startIdx + i) % dx12_hook_kFFXUiCompositeTimelineSize;
        const auto& e = g_FFXUiCompositeTimeline[slotIdx];
        const double submitToReturnMs =
            (e.returnQpc && e.submitQpc && freqMs > 0) ? static_cast<double>(e.returnQpc - e.submitQpc) / freqMs : -1.0;
        HookLogImportant(
            "  [timeline %d/%d] frame=%llu slot=%u fenceVal=%llu fenceCompleted=%llu waitTimedOut=%u "
            "gameEcl=%u submitQpc=%llu returnQpc=%llu submitToReturnMs=%.3f uiTex=%p ffxState=0x%X queue=%p",
            i + 1, count, static_cast<unsigned long long>(e.frame), e.slot, static_cast<unsigned long long>(e.fenceVal),
            static_cast<unsigned long long>(e.fenceCompleted), e.waitTimedOut, e.gameEclCount,
            static_cast<unsigned long long>(e.submitQpc), static_cast<unsigned long long>(e.returnQpc),
            submitToReturnMs, e.uiTexture, e.ffxState, e.queue);
    }
    const uint64_t lastForwardQpc = g_LastFfxConfigureForwardQpc.load(std::memory_order_relaxed);
    const uint64_t cfgFrame = g_FfxConfigureFrame.load(std::memory_order_relaxed);
    HookLogImportant("  [timeline] lastFfxConfigureForwardQpc=%llu ffxConfigureFrame=%llu",
                     static_cast<unsigned long long>(lastForwardQpc), static_cast<unsigned long long>(cfgFrame));
}

void DX12_LogFFXUiCompositeFreezeDiagnostics(const char* reason) {
    const uint64_t fenceVal = dx12_hook_g_FFXUiCompositeFenceVal;
    const uint64_t fenceCompleted = dx12_hook_g_FFXUiCompositeFence ? dx12_hook_g_FFXUiCompositeFence->GetCompletedValue() : 0;
    const int frame = dx12_hook_g_FFXUiCompositeFrame;
    const bool compositionActive = g_FFXUiResourceCompositionActive.load(std::memory_order_acquire);
    const uint64_t lastTickMs = g_FFXUiCompositeLastTickMs.load(std::memory_order_acquire);
    HookLogImportant(
        "DX12: [ffx-ui-composite-freeze-diag] %s — frame=%d fenceVal=%llu fenceCompleted=%llu "
        "fenceMatch=%d compositionActive=%d lastTickMs=%llu nowMs=%llu",
        reason ? reason : "freeze", frame, static_cast<unsigned long long>(fenceVal),
        static_cast<unsigned long long>(fenceCompleted), fenceVal == fenceCompleted ? 1 : 0, compositionActive ? 1 : 0,
        static_cast<unsigned long long>(lastTickMs), static_cast<unsigned long long>(GetTickCount64()));
    DumpFFXUiCompositeTimeline(reason);
    // Proxy-present driver + re-assert bracket state (defined later in this file / ffx_hook.cpp): shows in
    // one freeze dump whether the game-thread driver was live and whether a substitute re-assert was
    // in-flight (the historical presenter-thread deadlock signature).
    DX12_LogFFXProxyPresentHookFreezeDiagnostics(reason);
    FFXHook_LogSubstituteReRegFreezeDiagnostics(reason);
}

bool DX12_IsFFXUiResourceCompositionActive() {
    if (!g_FFXUiResourceCompositionActive.load(std::memory_order_acquire)) {
        return false;
    }
    // Recency-gated so it auto-disables once FG turns off and the per-frame UI-resource configures stop.
    const uint64_t last = g_FFXUiCompositeLastTickMs.load(std::memory_order_acquire);
    return last != 0 && (GetTickCount64() - last) < 500;
}

bool DX12_ShouldCacheFFXUiResourceForBundle() {
    return dx12_hook_g_NativeFSRInternalNoCallbackComposition.load(std::memory_order_acquire);
}

bool DX12_IsFFXUiResourceCachedForBundle() {
    return g_CachedFFXUiTexture.load(std::memory_order_acquire) != nullptr;
}

bool DX12_IsNativeFSRInternalNoCallbackCompositionActive() {
    return dx12_hook_g_NativeFSRInternalNoCallbackComposition.load(std::memory_order_acquire);
}

bool DX12_IsLiveSwapchainQueueOriginalGameQueue() {
    std::lock_guard<std::recursive_mutex> lock(g_CommandQueueMutex);
    return dx12_hook_g_SwapchainQueue != nullptr && dx12_hook_g_OriginalGameQueue != nullptr && dx12_hook_g_SwapchainQueue == dx12_hook_g_OriginalGameQueue;
}

bool DX12_IsNativeFSRFGSuspendedDisablePending() {
    return dx12_hook_g_ExplicitNativeFSROffPendingRuntimeOwnedTeardown.load(std::memory_order_acquire);
}

static void SetBundleTargetTexture(ID3D12Resource* targetTexture, uint32_t ffxState, uint32_t flags,
                                   bool needsTransparentClear, const char* targetKind) {
    if (!targetTexture) {
        return;
    }
    std::lock_guard<std::recursive_mutex> lock(dx12_hook_g_FFXUiCompositeMutex);
    // The game may rotate or release its registered UI resource immediately after ffxConfigure returns. Keep a
    // real cache reference until the next target replaces it; the prior raw pointer was a deterministic UAF.
    targetTexture->AddRef();
    ID3D12Resource* prev = g_CachedFFXUiTexture.exchange(targetTexture, std::memory_order_acq_rel);
    g_CachedFFXUiState.store(ffxState, std::memory_order_release);
    g_CachedFFXUiFlags.store(flags, std::memory_order_release);
    g_BundleTargetNeedsTransparentClear.store(needsTransparentClear, std::memory_order_release);
    g_FFXUiResourceCompositionActive.store(true, std::memory_order_release);
    g_FFXUiCompositeLastTickMs.store(GetTickCount64(), std::memory_order_release);
    static std::atomic<uint64_t> s_uiCacheUpdateCount{0};
    const uint64_t n = s_uiCacheUpdateCount.fetch_add(1, std::memory_order_relaxed);
    const bool changed = prev != targetTexture;
    if (n < 20 || changed || (n % 600) == 0) {
        HookLogImportant(
            "DX12: FFX UI-composite target %s=%p (prev=%p changed=%d state=0x%X flags=0x%X clear=%d update=%llu)",
            targetKind ? targetKind : "tex", (void*)targetTexture, (void*)prev, changed ? 1 : 0, ffxState, flags,
            needsTransparentClear ? 1 : 0, (unsigned long long)(n + 1));
    }
    if (prev) {
        prev->Release();
    }
}

bool IsResourceOwnedByDevice(ID3D12Resource* resource, ID3D12Device* expectedDevice) {
    if (!resource || !expectedDevice) {
        return false;
    }
    ID3D12Device* resourceDevice = nullptr;
    if (FAILED(resource->GetDevice(IID_PPV_ARGS(&resourceDevice))) || !resourceDevice) {
        return false;
    }
    IUnknown* resourceIdentity = nullptr;
    IUnknown* expectedIdentity = nullptr;
    const bool same = SUCCEEDED(resourceDevice->QueryInterface(IID_PPV_ARGS(&resourceIdentity))) &&
                      SUCCEEDED(expectedDevice->QueryInterface(IID_PPV_ARGS(&expectedIdentity))) &&
                      resourceIdentity == expectedIdentity;
    if (resourceIdentity) {
        resourceIdentity->Release();
    }
    if (expectedIdentity) {
        expectedIdentity->Release();
    }
    resourceDevice->Release();
    return same;
}

static ID3D12Resource* PrepareCEUiSubstituteTexture(ID3D12Device* device, uint32_t width, uint32_t height,
                                                    DXGI_FORMAT format, D3D12_RESOURCE_STATES initialState) {
    if (!device || width == 0 || height == 0 || format == DXGI_FORMAT_UNKNOWN) {
        return nullptr;
    }
    if (g_CEUiSubstituteTexture && IsResourceOwnedByDevice(g_CEUiSubstituteTexture, device) &&
        g_CEUiSubstituteWidth == width && g_CEUiSubstituteHeight == height && g_CEUiSubstituteFormat == format &&
        g_CEUiSubstituteInitialState == initialState) {
        g_CEUiSubstituteTexture->AddRef();
        return g_CEUiSubstituteTexture;
    }
    // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization) - zero-initialized placeholder; enum fields are assigned before use
    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC td = {};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = width;
    td.Height = height;
    td.DepthOrArraySize = 1;
    td.MipLevels = 1;
    td.Format = format;
    td.SampleDesc.Count = 1;
    td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_CLEAR_VALUE clearValue = {};
    clearValue.Format = FFXUiCompositeRtvFormat(format);  // typed RTV format (the bundle clears with this)
    ID3D12Resource* tex = nullptr;
    const HRESULT hr = device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &td, initialState, &clearValue,
                                                       IID_PPV_ARGS(&tex));
    if (FAILED(hr) || !tex) {
        HookLogImportant("DX12: FFX UI substitute texture creation FAILED (%ux%u fmt=%d hr=0x%08X)", width, height,
                         static_cast<int>(format), static_cast<unsigned>(hr));
        return nullptr;
    }
    tex->SetName(L"CE_FFXUiSubstituteTexture");
    HookLogImportant(
        "DX12: Prepared CE substitute UI texture %ux%u fmt=%d initState=0x%X; publication waits for successful "
        "FFX RegisterUiResource",
        width, height, static_cast<int>(format), static_cast<unsigned>(initialState));
    return tex;
}

bool DX12_PrepareFFXUiOverlayTarget(const ce::ffx_api::Resource& gameUi, uint32_t flags,
                                    ce::ffx_api::Resource* ceSubstitute,
                                    DX12FFXUiOverlayTargetPreparation* preparation) {
    auto* gameTex = static_cast<ID3D12Resource*>(gameUi.resource);
    if (!gameTex || !preparation) {
        return false;
    }
    *preparation = {};
    preparation->state = gameUi.state;
    preparation->flags = flags;
    preparation->sequence = g_FFXUiPreparationSequence.fetch_add(1, std::memory_order_acq_rel) + 1;

    auto stageGameTexture = [&]() {
        gameTex->AddRef();
        preparation->target = gameTex;
        preparation->substitute = false;
        preparation->clearTransparent = false;
    };

    // Authoritative geometry/format from the actual D3D12 resource (the FFX description can be partially filled);
    // fall back to the FFX description dims if the resource is not a usable 2D texture.
    uint32_t gameW = gameUi.description.width;
    uint32_t gameH = gameUi.description.height;
    DXGI_FORMAT gameFmt = DXGI_FORMAT_UNKNOWN;
    {
        const D3D12_RESOURCE_DESC gd = gameTex->GetDesc();
        if (gd.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && gd.Width != 0 && gd.Height != 0) {
            gameW = static_cast<uint32_t>(gd.Width);
            gameH = gd.Height;
            gameFmt = gd.Format;
        }
    }
    const uint32_t bbW = dx12_hook_g_NoCallbackBackbufferWidth.load(std::memory_order_acquire);
    const uint32_t bbH = dx12_hook_g_NoCallbackBackbufferHeight.load(std::memory_order_acquire);

    const auto target = ce::dx12_overlay_policy::ChooseFFXUiOverlayTarget(gameW, gameH, bbW, bbH);
    if (target == ce::dx12_overlay_policy::FFXUiOverlayTarget::kCompositeOntoGameTexture) {
        stageGameTexture();
        return false;
    }

    // Degenerate game UI texture (GTA's 1x1): substitute CE's own backbuffer-sized texture so AMD composites
    // the overlay. Requires known backbuffer geometry + a valid device; otherwise fall back to caching the game
    // texture (the bundle's degenerate guard then safely skips the draw — never a 1x1 garbage submit / crash).
    ID3D12Device* device = nullptr;
    gameTex->GetDevice(IID_PPV_ARGS(&device));
    const DXGI_FORMAT substituteFmt =
        (gameFmt != DXGI_FORMAT_UNKNOWN)
            ? gameFmt
            : static_cast<DXGI_FORMAT>(dx12_hook_g_NoCallbackBackbufferFormat.load(std::memory_order_acquire));
    if (!device || bbW == 0 || bbH == 0 || substituteFmt == DXGI_FORMAT_UNKNOWN || !ceSubstitute) {
        stageGameTexture();
        static std::atomic<int> s_substFallbackLog{0};
        const int n = s_substFallbackLog.fetch_add(1, std::memory_order_relaxed);
        if (n < 20 || (n % 300) == 0) {
            HookLogImportant(
                "DX12: FFX UI substitute UNAVAILABLE (device=%p bb=%ux%u fmt=%d) — degenerate game UI texture %ux%u "
                "kept as bundle target; bundle skips the draw rather than draw onto a placeholder (log=%d)",
                (void*)device, bbW, bbH, static_cast<int>(substituteFmt), gameW, gameH, n + 1);
        }
        if (device) {
            device->Release();
        }
        return false;
    }

    std::lock_guard<std::recursive_mutex> lock(dx12_hook_g_FFXUiCompositeMutex);
    const D3D12_RESOURCE_STATES initialState = GetDX12StateFromFFXResourceState(gameUi.state);
    // COMMON/PRESENT is legitimately numeric zero. Never truth-test D3D12_RESOURCE_STATES: creating in a
    // different fallback state while forwarding/caching COMMON would make the first owner-queue barrier's
    // StateBefore false and can remove the device.
    ID3D12Resource* ceTex = PrepareCEUiSubstituteTexture(device, bbW, bbH, substituteFmt, initialState);
    device->Release();
    if (!ceTex) {
        stageGameTexture();
        return false;
    }

    // Mirror the game's description/state so AMD treats CE's texture identically; override only geometry + ptr.
    *ceSubstitute = gameUi;
    ceSubstitute->resource = ceTex;
    ceSubstitute->description.width = bbW;
    ceSubstitute->description.height = bbH;

    preparation->target = ceTex;
    preparation->substitute = true;
    preparation->clearTransparent = true;
    preparation->width = bbW;
    preparation->height = bbH;
    preparation->format = substituteFmt;
    preparation->initialState = initialState;
    return true;
}

void DX12_DiscardFFXUiOverlayTarget(DX12FFXUiOverlayTargetPreparation* preparation) {
    if (!preparation) {
        return;
    }
    if (preparation->target) {
        preparation->target->Release();
    }
    *preparation = {};
}

void DX12_CommitFFXUiOverlayTarget(DX12FFXUiOverlayTargetPreparation* preparation) {
    if (!preparation || !preparation->target) {
        return;
    }

    {
        std::lock_guard<std::recursive_mutex> lock(dx12_hook_g_FFXUiCompositeMutex);
        if (preparation->sequence < g_FFXUiCommittedPreparationSequence) {
            HookLogImportant(
                "DX12: Discarding stale successful FFX UI registration commit (sequence=%llu committed=%llu)",
                static_cast<unsigned long long>(preparation->sequence),
                static_cast<unsigned long long>(g_FFXUiCommittedPreparationSequence));
        } else {
            g_FFXUiCommittedPreparationSequence = preparation->sequence;
            if (preparation->substitute && g_CEUiSubstituteTexture != preparation->target) {
                preparation->target->AddRef();
                ID3D12Resource* oldSubstitute = g_CEUiSubstituteTexture;
                g_CEUiSubstituteTexture = preparation->target;
                g_CEUiSubstituteWidth = preparation->width;
                g_CEUiSubstituteHeight = preparation->height;
                g_CEUiSubstituteFormat = preparation->format;
                g_CEUiSubstituteInitialState = preparation->initialState;
                if (oldSubstitute) {
                    oldSubstitute->Release();
                }
            }

            SetBundleTargetTexture(preparation->target, preparation->state, preparation->flags,
                                   preparation->clearTransparent,
                                   preparation->substitute ? "ce-substitute-tex" : "game-tex");
            static std::atomic<int> s_commitLog{0};
            const int n = s_commitLog.fetch_add(1, std::memory_order_relaxed);
            if (n < 20 || (n % 300) == 0) {
                HookLogImportant(
                    "DX12: Committed accepted FFX UI target %p (substitute=%d %ux%u fmt=%d state=0x%X "
                    "sequence=%llu log=%d)",
                    preparation->target, preparation->substitute ? 1 : 0, preparation->width, preparation->height,
                    static_cast<int>(preparation->format), preparation->state,
                    static_cast<unsigned long long>(preparation->sequence), n + 1);
            }
        }
    }
    DX12_DiscardFFXUiOverlayTarget(preparation);
}

void DX12_NoteFfxConfigureForward(uint64_t configureType) {
    LARGE_INTEGER qpc;
    QueryPerformanceCounter(&qpc);
    g_LastFfxConfigureForwardQpc.store(static_cast<uint64_t>(qpc.QuadPart), std::memory_order_relaxed);
    const uint64_t frame = g_FfxConfigureFrame.fetch_add(1, std::memory_order_relaxed) + 1;
    // Log RegisterUiResource (type=0x30002) calls with frame context so the FG-configure (0x20002) and
    // UI-register (0x30002) streams are correlatable per frame in the freeze dump.
    if (configureType == ce::ffx_api::kConfigureDescTypeFrameGenerationSwapChainRegisterUiResourceDX12) {
        static std::atomic<int> s_registerUiResLogCount{0};
        const int n = s_registerUiResLogCount.fetch_add(1, std::memory_order_relaxed);
        if (n < 20 || (n % 300) == 0) {
            HookLogImportant("FFX Hook: RegisterUiResource forwarded (type=0x30002 cfgFrame=%llu qpc=%llu log=%d)",
                             static_cast<unsigned long long>(frame), static_cast<unsigned long long>(qpc.QuadPart),
                             n + 1);
        }
    }
}

void ReleaseFFXUiCompositeInfra() {
    std::lock_guard<std::recursive_mutex> lock(dx12_hook_g_FFXUiCompositeMutex);
    // Invalidate any RegisterUiResource preparation that entered before teardown and has not returned from
    // AMD yet. Its later commit must not resurrect device-bound resources into the cleared generation.
    g_FFXUiCommittedPreparationSequence = g_FFXUiPreparationSequence.fetch_add(1, std::memory_order_acq_rel) + 1;
    g_FFXUiPresenterFallbackLastSequence = 0;
    if (g_FFXUiCompositeList) {
        g_FFXUiCompositeList->Release();
        g_FFXUiCompositeList = nullptr;
    }
    for (auto& a : g_FFXUiCompositeAlloc) {
        if (a) {
            a->Release();
            a = nullptr;
        }
    }
    if (g_FFXUiCompositeRtvHeap) {
        g_FFXUiCompositeRtvHeap->Release();
        g_FFXUiCompositeRtvHeap = nullptr;
    }
    if (dx12_hook_g_FFXUiCompositeQueue) {
        dx12_hook_g_FFXUiCompositeQueue->Release();
        dx12_hook_g_FFXUiCompositeQueue = nullptr;
    }
    if (dx12_hook_g_FFXUiCompositeFence) {
        dx12_hook_g_FFXUiCompositeFence->Release();
        dx12_hook_g_FFXUiCompositeFence = nullptr;
    }
    dx12_hook_g_FFXUiCompositeFenceVal = 0;
    for (int i = 0; i < kFFXUiCompositeSlotCount; ++i) {
        g_FFXUiCompositeAllocFenceVal[i] = 0;
    }
    dx12_hook_g_FFXUiCompositeFrame = 0;
    g_FFXUiCompositeTimelineIdx.store(0, std::memory_order_relaxed);
    g_LastFfxConfigureForwardQpc.store(0, std::memory_order_relaxed);
    g_FfxConfigureFrame.store(0, std::memory_order_relaxed);
    ID3D12Resource* cachedTexture = g_CachedFFXUiTexture.exchange(nullptr, std::memory_order_acq_rel);
    g_CachedFFXUiState.store(0, std::memory_order_release);
    g_CachedFFXUiFlags.store(0, std::memory_order_release);
    g_BundleTargetNeedsTransparentClear.store(false, std::memory_order_release);
    if (cachedTexture) {
        cachedTexture->Release();
    }
    // Stop the per-present substitute re-registration: the stored desc's resource pointer (CE's substitute)
    // is about to dangle. ffx_hook re-stores it on the next RegisterUiResource substitution.
    FFXHook_ClearSubstituteUiReRegistration();
    // Release CE's own substitute UI texture (degenerate-game-texture path). Released here on teardown / device
    // change only — never from the per-frame composite path (that would blank the overlay every frame).
    if (g_CEUiSubstituteTexture) {
        g_CEUiSubstituteTexture->Release();
        g_CEUiSubstituteTexture = nullptr;
    }
    g_CEUiSubstituteWidth = 0;
    g_CEUiSubstituteHeight = 0;
    g_CEUiSubstituteFormat = DXGI_FORMAT_UNKNOWN;
    g_CEUiSubstituteInitialState = D3D12_RESOURCE_STATE_COMMON;
}

DXGI_FORMAT FFXUiCompositeRtvFormat(DXGI_FORMAT texFormat) {
    switch (texFormat) {
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:
            return DXGI_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_B8G8R8A8_TYPELESS:
            return DXGI_FORMAT_B8G8R8A8_UNORM;
        case DXGI_FORMAT_R10G10B10A2_TYPELESS:
            return DXGI_FORMAT_R10G10B10A2_UNORM;
        case DXGI_FORMAT_R16G16B16A16_TYPELESS:
            return DXGI_FORMAT_R16G16B16A16_FLOAT;
        default:
            return texFormat;
    }
}
