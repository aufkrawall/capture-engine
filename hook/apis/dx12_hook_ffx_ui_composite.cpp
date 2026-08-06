#include "dx12_hook_internal.h"
#include "dx12_hook_ffx_shared.h"

DXGI_FORMAT FFXUiCompositeRtvFormat(DXGI_FORMAT texFormat);  // forward decl — defined below

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

bool DX12_CompositeOverlayOntoFFXUiResource(void* uiResourcePtr, uint32_t ffxState, uint32_t flags) {
    if (!uiResourcePtr) {
        return false;
    }
    auto* uiTexture = static_cast<ID3D12Resource*>(uiResourcePtr);

    ID3D12CommandQueue* gameQueue = nullptr;
    {
        std::lock_guard<std::recursive_mutex> qlock(g_CommandQueueMutex);
        gameQueue = dx12_hook_g_OriginalGameQueue;  // used for node-mask + overlay-adapter init, NOT for submit
    }
    ID3D12Device* device = g_Device.load(std::memory_order_acquire);
    if (!gameQueue || !device) {

        return false;
    }

    std::lock_guard<std::recursive_mutex> lock(dx12_hook_g_FFXUiCompositeMutex);

    const D3D12_RESOURCE_DESC td = uiTexture->GetDesc();
    if (td.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || td.Width == 0 || td.Height == 0) {
        return false;
    }
    // DEGENERATE-TARGET GUARD: never draw the overlay onto a placeholder UI texture (GTA registers a 1x1).
    // DX12_PrepareFFXUiOverlayTarget normally substitutes CE's backbuffer-sized texture for a degenerate
    // game UI texture, but if that substitution was unavailable (no backbuffer geometry yet) the cached
    // target is still the 1x1 placeholder. Drawing onto a 1x1 RT is useless and trips CreateRenderTargetView/
    // Reset E_INVALIDARG (session 20260621_191028), so skip — the overlay stays blank that frame but never
    // crashes. (Same degenerate test ChooseFFXUiOverlayTarget / the old bundle used.)
    {
        const uint32_t bbW = dx12_hook_g_NoCallbackBackbufferWidth.load(std::memory_order_acquire);
        const uint32_t bbH = dx12_hook_g_NoCallbackBackbufferHeight.load(std::memory_order_acquire);
        const bool degenerate =
            td.Width <= 1 || td.Height <= 1 ||
            (bbW > 0 && bbH > 0 &&
             (static_cast<uint32_t>(td.Width) * 2u < bbW || static_cast<uint32_t>(td.Height) * 2u < bbH));
        if (degenerate) {
            static std::atomic<int> s_degenerateSkipLog{0};
            const int n = s_degenerateSkipLog.fetch_add(1, std::memory_order_relaxed);
            if (n < 20 || (n % 600) == 0) {
                HookLogImportant(
                    "DX12: FFX UI-composite SKIPPED degenerate target %p (%llux%u bb=%ux%u) — no substitute "
                    "available yet; overlay blank-but-safe this frame (log=%d)",
                    (void*)uiTexture, static_cast<unsigned long long>(td.Width), td.Height, bbW, bbH, n + 1);
            }
            return false;
        }
    }
    const int width = static_cast<int>(td.Width);
    const int height = static_cast<int>(td.Height);
    const DXGI_FORMAT rtvFormat = FFXUiCompositeRtvFormat(td.Format);
    // CE's substitute texture is CE-owned and otherwise empty, so it must be cleared to transparent each
    // frame (only the overlay composites over the game frame). The game's own usable UI texture is never
    // cleared (the overlay blends on top of the HUD already present there). Mirrors the retired bundle.
    const bool needsTransparentClear = g_BundleTargetNeedsTransparentClear.load(std::memory_order_acquire);

    if (g_FFXUiCompositeDevice == nullptr) {
        // FIRST-TIME init on this device: just record it. This is NOT a device change, so it must NOT call
        // ReleaseFFXUiCompositeInfra — that clears g_CachedFFXUiTexture, which Hooked_ffxConfigure just
        // populated and (after the one-shot ffxConfigure VEH disarms) may never repopulate. Session
        // 20260624_001619: the spurious first-call teardown nulled the cache, so the wrapper saw a null cache
        // on every subsequent present and the overlay composited exactly ONE frame then went blank.
        g_FFXUiCompositeDevice = device;
    } else if (g_FFXUiCompositeDevice != device) {
        // GENUINE device change: the infra + cache + substitute all belong to the now-dead device. Tear them
        // down and bail this frame — the passed uiTexture is from the old device. The next present rebuilds once
        // a UI texture is re-registered on the new device (or stays blank-but-safe if the VEH already disarmed).
        ReleaseFFXUiCompositeInfra();
        if (dx12_hook_g_FFXPresentOverlayAdapter.IsInitialized()) {
            dx12_hook_g_FFXPresentOverlayAdapter.Shutdown();
        }
        dx12_hook_g_FFXPresentOverlayDevice = nullptr;
        dx12_hook_g_FFXPresentOverlayFormat = DXGI_FORMAT_UNKNOWN;
        g_FFXUiCompositeAdapterFormat = DXGI_FORMAT_UNKNOWN;
        g_FFXUiCompositeDevice = device;
        return false;
    }

    // Lazily build the dedicated CE-queue submission infra (separate from the present-thread overlay and
    // separate from g_State.overlayQueue whose lifecycle is tied to FG state transitions).
    if (!dx12_hook_g_FFXUiCompositeFence) {
        if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&dx12_hook_g_FFXUiCompositeFence)))) {
            return false;
        }
        if (!g_FFXUiCompositeFenceEvent) {
            g_FFXUiCompositeFenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        }
        if (!g_FFXUiCompositeFenceEvent) {
            HookLogImportant("DX12: FFX UI-composite failed to create completion event gle=%lu", GetLastError());
            return false;
        }
    }
    // Step 2 revised: create CE's own dedicated DIRECT queue for the UI-composite submit. AMD does NOT track
    // this queue, so submitting here + signaling the fence here does not perturb AMD's pacing. The UI texture
    // is a game-owned committed resource (not a swapchain backbuffer), so cross-queue writes are legal.
    if (!dx12_hook_g_FFXUiCompositeQueue) {
        D3D12_COMMAND_QUEUE_DESC qDesc = {};
        qDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        qDesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
        qDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
        if (gameQueue) {
            qDesc.NodeMask = gameQueue->GetDesc().NodeMask;
        }
        if (FAILED(device->CreateCommandQueue(&qDesc, IID_PPV_ARGS(&dx12_hook_g_FFXUiCompositeQueue)))) {
            HookLogImportant(
                "DX12: FFX UI-composite FAILED to create dedicated CE queue — refusing the known-wedging "
                "game/runtime-queue fallback");
            dx12_hook_g_FFXUiCompositeQueue = nullptr;
            return false;
        } else {
            dx12_hook_g_FFXUiCompositeQueue->SetName(L"CE_FFXUiCompositeQueue");
            HookLogImportant("DX12: FFX UI-composite dedicated CE queue created (ptr=%p gameQueue=%p)",
                             dx12_hook_g_FFXUiCompositeQueue, gameQueue);
        }
    }
    for (auto& a : g_FFXUiCompositeAlloc) {
        if (!a && FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a)))) {
            return false;
        }
    }
    if (!g_FFXUiCompositeList) {
        if (FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_FFXUiCompositeAlloc[0], nullptr,
                                             IID_PPV_ARGS(&g_FFXUiCompositeList)))) {
            return false;
        }
        g_FFXUiCompositeList->Close();
    }
    if (!g_FFXUiCompositeRtvHeap) {
        D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE,
                                                  0};
        if (FAILED(device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&g_FFXUiCompositeRtvHeap)))) {
            return false;
        }
    }

    // Reuse the dedicated FFX overlay adapter (the callback path is mutually exclusive with no-callback FSR).
    {
        std::lock_guard<std::recursive_mutex> olock(dx12_hook_g_OverlayMutex);
        if (!dx12_hook_g_FFXPresentOverlayAdapter.IsInitialized() || g_FFXUiCompositeAdapterFormat != rtvFormat) {
            if (dx12_hook_g_FFXPresentOverlayAdapter.IsInitialized()) {
                dx12_hook_g_FFXPresentOverlayAdapter.Shutdown();
            }
            dx12_hook_g_FFXPresentOverlayAdapter.SetHwnd(nullptr);
            if (!dx12_hook_g_FFXPresentOverlayAdapter.InitDX12(device, gameQueue, static_cast<int>(rtvFormat))) {
                HookLogImportant("DX12: FFX UI-composite failed to init overlay adapter (fmt=%d)",
                                 static_cast<int>(rtvFormat));
                return false;
            }
            const bool uiTargetHDR = DX12_ResolveRuntimeOwnedOverlayTargetHDRState(rtvFormat);
            dx12_hook_g_FFXPresentOverlayAdapter.SetHDR(uiTargetHDR, static_cast<int>(rtvFormat));
            dx12_hook_g_FFXPresentOverlayDevice = device;
            dx12_hook_g_FFXPresentOverlayFormat = rtvFormat;
            g_FFXUiCompositeAdapterFormat = rtvFormat;
            HookLogImportant("DX12: FFX UI-composite initialized overlay adapter (fmt=%d hdr=%d %dx%d)",
                             static_cast<int>(rtvFormat), uiTargetHDR ? 1 : 0, width, height);
        }
    }

    // Step 2 revised: submit on CE's OWN dedicated queue (g_FFXUiCompositeQueue), NOT the game queue.
    // AMD does not track CE's queue, so the ECL + fence Signal here do not perturb AMD's pacing.
    // The fence IS signaled (on CE's own queue) and we CPU-wait for completion before returning, so the
    // caller forwards RegisterUiResource only after CE's overlay write is GPU-complete — no write/read race
    // with AMD's UI-texture snapshot. The 3-slot rotation is recycled by fence value as before.
    ID3D12CommandQueue* submitQueue = dx12_hook_g_FFXUiCompositeQueue;
    if (!submitQueue) {
        return false;
    }
    const int slot = dx12_hook_g_FFXUiCompositeFrame % kFFXUiCompositeSlotCount;
    // Recycle the allocator slot by fence value (signaled on CE's own queue). Fast path: already complete.
    if (dx12_hook_g_FFXUiCompositeFence && dx12_hook_g_FFXUiCompositeFence->GetCompletedValue() < g_FFXUiCompositeAllocFenceVal[slot]) {
        HookLogImportant(
            "DX12: FFX UI-composite refused in-flight allocator reuse (slot=%d guard=%llu completed=%llu) — "
            "prior completion proof is missing; no wait/overwrite",
            slot, static_cast<unsigned long long>(g_FFXUiCompositeAllocFenceVal[slot]),
            static_cast<unsigned long long>(dx12_hook_g_FFXUiCompositeFence->GetCompletedValue()));
        return false;
    }
    const HRESULT allocResetHr = g_FFXUiCompositeAlloc[slot]->Reset();
    const HRESULT listResetHr =
        SUCCEEDED(allocResetHr) ? g_FFXUiCompositeList->Reset(g_FFXUiCompositeAlloc[slot], nullptr) : E_FAIL;
    if (FAILED(allocResetHr) || FAILED(listResetHr)) {
        static std::atomic<int> s_resetFailLog{0};
        const int n = s_resetFailLog.fetch_add(1, std::memory_order_relaxed);
        if (n < 10 || (n % 600) == 0) {
            HookLogImportant(
                "DX12: FFX UI-composite allocator/list Reset FAILED (slot=%d frame=%d allocHr=0x%08X listHr=0x%08X) — "
                "recreating command list to recover",
                slot, dx12_hook_g_FFXUiCompositeFrame, static_cast<unsigned>(allocResetHr), static_cast<unsigned>(listResetHr));
        }
        // RECOVERY: a failed list->Reset (E_INVALIDARG) means the list is stuck OPEN from a prior failed Close.
        // Release it so the lazy-init above recreates a clean closed list next frame — otherwise every subsequent
        // frame fails the same Reset and the overlay blanks permanently.
        if (FAILED(listResetHr) && g_FFXUiCompositeList) {
            g_FFXUiCompositeList->Release();
            g_FFXUiCompositeList = nullptr;
        }
        return false;
    }

    const D3D12_CPU_DESCRIPTOR_HANDLE rtv = g_FFXUiCompositeRtvHeap->GetCPUDescriptorHandleForHeapStart();
    D3D12_RENDER_TARGET_VIEW_DESC rtvDesc = {};
    rtvDesc.Format = rtvFormat;
    rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    device->CreateRenderTargetView(uiTexture, &rtvDesc, rtv);

    const D3D12_RESOURCE_STATES regState = GetDX12StateFromFFXResourceState(ffxState);
    // GPU-breadcrumb CE's UI-texture write so a freeze dump distinguishes "CE's game-queue write completed
    // (wedge is AMD's pacing/read of the shared UI texture)" from "CE's write hung on the GPU (cross-queue
    // resource-state hazard against AMD's own use of the texture)".
    BeginOverlayGpuBreadcrumbFrame(device);
    WriteOverlayGpuBreadcrumb(g_FFXUiCompositeList, kOverlayBcStart);
    TransitionResourceIfNeeded(g_FFXUiCompositeList, uiTexture, regState, D3D12_RESOURCE_STATE_RENDER_TARGET);
    WriteOverlayGpuBreadcrumb(g_FFXUiCompositeList, kOverlayBcAfterRTBarrier);
    if (needsTransparentClear) {
        // CE's substitute texture (degenerate game UI texture) — clear to transparent so ONLY the overlay
        // composites over the game frame (the game's real content shows through AMD's UI composition). The
        // game's own usable UI texture is never cleared (the overlay blends on top of the HUD already there).
        const float kTransparent[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        g_FFXUiCompositeList->ClearRenderTargetView(rtv, kTransparent, 0, nullptr);
    }
    {
        std::lock_guard<std::recursive_mutex> olock(dx12_hook_g_OverlayMutex);
        dx12_hook_g_FFXPresentOverlayAdapter.SetIPCClient(g_IPC);
        dx12_hook_g_FFXPresentOverlayAdapter.SetReserveInactiveFGSpace(false);
        if (auto* perf = DXGIShared::GetPerformanceMetrics()) {
            dx12_hook_g_FFXPresentOverlayAdapter.SetMetrics(perf);
        }
        dx12_hook_g_FFXPresentOverlayAdapter.SetGraphicsAPI("DX12");
        dx12_hook_g_FFXPresentOverlayAdapter.SetDX12RenderTarget(g_FFXUiCompositeList, reinterpret_cast<void*>(rtv.ptr));
        dx12_hook_g_FFXPresentOverlayAdapter.RenderOverlay(width, height);
    }
    WriteOverlayGpuBreadcrumb(g_FFXUiCompositeList, kOverlayBcAfterDraw);
    TransitionResourceIfNeeded(g_FFXUiCompositeList, uiTexture, D3D12_RESOURCE_STATE_RENDER_TARGET, regState);
    WriteOverlayGpuBreadcrumb(g_FFXUiCompositeList, kOverlayBcBeforeClose);
    const HRESULT closeHr = g_FFXUiCompositeList->Close();
    if (FAILED(closeHr)) {
        // A Close failure (commonly E_INVALIDARG from an invalid recording — e.g. a barrier whose StateBefore
        // does not match the UI texture's real state) leaves the list OPEN. Log it and release the list so the
        // next frame rebuilds a clean one — otherwise the next list->Reset fails forever and the overlay blanks
        // permanently. A persistent failure points at the registered UI-resource state vs the texture's real state.
        static std::atomic<int> s_compositeCloseFailLog{0};
        const int n = s_compositeCloseFailLog.fetch_add(1, std::memory_order_relaxed);
        if (n < 20 || (n % 600) == 0) {
            HookLogImportant(
                "DX12: FFX UI-composite command list Close FAILED hr=0x%08X (uiTex=%p ffxState=0x%X regState=0x%X "
                "needsClear=%d) — recreating list to recover",
                static_cast<unsigned>(closeHr), (void*)uiTexture, ffxState, static_cast<unsigned>(regState),
                needsTransparentClear ? 1 : 0);
        }
        if (g_FFXUiCompositeList) {
            g_FFXUiCompositeList->Release();
            g_FFXUiCompositeList = nullptr;
        }
        return false;
    }

    // QPC stamp at ECL submit (for the timeline ring buffer — CPU-side submit→return causality).
    LARGE_INTEGER submitQpc;
    QueryPerformanceCounter(&submitQpc);
    ID3D12CommandList* lists[] = {g_FFXUiCompositeList};
    ExecuteCommandListsPtr realECL = dx12_hook_g_RealD3D12ECL.load(std::memory_order_acquire);
    {
        ScopedCEOverlayECLSubmission ceOverlayECLGuard("ffx-ui-composite");
        if (realECL) {
            realECL(submitQueue, 1, lists);
        } else {
            submitQueue->ExecuteCommandLists(1, lists);
        }
    }
    // Step 2 revised: signal the fence on CE's OWN queue (not the game queue). AMD does not track this
    // queue, so this Signal does not perturb AMD's pacing.
    ++dx12_hook_g_FFXUiCompositeFenceVal;
    const HRESULT signalHr = submitQueue->Signal(dx12_hook_g_FFXUiCompositeFence, dx12_hook_g_FFXUiCompositeFenceVal);
    if (FAILED(signalHr)) {
        g_FFXUiCompositeAllocFenceVal[slot] = UINT64_MAX;
        HookLogImportant(
            "DX12: FFX UI-composite completion Signal FAILED (queue=%p frame=%d value=%llu hr=0x%08X) — "
            "overlay not published and allocator permanently quarantined",
            submitQueue, dx12_hook_g_FFXUiCompositeFrame, static_cast<unsigned long long>(dx12_hook_g_FFXUiCompositeFenceVal),
            static_cast<unsigned>(signalHr));
        return false;
    }
    g_FFXUiCompositeAllocFenceVal[slot] = dx12_hook_g_FFXUiCompositeFenceVal;
    ++dx12_hook_g_FFXUiCompositeFrame;
    // Step 2 revised: CPU-wait for CE's overlay write to complete on CE's own queue before returning, so
    // the caller (Hooked_ffxConfigure) forwards RegisterUiResource only after the overlay is GPU-complete.
    // This is a deterministic fence completion wait (not a sleep/poll), on the ffxConfigure thread (which is
    // already blocked calling ffxConfigure), not the game's ECL thread. Wait duration = overlay draw time (<1ms).
    uint32_t waitTimedOut = 0;
    if (g_FFXUiCompositeFenceEvent && dx12_hook_g_FFXUiCompositeFence &&
        dx12_hook_g_FFXUiCompositeFence->GetCompletedValue() < dx12_hook_g_FFXUiCompositeFenceVal) {
        const HRESULT eventHr =
            dx12_hook_g_FFXUiCompositeFence->SetEventOnCompletion(dx12_hook_g_FFXUiCompositeFenceVal, g_FFXUiCompositeFenceEvent);
        const DWORD waitResult =
            SUCCEEDED(eventHr) ? WaitForSingleObject(g_FFXUiCompositeFenceEvent, INFINITE) : WAIT_FAILED;
        if (waitResult != WAIT_OBJECT_0) {
            waitTimedOut = 1;
            HookLogImportant(
                "DX12: FFX UI-composite completion proof FAILED (frame=%d fenceVal=%llu completed=%llu "
                "setEventHr=0x%08X wait=%lu) — overlay not published/re-registered",
                dx12_hook_g_FFXUiCompositeFrame, static_cast<unsigned long long>(dx12_hook_g_FFXUiCompositeFenceVal),
                static_cast<unsigned long long>(dx12_hook_g_FFXUiCompositeFence->GetCompletedValue()),
                static_cast<unsigned>(eventHr), waitResult);
            return false;
        }
    }
    LARGE_INTEGER returnQpc;
    QueryPerformanceCounter(&returnQpc);
    g_FFXUiResourceCompositionActive.store(true, std::memory_order_release);
    g_FFXUiCompositeLastTickMs.store(GetTickCount64(), std::memory_order_release);

    // Record this composite call in the timeline ring buffer for freeze diagnosis.
    const int gameEclCount = dx12_hook_g_CommandListsExecutedThisFrame.load(std::memory_order_relaxed);
    RecordFFXUiCompositeTimelineEntry(
        {static_cast<uint64_t>(dx12_hook_g_FFXUiCompositeFrame), dx12_hook_g_FFXUiCompositeFenceVal,
         dx12_hook_g_FFXUiCompositeFence ? dx12_hook_g_FFXUiCompositeFence->GetCompletedValue() : 0,
         static_cast<uint64_t>(submitQpc.QuadPart), static_cast<uint64_t>(returnQpc.QuadPart), waitTimedOut,
         static_cast<uint32_t>(slot), static_cast<uint32_t>(gameEclCount), uiTexture, ffxState, submitQueue});

    static std::atomic<int> s_uiCompositeLogCount{0};
    const int logCount = s_uiCompositeLogCount.fetch_add(1, std::memory_order_relaxed);
    if (logCount < 20 || (logCount % 300) == 0) {
        // Heartbeat proving the composite ran AND the CE-queue fence completed (a GTA run shows this with
        // waitTimedOut=0). fenceCompleted >= fenceVal == fence Signal observed by the CPU before Present.
        HookLogImportant(
            "DX12: FFX UI-composite overlay drawn via CE queue %p onto FFX UI resource %p (frame=%d "
            "fenceVal=%llu fenceCompleted=%llu waitTimedOut=%u clear=%d regState=0x%X ffxState=0x%X flags=0x%X "
            "%dx%d fmt=%d slot=%d gameEcl=%d log=%d) — self-signaled, CPU-waited before Present",
            submitQueue, uiTexture, dx12_hook_g_FFXUiCompositeFrame, static_cast<unsigned long long>(dx12_hook_g_FFXUiCompositeFenceVal),
            static_cast<unsigned long long>(dx12_hook_g_FFXUiCompositeFence ? dx12_hook_g_FFXUiCompositeFence->GetCompletedValue() : 0),
            waitTimedOut, needsTransparentClear ? 1 : 0, static_cast<unsigned>(regState), ffxState, flags, width,
            height, static_cast<int>(rtvFormat), slot, gameEclCount, logCount + 1);
    }
    return true;
}
bool DX12_CompositeOverlayOntoCachedFFXUiResource() {
    bool composited = false;
    bool alreadyCovered = false;
    uint64_t coveredSequence = 0;
    {
        std::lock_guard<std::recursive_mutex> lock(dx12_hook_g_FFXUiCompositeMutex);
        ID3D12Resource* uiTexture = g_CachedFFXUiTexture.load(std::memory_order_acquire);
        if (uiTexture) {
            const uint64_t targetSequence = g_FFXUiCommittedPreparationSequence;
            alreadyCovered = !ce::dx12_overlay_policy::ShouldCompositeFFXPresenterFallback(
                targetSequence, g_FFXUiPresenterFallbackLastSequence);
            coveredSequence = targetSequence;
            if (!alreadyCovered) {
                const uint32_t ffxState = g_CachedFFXUiState.load(std::memory_order_acquire);
                const uint32_t flags = g_CachedFFXUiFlags.load(std::memory_order_acquire);
                composited = DX12_CompositeOverlayOntoFFXUiResource(uiTexture, ffxState, flags);
                if (composited) {
                    g_FFXUiPresenterFallbackLastSequence = targetSequence;
                }
            }
        }
    }
    // NOTE: the substitute UI-resource re-assert is deliberately NOT called here anymore. This function is
    // reachable from DetourPresent on AMD's PRESENTER thread (the real-swapchain fallback driver), and the
    // re-assert's ffxConfigure(RegisterUiResource) takes AMD's FrameInterpolationSwapchain criticalSection —
    // which AMD's Present holds on the GAME thread while spin-waiting (no timeout) on compositionFenceCPU
    // that only the presenter thread can advance. Calling it from here deadlocked GTA permanently on the
    // first FSR-FG frame (session 20260701_213656 freeze dump: presenter thread blocked in
    // RtlEnterCriticalSection under CE's DetourPresent, game thread spinning in amd_fidelityfx ffxQuery).
    // The re-assert now runs ONLY from the FFX proxy-present prework (game thread, before AMD's Present).
    if (alreadyCovered) {
        static std::atomic<int> s_presenterDuplicateSkipLogCount{0};
        const int logCount = s_presenterDuplicateSkipLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logCount < 20 || (logCount % 300) == 0) {
            HookLogImportant(
                "DX12: FFX presenter fallback skipped duplicate composite for accepted UI registration "
                "sequence=%llu (log=%d) — real/generated outputs share one post-interpolation UI input",
                static_cast<unsigned long long>(coveredSequence), logCount + 1);
        }
    }
    if (composited || alreadyCovered) {
        NoteDX12OverlayRendered(DX12OverlayRenderRoute::kFFXPresentCallback);
    }
    return composited || alreadyCovered;
}
