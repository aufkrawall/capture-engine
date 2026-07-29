    void** vtable = *(void***)pSwapChain;
    return vtable[8];  // Present is at vtable slot 8
}

// Helper to get Present1 function address from a swapchain's vtable
static void* GetPresent1Address(IDXGISwapChain* pSwapChain) {
    if (!pSwapChain)
        return nullptr;
    void** vtable = *(void***)pSwapChain;
    return vtable[22];  // Present1 is at vtable slot 22
}

// Global flag to disable DXGI hooks when Vulkan is active
// This is set once at startup and prevents DXGI hooks from interfering with
// Vulkan WSI
static bool s_vulkanPresent = false;
static bool s_checkedVulkan = false;

// Forward declaration for lazy hook installation
static void InstallHooksIfPending(IDXGISwapChain* pSwapChain);

static bool IsVulkanActive() {
    if (!s_checkedVulkan) {
        HMODULE hVulkan = GetModuleHandleW(L"vulkan-1.dll");
        s_vulkanPresent = (hVulkan != nullptr);
        if (s_vulkanPresent) {
            HookLog(
                "DXGIShared: Vulkan detected (vulkan-1.dll), DXGI hooks will "
                "pass through");
        }
        s_checkedVulkan = true;
    }
    return s_vulkanPresent;
}

static bool IsThirdPartyOverlayLoaded() {
    return ce::overlay_compat::IsThirdPartyOverlayLoaded();
}

// Unified Detours
// For DX12 wrapped swapchains: CWrapDXGISwapChain handles Present, so when
// wrapper calls m_pReal->Present() and it re-enters here, we just passthrough.
// For DX12 pre-existing swapchains (not wrapped): full processing here.
// For DX11: full processing here.
static bool IsReadableMemory(const void* ptr, size_t size) {
    if (!ptr)
        return false;
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery(ptr, &mbi, sizeof(mbi)) == 0)
        return false;
    if (mbi.State != MEM_COMMIT)
        return false;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))
        return false;
    return (mbi.Protect &
            (PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY)) != 0;
}

// Forward declarations for SL vtable hook (defined below).
HRESULT STDMETHODCALLTYPE DetourPresent(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags);
HRESULT STDMETHODCALLTYPE DetourPresent1(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags,
                                         const DXGI_PRESENT_PARAMETERS* pPresentParameters);

static bool HasExternalEntryHook(const void* target) {
    const auto* code = static_cast<const uint8_t*>(target);
    if (!IsReadableMemory(code, 16)) {
        return false;
    }
    return code[0] == 0xE9 || (code[0] == 0xFF && code[1] == 0x25);
}

// Resolves the target of an E9 (near JMP) hook at the given function address.
// Returns the absolute address of the hook handler, or nullptr if no E9 JMP
// is present or the function body is unreadable.
static void* ResolveE9JmpTarget(void* funcAddress) {
    if (!funcAddress) {
        return nullptr;
    }
    const auto* code = static_cast<const uint8_t*>(funcAddress);
    if (!IsReadableMemory(code, 5)) {
        return nullptr;
    }
    if (code[0] != 0xE9) {
        return nullptr;
    }
    int32_t relOffset;
    memcpy(&relOffset, code + 1, sizeof(relOffset));
    return static_cast<uint8_t*>(funcAddress) + 5 + relOffset;
}

static void* ResolveFF25JmpTarget(void* funcAddress) {
    if (!funcAddress) {
        return nullptr;
    }
    const auto* code = static_cast<const uint8_t*>(funcAddress);
    if (!IsReadableMemory(code, 14)) {
        return nullptr;
    }
    if (code[0] != 0xFF || code[1] != 0x25) {
        return nullptr;
    }

    int32_t dispOffset = 0;
    memcpy(&dispOffset, code + 2, sizeof(dispOffset));
    const auto* targetSlot = reinterpret_cast<void* const*>(code + 6 + dispOffset);
    if (!IsReadableMemory(targetSlot, sizeof(void*))) {
        return nullptr;
    }
    return *targetSlot;
}

static PFN_Present EnsurePresentBypassTrampoline() {
    if (oPresentBypass) {
        return oPresentBypass;
    }

    std::lock_guard<std::mutex> lock(g_SharedMutex);
    if (oPresentBypass) {
        return oPresentBypass;
    }

    const PFN_Present presentOriginal = oPresent;
    if (!presentOriginal || presentOriginal == DetourPresent || !HasExternalEntryHook((const void*)presentOriginal)) {
        return nullptr;
    }

    void* bypass = InlineHook::CreateBypassTrampoline((void*)presentOriginal);
    if (!bypass) {
        static int s_bypassFailLogCount = 0;
        if (s_bypassFailLogCount++ < 5) {
            HookLogImportant("DXGIShared: Failed to lazily create Present bypass trampoline from %p", presentOriginal);
        }
        return nullptr;
    }

    oPresentBypass = (PFN_Present)bypass;
    HookLogImportant("DXGIShared: Lazily created Present bypass trampoline at %p from %p", bypass, presentOriginal);
    return oPresentBypass;
}

static PFN_Present1 EnsurePresent1BypassTrampoline() {
    if (oPresent1Bypass) {
        return oPresent1Bypass;
    }

    std::lock_guard<std::mutex> lock(g_SharedMutex);
    if (oPresent1Bypass) {
        return oPresent1Bypass;
    }

    const PFN_Present1 present1Original = oPresent1;
    if (!present1Original || present1Original == DetourPresent1 ||
        !HasExternalEntryHook((const void*)present1Original)) {
        return nullptr;
    }

    void* bypass = InlineHook::CreateBypassTrampoline((void*)present1Original);
    if (!bypass) {
        static int s_bypassFailLogCount = 0;
        if (s_bypassFailLogCount++ < 5) {
            HookLogImportant("DXGIShared: Failed to lazily create Present1 bypass trampoline from %p",
                             present1Original);
        }
        return nullptr;
    }

    oPresent1Bypass = (PFN_Present1)bypass;
    HookLogImportant("DXGIShared: Lazily created Present1 bypass trampoline at %p from %p", bypass, present1Original);
    return oPresent1Bypass;
}

static bool TryReadSteamOverlayNullCallbackSlot(void** callbackValueOut) {
    if (!callbackValueOut) {
        return false;
    }

#ifdef _WIN64
    const wchar_t* steamModuleName = L"gameoverlayrenderer64.dll";
    const uintptr_t kSteamCallbackRva = 0x1621d8;
#else
    const wchar_t* steamModuleName = L"gameoverlayrenderer.dll";
    const uintptr_t kSteamCallbackRva = 0x1621d8;
#endif

    HMODULE steamMod = GetModuleHandleW(steamModuleName);
    if (!steamMod) {
        return false;
    }

    void** callbackSlot = reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(steamMod) + kSteamCallbackRva);
    if (!IsReadableMemory(callbackSlot, sizeof(void*))) {
        return false;
    }

    *callbackValueOut = *callbackSlot;
    return true;
}

static bool TryGetSwapChainBackBufferIndex(IDXGISwapChain* pSwapChain, UINT* indexOut) {
    if (!pSwapChain || !indexOut) {
        return false;
    }

    IDXGISwapChain3* sc3 = nullptr;
    if (FAILED(pSwapChain->QueryInterface(IID_PPV_ARGS(&sc3))) || !sc3) {
        return false;
    }

    *indexOut = sc3->GetCurrentBackBufferIndex();
    sc3->Release();
    return true;
}

static bool TryInvokeGuardedExternalSteamOverlayPresent(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags,
                                                        const char* reason, HRESULT* resultOut) {
    if (!pSwapChain || !resultOut) {
        return false;
    }

    PFN_Present externalPresent = g_externalOverlayPresentHook;
    PFN_Present presentBypass = EnsurePresentBypassTrampoline();
    const char* overlayModule = ce::overlay_compat::GetLoadedThirdPartyOverlayModuleName();
    const bool isSteamOverlay = IsSteamOverlayModule(overlayModule);
    const bool isD3D12SwapChain = DetectAPIType(pSwapChain) == APIType::D3D12;
    const bool streamlineStackActive = isD3D12SwapChain && HasStreamlineModuleInCurrentStack();
    const bool streamlinePluginLookupGuardReady = StreamlineHook::IsExternalOverlayPluginLookupGuardReady();
    const bool streamlineFGRunning = g_StreamlineFGRunning.load(std::memory_order_acquire);
    const bool postSLConfirmedRendering = isD3D12SwapChain && HookIsPostSLOverlayConfirmedRendering();
    const bool postSLConfirmedButStartupSettling = isD3D12SwapChain && HookIsPostSLOverlayConfirmedButStartupSettling();
    const bool startupTransitionWindowActive =
        isD3D12SwapChain && DXGIShared::IsStreamlineStartupTransitionWindowActive();
    void* steamCallbackBefore = nullptr;
    const bool steamCallbackReadable = TryReadSteamOverlayNullCallbackSlot(&steamCallbackBefore);
    const auto steamCallbackAddress = reinterpret_cast<uintptr_t>(steamCallbackBefore);
    const bool steamCallbackIsNull = steamCallbackReadable && steamCallbackBefore == nullptr;
    const bool steamCallbackIsCEDummy =
        steamCallbackReadable && steamCallbackBefore == reinterpret_cast<void*>(SteamDummyRenderingCallback);
    const bool steamCallbackIsInvalidLowAddress =
        steamCallbackReadable && steamCallbackBefore != nullptr && steamCallbackAddress < 0x10000;
    ScopedSteamNullCallbackRecoveryGuard steamNullCallbackGuard(
        externalPresent != nullptr && externalPresent != DetourPresent && presentBypass != nullptr && isSteamOverlay &&
            isD3D12SwapChain,
        "guarded external Present", reason, reinterpret_cast<void*>(externalPresent),
        reinterpret_cast<void*>(presentBypass), streamlineStackActive, streamlinePluginLookupGuardReady);
    const bool steamNullCallbackRecoveryReady = steamNullCallbackGuard.IsInstalled();
    const bool basePolicyAllowsGuardedSteamInvoke = DXGIShared::ShouldInvokeGuardedExternalSteamOverlayPresentForState(
        externalPresent != nullptr && externalPresent != DetourPresent, presentBypass != nullptr, isSteamOverlay,
        isD3D12SwapChain, IsInWrapperPresent(), IsWrappedSwapChainObject(pSwapChain),
        s_externalOverlayPresentInvokeDepth > 0, streamlineStackActive, streamlinePluginLookupGuardReady,
        steamNullCallbackRecoveryReady);
    const bool callbackStateAllowsGuardedSteamInvoke =
        DXGIShared::ShouldInvokeGuardedExternalSteamOverlayPresentForCallbackState(
            basePolicyAllowsGuardedSteamInvoke, steamCallbackReadable, steamCallbackIsNull, steamCallbackIsCEDummy,
            steamCallbackIsInvalidLowAddress, steamNullCallbackRecoveryReady);
    if (!callbackStateAllowsGuardedSteamInvoke) {
        if (basePolicyAllowsGuardedSteamInvoke && externalPresent && presentBypass && isSteamOverlay &&
            isD3D12SwapChain) {
            static std::atomic<int> s_guardedSteamCallbackStateSkipLogCount{0};
            const int skipNum = s_guardedSteamCallbackStateSkipLogCount.fetch_add(1, std::memory_order_relaxed) + 1;
            if (skipNum <= 20 || skipNum == 50 || (skipNum % 500) == 0) {
                HookLogImportant(
                    "DXGIShared: Skipping guarded Steam Present hook #%d for %s because Steam callback state is not "
                    "a real renderer; using bypass trampoline instead (callbackReadable=%d null=%d ceDummy=%d "
                    "lowAddress=%d callback=%p steamNullGuard=%d streamlineStack=%d pluginGuard=%d tid=0x%04X)",
                    skipNum, reason ? reason : "Present", steamCallbackReadable ? 1 : 0, steamCallbackIsNull ? 1 : 0,
                    steamCallbackIsCEDummy ? 1 : 0, steamCallbackIsInvalidLowAddress ? 1 : 0, steamCallbackBefore,
                    steamNullCallbackRecoveryReady ? 1 : 0, streamlineStackActive ? 1 : 0,
                    streamlinePluginLookupGuardReady ? 1 : 0, GetCurrentThreadId());
            }
        }
        if (externalPresent && presentBypass && isSteamOverlay && isD3D12SwapChain && streamlineStackActive) {
            static std::atomic<int> s_guardedSteamStreamlineStackSkipLogCount{0};
            const int skipNum = s_guardedSteamStreamlineStackSkipLogCount.fetch_add(1, std::memory_order_relaxed) + 1;
            if (skipNum <= 20 || skipNum == 50 || (skipNum % 500) == 0) {
                HookLogImportant(
                    "DXGIShared: Skipping guarded Steam Present hook #%d for %s because current stack is inside "
                    "Streamline startup/FG routing and required guard state is not ready; using bypass trampoline "
                    "instead (slFG=%d confirmed=%d settling=%d startupWindow=%d pluginGuard=%d invokeDepth=%d "
                    "steamNullGuard=%d steamCallbackReadable=%d steamCallback=%p tid=0x%04X)",
                    skipNum, reason ? reason : "Present", streamlineFGRunning ? 1 : 0, postSLConfirmedRendering ? 1 : 0,
                    postSLConfirmedButStartupSettling ? 1 : 0, startupTransitionWindowActive ? 1 : 0,
                    streamlinePluginLookupGuardReady ? 1 : 0, s_externalOverlayPresentInvokeDepth,
                    steamNullCallbackRecoveryReady ? 1 : 0, steamCallbackReadable ? 1 : 0, steamCallbackBefore,
                    GetCurrentThreadId());
            }
        }
        return false;
    }

    static std::atomic<int> s_guardedSteamInvokeLogCount{0};
    const int invokeNum = s_guardedSteamInvokeLogCount.fetch_add(1, std::memory_order_relaxed) + 1;
    if (invokeNum <= 20 || invokeNum == 50 || (invokeNum % 500) == 0) {
        HookLogImportant(
            "DXGIShared: Invoking guarded Steam Present hook #%d for %s "
            "(hook=%p bypass=%p slLoaded=%d streamlineFG=%d streamlineStack=%d pluginGuard=%d "
            "steamNullGuard=%d steamCallbackReadable=%d steamCallback=%p tid=0x%04X)",
            invokeNum, reason ? reason : "Present", (void*)externalPresent, (void*)presentBypass,
            ce::overlay_compat::IsStreamlineInterposerModuleLoaded() ? 1 : 0, streamlineFGRunning ? 1 : 0,
            streamlineStackActive ? 1 : 0, streamlinePluginLookupGuardReady ? 1 : 0,
            steamNullCallbackRecoveryReady ? 1 : 0, steamCallbackReadable ? 1 : 0, steamCallbackBefore,
            GetCurrentThreadId());
    }

    ++s_externalOverlayPresentInvokeDepth;
    auto depthGuard = ce::make_scope_guard([]() {
        if (s_externalOverlayPresentInvokeDepth > 0) {
            --s_externalOverlayPresentInvokeDepth;
        }
    });
    StreamlineHook::ExternalOverlayPresentGuard slGuard;

    UINT bbIdxBefore = UINT_MAX;
    UINT bbIdxAfter = UINT_MAX;
    const bool bbIdxBeforeMeasured = TryGetSwapChainBackBufferIndex(pSwapChain, &bbIdxBefore);
    const HRESULT hr = externalPresent(pSwapChain, SyncInterval, Flags);
    const bool bbIdxAfterMeasured = TryGetSwapChainBackBufferIndex(pSwapChain, &bbIdxAfter);
    const bool bbIdxMeasured = bbIdxBeforeMeasured && bbIdxAfterMeasured;
    const bool bbIdxAdvanced = bbIdxMeasured && bbIdxAfter != bbIdxBefore;
    if (FAILED(hr)) {
        static std::atomic<int> s_guardedSteamFailureLogCount{0};
        const int failureNum = s_guardedSteamFailureLogCount.fetch_add(1, std::memory_order_relaxed) + 1;
        if (failureNum <= 10 || (failureNum % 100) == 0) {
            HookLogImportant("DXGIShared: Guarded Steam Present hook failed for %s hr=0x%08X (failure #%d)",
                             reason ? reason : "Present", (unsigned)hr, failureNum);
        }
    }

    if (DXGIShared::ShouldFallbackGuardedExternalSteamOverlayPresentForResult(presentBypass != nullptr, hr,
                                                                              bbIdxMeasured, bbIdxAdvanced)) {
        static std::atomic<int> s_guardedSteamBypassFallbackLogCount{0};
        const int fallbackNum = s_guardedSteamBypassFallbackLogCount.fetch_add(1, std::memory_order_relaxed) + 1;
        if (fallbackNum <= 20 || fallbackNum == 50 || (fallbackNum % 500) == 0) {
            HookLogImportant(
                "DXGIShared: Guarded Steam Present hook fallback #%d for %s via bypass=%p "
                "(hr=0x%08X bbMeasured=%d bbIdx=%u->%u steamNullGuard=%d tid=0x%04X)",
                fallbackNum, reason ? reason : "Present", (void*)presentBypass, (unsigned)hr, bbIdxMeasured ? 1 : 0,
                bbIdxBefore, bbIdxAfter, steamNullCallbackRecoveryReady ? 1 : 0, GetCurrentThreadId());
        }
        *resultOut = presentBypass(pSwapChain, SyncInterval, Flags);
        return true;
    }

    *resultOut = hr;
    return true;
}

namespace {

static bool IsSLInterposerLoaded();

// Streamline FG routing state.
//
// Problem: When SL hooks Present with an E9 JMP at the function entry, our
// inline hook trampoline (oPresentTrampoline) bypasses SL's hook entirely,
// because the trampoline contains the ORIGINAL function bytes (from before
// any hooks).  With SL bypassed, Frame Generation never runs.
//
// Solution: Detect SL's E9 JMP on the Present function and route through it
// instead of through the trampoline.  This way:
//   Game → vtable[8] (DetourPresent) → overlay render →
//   oPresent (has SL E9 JMP) → SL_Detour → SL trampoline (has our FF 25) →
//   DetourPresent (re-entrant, forwarded to oPresentTrampoline) →
//   real Present → SL post-Present FG → return
//
// The vtable already points to DetourPresent (from inline hook install).
// We just need to change the FINAL call from oPresentTrampoline to oPresent.
static std::atomic<bool> s_slRoutingActive{false};

static bool IsSLInterposerLoaded() {
    // Latch once SL is seen (SL routing decisions assume SL stays present for the session).
    // The presence check is LOADER-FREE (cached loaded-set maintained by the seed + DLL
    // load/unload notifications); the old per-call GetModuleHandleA("sl.interposer.dll") was a
    // per-Present loader walk that stalled the present thread during the Alt+Tab mode-switch
    // DLL churn. SL loads via LoadLibrary/LdrLoadDll, which feed the cache, so detection timing
    // is equivalent.
    static std::atomic<bool> detected{false};
    if (detected.load(std::memory_order_acquire))
        return true;
    if (ce::overlay_compat::IsStreamlineInterposerModuleLoaded()) {
        detected.store(true, std::memory_order_release);
        return true;
    }
    return false;
}

static bool ShouldKeepSLPresentRoutingDisabledNow(ce::fg_runtime::RuntimeMode* runtimeModeOut = nullptr,
                                                  bool* runtimeOwnedNativeFGPresentPathOut = nullptr) {
    const auto runtimeMode = g_FGCompat.GetRuntimeMode();
    const bool runtimeOwnedNativeFGPresentPath = HookHasRuntimeOwnedNativeFGPresentPath();
    if (runtimeModeOut) {
        *runtimeModeOut = runtimeMode;
    }
    if (runtimeOwnedNativeFGPresentPathOut) {
        *runtimeOwnedNativeFGPresentPathOut = runtimeOwnedNativeFGPresentPath;
    }
    return DXGIShared::ShouldKeepSLPresentRoutingDisabledForRuntimeState(runtimeMode, runtimeOwnedNativeFGPresentPath);
}

// Detect if SL has hooked the Present function with an E9 JMP or FF 25
// indirect JMP.  If so, set up routing so our final Present call goes
// through SL's hook chain instead of bypassing it via the trampoline.
static void DetectSLPresentHook() {
    if (s_slRoutingActive.load(std::memory_order_acquire))
        return;
    if (!oPresent || !oPresentTrampoline) {
        // Vtable hook path (externally hooked Present): oPresentTrampoline is
        // NULL because we use vtable hooking instead of inline hooking when an
        // external E9 JMP (e.g. Steam overlay) is detected on dxgi!Present.
        // In this path, oPresent is the vtable's Present entry (whose dxgi.dll
        // bytes may be owned by Steam/SL), so SL routing detection via E9 JMP on
        // oPresent bytes does not apply here.  CE uses guarded Steam-overlay
        // invocations for the bypass-only paths and otherwise lets normal
        // vtable routing decide the live Present chain.
        // Log once so post-mortem analysis can distinguish the vtable path from
        // a missing inline hook bug.
        static std::atomic<uint32_t> s_vtablePathLogCount{0};
        if (s_vtablePathLogCount.fetch_add(1, std::memory_order_relaxed) < 3) {
            HookLogImportant(
                "DetectSLPresentHook: Skipping — vtable hook path (oPresent=%p, oPresentTrampoline=NULL). "
                "SL routing detection is not applicable on the external-overlay vtable path.",
                oPresent);
        }
        return;
    }

    // If oPresent is our own trampoline, SL hasn't hooked the vtable yet.
    // The vtable repair code sets oPresent to SL's hook when detected.
    if (oPresent == oPresentTrampoline)
        return;

    auto* funcBytes = (const uint8_t*)oPresent;
    if (!IsReadableMemory(funcBytes, 16))
        return;

    // Rate-limit diagnostic logging to avoid per-frame spam.
    static int s_checkCount = 0;
    int checkNum = ++s_checkCount;
    if (checkNum <= 5 || (checkNum <= 50 && (checkNum % 10) == 0) || (checkNum % 500) == 0) {
        HookLogImportant("DetectSLPresentHook: oPresent=%p bytes: %02X %02X %02X %02X %02X %02X (check #%d)", oPresent,
                         funcBytes[0], funcBytes[1], funcBytes[2], funcBytes[3], funcBytes[4], funcBytes[5], checkNum);
    }

    // Detect SL hooks: E9 relative JMP or FF 25 indirect JMP (JMP [RIP+0]).
    // SL may use either pattern depending on version and game.
    bool isE9 = (funcBytes[0] == 0xE9);
    bool isFF25 = (funcBytes[0] == 0xFF && funcBytes[1] == 0x25);

    if (!isE9 && !isFF25) {
        return;
    }

    void* hookTarget = isE9 ? ResolveE9JmpTarget((void*)oPresent) : ResolveFF25JmpTarget((void*)oPresent);
    char hookTargetModulePath[MAX_PATH] = {};
    HMODULE hookTargetModule = nullptr;
    const bool hookTargetResolved =
        hookTarget && TryGetModulePathFromCodeAddress(hookTarget, hookTargetModulePath, sizeof(hookTargetModulePath),
                                                      &hookTargetModule);
    const bool hookTargetFromStreamline = hookTargetResolved && IsStreamlineModuleHandle(hookTargetModule);
    const bool hookTargetFromCaptureHook = hookTargetResolved && IsCaptureHookModulePath(hookTargetModulePath);
    if (!DXGIShared::ShouldActivateStreamlinePresentRoutingForHookTarget(
            true, hookTargetResolved, hookTargetFromStreamline, hookTargetFromCaptureHook)) {
        static std::atomic<uint32_t> s_rejectedHookTargetLogCount{0};
        const uint32_t rejectedLogCount = s_rejectedHookTargetLogCount.fetch_add(1, std::memory_order_relaxed) + 1;
        if (rejectedLogCount <= 20 || (rejectedLogCount % 500) == 0) {
            HookLogImportant(
                "DetectSLPresentHook: %s JMP at oPresent=%p rejected as non-Streamline target "
                "(target=%p resolved=%d module=%s captureHook=%d streamline=%d log=%u)",
                isE9 ? "E9" : "FF25", oPresent, hookTarget, hookTargetResolved ? 1 : 0,
                hookTargetModulePath[0] ? hookTargetModulePath : "unknown", hookTargetFromCaptureHook ? 1 : 0,
                hookTargetFromStreamline ? 1 : 0, rejectedLogCount);
        }
        return;
    }

    // Verify that our trampoline is different (it should have the original
    // function bytes, not a JMP).
    auto* trampolineBytes = (const uint8_t*)oPresentTrampoline;
    static std::atomic<uint32_t> s_trampolineBytesLogCount{0};
    const uint32_t trampolineLogCount = s_trampolineBytesLogCount.fetch_add(1, std::memory_order_relaxed) + 1;
    if (trampolineLogCount <= 5 || (trampolineLogCount % 500) == 0) {
        HookLogImportant("DetectSLPresentHook: trampoline=%p bytes: %02X %02X %02X %02X %02X %02X (trampolineLog=%u)",
                         oPresentTrampoline, trampolineBytes[0], trampolineBytes[1], trampolineBytes[2],
                         trampolineBytes[3], trampolineBytes[4], trampolineBytes[5], trampolineLogCount);
    }

    ce::fg_runtime::RuntimeMode runtimeMode = ce::fg_runtime::RuntimeMode::kOff;
    bool runtimeOwnedNativeFGPresentPath = false;

    // Don't re-enable SL routing while the native/runtime-owned FSR path still
    // owns presentation. GTA showed that the old runtime=FSR_FG-only guard was
    // too narrow: during the explicit native-FSR OFF teardown window, the FFX
    // runtime can still own presentation even though ffxConfigure briefly
    // publishes Off. Re-attaching Streamline's Present hook chain in that window
    // reintroduces mixed-runtime routing on the native FSR path.
    if (ShouldKeepSLPresentRoutingDisabledNow(&runtimeMode, &runtimeOwnedNativeFGPresentPath)) {
        static int s_suppressedCount = 0;
        int suppressedNum = ++s_suppressedCount;
        if (suppressedNum <= 5 || (suppressedNum % 500) == 0) {
            HookLogImportant(
                "DetectSLPresentHook: SL %s JMP detected at oPresent=%p but SL routing NOT re-enabled "
                "(native FG path owns Present routing, suppressed #%d, runtime=%s "
                "runtimeOwnedNativeFG=%d)",
                isE9 ? "E9" : "FF25", oPresent, suppressedNum, ce::fg_runtime::GetRuntimeModeName(runtimeMode),
                runtimeOwnedNativeFGPresentPath ? 1 : 0);
        }
        return;
    }

    s_slRoutingActive.store(true, std::memory_order_release);
    HookLogImportant(
        "SL routing ACTIVE: Present calls will go through oPresent=%p "
        "(%s JMP target=%p module=%s) instead of trampoline=%p.  SL FG chain will execute.",
        oPresent, isE9 ? "E9" : "FF25", hookTarget, hookTargetModulePath[0] ? hookTargetModulePath : "unknown",
        oPresentTrampoline);
}

static void UpdateDXGIPresentMetricsAndPublish(bool isFirstHook, const char* publicationSource) {
    if (!isFirstHook) {
        return;
    }

    ce::fg_session::EmitFGEvent(ce::fg_session::FGEventKind::kPresentObserved, publicationSource);

    static int64_t qpcFreq = 0;
    if (qpcFreq == 0) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        qpcFreq = f.QuadPart;
    }

    LARGE_INTEGER qpc;
    QueryPerformanceCounter(&qpc);
    const int64_t us = (qpc.QuadPart * 1000000) / qpcFreq;
    g_DXGIPerfMetrics.Update(us);
    const ce::fg_session::FGActionPlan plan = ce::fg_session::GetLatestFGActionPlan();
    ce::overlay_metrics::PublishOverlayFGMetrics(&g_DXGIPerfMetrics, plan, g_FGCompat.GetOutputFPS(),
                                                 g_FGCompat.GetBaseFPS(), g_FGCompat.GetFGMultiplier(),
                                                 publicationSource);
}

static void RefreshLivePresentHooksForSwapchainIfNeeded(IDXGISwapChain* pSwapChain, const char* source) {
    if (!pSwapChain || !IsReadableMemory(pSwapChain, sizeof(void*))) {
        return;
    }

    void** vtable = *(void***)pSwapChain;
    const bool hasReadableVtable = vtable && IsReadableMemory(vtable, 23 * sizeof(void*));
    const bool trackedVtableMatchesCurrent = hasReadableVtable && s_hookedVTable == vtable;
    const bool presentHookInstalled = hasReadableVtable && vtable[8] == (void*)DetourPresent;
    const bool present1HookInstalled = hasReadableVtable && vtable[22] == (void*)DetourPresent1;

    if (!DXGIShared::ShouldRefreshLivePresentHooksForSwapchainPath(hasReadableVtable, trackedVtableMatchesCurrent,
                                                                   presentHookInstalled, present1HookInstalled)) {
        return;
    }

    HookLogImportant(
        "DXGIShared: Refreshing live Present hook path via %s swapchain %p (oldVtable=%p newVtable=%p hooked8=%d "
        "hooked22=%d)",
        source ? source : "runtime", pSwapChain, s_hookedVTable, vtable, presentHookInstalled ? 1 : 0,
        present1HookInstalled ? 1 : 0);

    InstallHooks(pSwapChain, true);
    RepairVTableHooksIfNeeded();

    if (IsSLInterposerLoaded() && !ce::fg_runtime::RuntimeModeUsesFSR(g_FGCompat.GetRuntimeMode())) {
        DetectSLPresentHook();
    }
}
}  // namespace

// Opt-in kill-switch for the Round-4 RTSS-style eager overlay draw during a runtime DLSS-FG
// toggle-ON. Default OFF; enable with CE_DLSS_TOGGLE_OVERLAY_EAGER=1 (Steam launch options or a
// system env var). Evaluated once and cached.
bool IsDlssToggleEagerOverlayEnabled() {
    static const bool enabled = []() {
        char b[8] = {};
        DWORD n = GetEnvironmentVariableA("CE_DLSS_TOGGLE_OVERLAY_EAGER", b, sizeof(b));
        return n > 0 && n < sizeof(b) && (b[0] == '1' || b[0] == 'y' || b[0] == 'Y' || b[0] == 't' || b[0] == 'T');
    }();
    return enabled;
}

// RTSS-style: draw the overlay present-time before a Streamline-startup bypass present so the
// toggle-on / DLSS-G-init frozen frame carries the overlay. Opt-in + gated; HandleDX12ProcessFrame
// resolves the submit queue and does the same-queue safety check internally (see the pre-SL un-gate
// in dx12_hook.cpp). Gated so steady-state FG and the round-1..3 wins are untouched: D3D12 only,
// DLSS FG turning on, PostSL not yet confirmed (once PostSL owns the overlay this bypass path is not
// taken), and pure DLSS (no FSR history).
static void MaybeEagerDrawOverlayBeforeStreamlineStartupBypass(IDXGISwapChain* pSwapChain, bool isD3D12,
                                                               bool streamlineFGRunning, bool postSLConfirmedRendering,
                                                               bool hadFSRFGPhase, const char* site) {
    if (!IsDlssToggleEagerOverlayEnabled() || !isD3D12 || !streamlineFGRunning || postSLConfirmedRendering ||
        hadFSRFGPhase)
        return;
    static std::atomic<int> s_log{0};
    const int n = s_log.fetch_add(1, std::memory_order_relaxed);
    if (n < 10 || (n % 120) == 0)
        HookLogImportant(
            "DX12: Eager present-time overlay draw before Streamline-startup bypass (RTSS-style, site=%s sc=%p)", site,
            (void*)pSwapChain);
    HandleDX12ProcessFrame(pSwapChain, false, true);
}

HRESULT STDMETHODCALLTYPE DetourPresent(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags) {
    g_PresentCallCounter.fetch_add(1, std::memory_order_relaxed);

    // DIAGNOSTIC: time the WHOLE DetourPresent call. The ECL diagnostic proved the Alt+Tab
    // freeze stall is NOT in ExecuteCommandLists, so it is in the present path. With the
    // ProcessFrame (overlay) and overlay-completion-wait phase timers, a slow total here with
    // NO matching slow ProcessFrame/wait log means the stall is the real Present call blocking
    // on the hung GPU (the iflip<->composited mode-switch GPU TDR). Compare 32-bit vs 64-bit.
    LARGE_INTEGER diagPresentT0;
    QueryPerformanceCounter(&diagPresentT0);
    auto diagPresentTimer = ce::make_scope_guard([&]() {
        LARGE_INTEGER diagPresentT1, diagPresentFreq;
        QueryPerformanceCounter(&diagPresentT1);
        QueryPerformanceFrequency(&diagPresentFreq);
        const double diagPresentMs =
            (double)(diagPresentT1.QuadPart - diagPresentT0.QuadPart) * 1000.0 / (double)diagPresentFreq.QuadPart;
        if (diagPresentMs >= 20.0) {
            static std::atomic<int> s_diagPresentLog{0};
            const int n = s_diagPresentLog.fetch_add(1, std::memory_order_relaxed);
            if (n < 200 || (n % 50) == 0) {
                HookLogImportant("DX12 DIAG: DetourPresent TOTAL SLOW %.1fms (tid=0x%04X)", diagPresentMs,
                                 GetCurrentThreadId());
            }
        }
    });

    // CRITICAL: Recursion guard using thread-local depth counter.
    // When using vtable hooking with Steam overlay, Steam's trampoline can call
    // back into DetourPresent via early-return paths (wrapped swapchain, shutdown, etc.)
    // before reaching the normal IsRecursivePresent() check at line ~530.
    // This caused stack overflow crashes (0xC00000FD) with ~500 recursive frames.
    static thread_local int s_presentRecurseDepth = 0;
    const bool wrappedSwapchain = IsWrappedSwapChainObject(pSwapChain);
    const bool inWrapperPresent = IsInWrapperPresent();
    const bool streamlineFGRunning = g_StreamlineFGRunning.load(std::memory_order_acquire);
    const bool isReentrant =
        (s_presentRecurseDepth > 0) && DXGIShared::ShouldTreatEarlyPresentRecursionAsForwardable(
                                           oPresentTrampoline != nullptr, oPresentBypass != nullptr, inWrapperPresent,
                                           wrappedSwapchain, streamlineFGRunning);
    s_presentRecurseDepth++;
    auto depthGuard = ce::make_scope_guard([]() { s_presentRecurseDepth--; });

    if (isReentrant) {
        // Re-entrant call - forward directly to bypass or return S_OK
        if (oPresentTrampoline) {
            return oPresentTrampoline(pSwapChain, SyncInterval, Flags);
