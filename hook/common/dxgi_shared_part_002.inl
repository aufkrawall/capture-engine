#ifdef _WIN64
    if (returnAddress < steamStart + 2 || returnAddress >= steamEnd) {
        return nullptr;
    }

    const uintptr_t callAddress = returnAddress - 2;
    const auto* callBytes = reinterpret_cast<const uint8_t*>(callAddress);
    if (!IsReadableMemory(callBytes, 2) || callBytes[0] != 0xFF || callBytes[1] != 0xD0) {
        return nullptr;
    }

    const uintptr_t scanStart = (callAddress > steamStart + 64) ? callAddress - 64 : steamStart;
    for (uintptr_t instr = callAddress; instr >= scanStart; --instr) {
        const auto* bytes = reinterpret_cast<const uint8_t*>(instr);
        if (!IsReadableMemory(bytes, 7)) {
            if (instr == scanStart) {
                break;
            }
            continue;
        }

        if (bytes[0] == 0x48 && bytes[1] == 0x8B && bytes[2] == 0x05) {
            int32_t disp = 0;
            memcpy(&disp, bytes + 3, sizeof(disp));
            auto** slot = reinterpret_cast<void**>(instr + 7 + disp);
            const uintptr_t slotAddress = reinterpret_cast<uintptr_t>(slot);
            if (slotAddress >= steamStart && slotAddress + sizeof(void*) <= steamEnd &&
                IsReadableMemory(reinterpret_cast<const void*>(slot), sizeof(void*)) && *slot == nullptr) {
                return slot;
            }
        }

        if (instr == scanStart) {
            break;
        }
    }
#else
    if (returnAddress < steamStart + 2 || returnAddress >= steamEnd) {
        return nullptr;
    }

    const uintptr_t callAddress = returnAddress - 2;
    const auto* callBytes = reinterpret_cast<const uint8_t*>(callAddress);
    if (!IsReadableMemory(callBytes, 2) || callBytes[0] != 0xFF || callBytes[1] != 0xD0) {
        return nullptr;
    }

    const uintptr_t scanStart = (callAddress > steamStart + 32) ? callAddress - 32 : steamStart;
    for (uintptr_t instr = callAddress; instr >= scanStart; --instr) {
        const auto* bytes = reinterpret_cast<const uint8_t*>(instr);
        uintptr_t slotAddress = 0;
        if (IsReadableMemory(bytes, 5) && bytes[0] == 0xA1) {
            uint32_t absolute = 0;
            memcpy(&absolute, bytes + 1, sizeof(absolute));
            slotAddress = absolute;
        } else if (IsReadableMemory(bytes, 6) && bytes[0] == 0x8B && bytes[1] == 0x05) {
            uint32_t absolute = 0;
            memcpy(&absolute, bytes + 2, sizeof(absolute));
            slotAddress = absolute;
        }

        auto** slot = reinterpret_cast<void**>(slotAddress);
        if (slotAddress >= steamStart && slotAddress + sizeof(void*) <= steamEnd &&
            IsReadableMemory(slot, sizeof(void*)) && *slot == nullptr) {
            return slot;
        }

        if (instr == scanStart) {
            break;
        }
    }
#endif

    return nullptr;
}

// VEH handler: catches Steam's NULL rendering callback crash during the
// one-time init and guarded Present paths.  It resolves the exact Steam global
// slot that supplied NULL to `call (e)ax`, patches that slot to CE's DXGI
// bypass Present when possible, and retries the call so Steam can keep its own
// overlay chain alive.
//
// Architecture notes:
//   x64: returnAddr from [RSP], RIP, RAX, RSP, call rax = FF D0 (2 bytes)
//   x86: returnAddr from [ESP], EIP, EAX, ESP, call eax = FF D0 (2 bytes)
//   Steam module: x64=gameoverlayrenderer64.dll, x86=gameoverlayrenderer.dll
//   Legacy fallback RVA: x64=0x1621d8. Newer Steam builds can use nearby slots;
//   the handler first resolves the slot dynamically from the faulting mov/call.
static LONG CALLBACK SteamOverlayInitVehHandler(PEXCEPTION_POINTERS ep) {
    if (ep->ExceptionRecord->ExceptionCode != STATUS_ACCESS_VIOLATION) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

#ifdef _WIN64
    const wchar_t* steamModuleName = L"gameoverlayrenderer64.dll";
    const uintptr_t kSteamCallbackRva = 0x1621d8;
    const int kCallOpcodeSize = 2;  // FF D0 = call rax (2 bytes)
    // RIP=0, RAX=0: calling through NULL (`call rax` where RAX loaded from NULL ptr)
    if (ep->ContextRecord->Rip != 0 || ep->ContextRecord->Rax != 0) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    uintptr_t returnAddress = 0;
    if (ep->ContextRecord->Rsp) {
        returnAddress = *(uintptr_t*)ep->ContextRecord->Rsp;
    }
#else
    const wchar_t* steamModuleName = L"gameoverlayrenderer.dll";
    const uintptr_t kSteamCallbackRva = 0x1621d8;
    const int kCallOpcodeSize = 2;  // FF D0 = call eax (2 bytes)
    // EIP=0, EAX=0: calling through NULL (`call eax` where EAX loaded from NULL ptr)
    if (ep->ContextRecord->Eip != 0 || ep->ContextRecord->Eax != 0) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    uintptr_t returnAddress = 0;
    if (ep->ContextRecord->Esp) {
        returnAddress = *(uintptr_t*)ep->ContextRecord->Esp;
    }
#endif

    if (!returnAddress) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    HMODULE steamMod = GetModuleHandleW(steamModuleName);
    if (!steamMod) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    MODULEINFO modInfo = {};
    if (!GetModuleInformation(GetCurrentProcess(), steamMod, &modInfo, sizeof(modInfo))) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    uintptr_t steamStart = (uintptr_t)steamMod;
    uintptr_t steamEnd = steamStart + modInfo.SizeOfImage;
    if (returnAddress < steamStart || returnAddress >= steamEnd) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    const SteamNullCallbackRecoveryContext recoveryContext = s_steamNullCallbackRecoveryContext;
    const void* patchTarget = SelectSteamNullCallbackRecoveryTarget(recoveryContext);
    void** nullFnPtr = ResolveSteamNullCallbackSlotFromFault(returnAddress, steamStart, steamEnd);
    const bool dynamicallyResolvedSlot = nullFnPtr != nullptr;
    if (!nullFnPtr) {
        nullFnPtr = reinterpret_cast<void**>(steamStart + kSteamCallbackRva);
    }
    const uintptr_t resolvedRva = reinterpret_cast<uintptr_t>(nullFnPtr) - steamStart;
    void* callbackBefore = nullptr;
    const bool callbackSlotReadable = IsReadableMemory(reinterpret_cast<const void*>(nullFnPtr), sizeof(void*));
    if (callbackSlotReadable) {
        callbackBefore = *nullFnPtr;
    }
    bool patched = false;
    if (callbackSlotReadable && callbackBefore == nullptr) {
        DWORD oldProtect;
        if (VirtualProtect(reinterpret_cast<void*>(nullFnPtr), sizeof(void*), PAGE_READWRITE, &oldProtect)) {
            *nullFnPtr = const_cast<void*>(patchTarget);
            VirtualProtect(reinterpret_cast<void*>(nullFnPtr), sizeof(void*), oldProtect, &oldProtect);
            patched = true;
            HookLogImportant(
                "SteamOverlayInitVehHandler: Patched NULL callback at %p (steam+0x%zX) "
                "-> %s=%p context=%s reason=%s hook=%p bypass=%p dynamicSlot=%d streamlineStack=%d pluginGuard=%d",
                nullFnPtr, resolvedRva,
                patchTarget == recoveryContext.bypass ? "DXGIBypassPresent" : "SteamDummyRenderingCallback",
                patchTarget, recoveryContext.context ? recoveryContext.context : "unknown",
                recoveryContext.reason ? recoveryContext.reason : "Present", recoveryContext.hook,
                recoveryContext.bypass, dynamicallyResolvedSlot ? 1 : 0, recoveryContext.streamlineStackActive ? 1 : 0,
                recoveryContext.pluginLookupGuardReady ? 1 : 0);
        } else {
            HookLogImportant(
                "SteamOverlayInitVehHandler: VirtualProtect failed for Steam callback at %p context=%s reason=%s",
                nullFnPtr, recoveryContext.context ? recoveryContext.context : "unknown",
                recoveryContext.reason ? recoveryContext.reason : "Present");
        }
    } else {
        HookLogImportant(
            "SteamOverlayInitVehHandler: RVA 0x%zX not patchable (slot=%p readable=%d value=%p) - RVA may have "
            "changed, skipping patch and falling back to crash skip context=%s reason=%s dynamicSlot=%d",
            resolvedRva, nullFnPtr, callbackSlotReadable ? 1 : 0, callbackBefore,
            recoveryContext.context ? recoveryContext.context : "unknown",
            recoveryContext.reason ? recoveryContext.reason : "Present", dynamicallyResolvedSlot ? 1 : 0);
    }

    if (patched) {
        // Retry `call (e)ax` with our patched callback.
#ifdef _WIN64
        ep->ContextRecord->Rsp += 8;  // undo the call's stack push (8 bytes on x64)
        ep->ContextRecord->Rax = (DWORD64)patchTarget;
        ep->ContextRecord->Rip = returnAddress - kCallOpcodeSize;
        HookLog("SteamOverlayInitVehHandler: Retrying call rax with RAX=%p", (void*)ep->ContextRecord->Rax);
#else
        ep->ContextRecord->Esp += 4;  // undo the call's stack push (4 bytes on x86)
        ep->ContextRecord->Eax = (DWORD)(uintptr_t)patchTarget;
        ep->ContextRecord->Eip = returnAddress - kCallOpcodeSize;
        HookLog("SteamOverlayInitVehHandler: Retrying call eax with EAX=%p", (void*)(DWORD_PTR)ep->ContextRecord->Eax);
#endif
    } else {
        // Could not patch - skip past the crash entirely.
        // Set (R/E)IP past the call and (R/E)AX = S_OK (0) so Steam continues.
        // This may cause Steam to crash elsewhere if the callback was mandatory,
        // but at least we tried.
#ifdef _WIN64
        ep->ContextRecord->Rax = 0;  // S_OK
        ep->ContextRecord->Rip = returnAddress;
        // NOTE: RSP already points to the return address (pushed by `call rax`).
        // By setting RIP=returnAddress, we consume that pushed return address
        // as the "function returned normally". RSP is preserved correctly.
        HookLog("SteamOverlayInitVehHandler: Skipped past crash (fallback) - RIP=%p RAX=0",
                (void*)ep->ContextRecord->Rip);
#else
        ep->ContextRecord->Eax = 0;  // S_OK
        ep->ContextRecord->Eip = returnAddress;
        HookLog("SteamOverlayInitVehHandler: Skipped past crash (fallback) - EIP=%p EAX=0",
                (void*)(DWORD_PTR)ep->ContextRecord->Eip);
#endif
    }

    return EXCEPTION_CONTINUE_EXECUTION;
}

class ScopedSteamNullCallbackRecoveryGuard {
public:
    ScopedSteamNullCallbackRecoveryGuard(bool enabled, const char* context, const char* reason, void* hook,
                                         void* bypass, bool streamlineStackActive, bool pluginLookupGuardReady)
        : previousContext_(s_steamNullCallbackRecoveryContext) {
        if (!enabled) {
            return;
        }

        s_steamNullCallbackRecoveryContext = SteamNullCallbackRecoveryContext{
            context ? context : "unknown", reason, hook, bypass, streamlineStackActive, pluginLookupGuardReady,
        };
        handle_ = AddVectoredExceptionHandler(1, SteamOverlayInitVehHandler);
        if (handle_) {
            static std::atomic<int> s_guardInstallLogCount{0};
            const int logCount = s_guardInstallLogCount.fetch_add(1, std::memory_order_relaxed) + 1;
            if (logCount <= 20 || logCount == 50 || (logCount % 500) == 0) {
                HookLogImportant(
                    "Guarded Steam Present hook installed Steam null-callback VEH recovery #%d "
                    "(context=%s reason=%s hook=%p bypass=%p streamlineStack=%d pluginGuard=%d tid=0x%04X)",
                    logCount, context ? context : "unknown", reason ? reason : "Present", hook, bypass,
                    streamlineStackActive ? 1 : 0, pluginLookupGuardReady ? 1 : 0, GetCurrentThreadId());
            }
        } else {
            HookLogImportant(
                "Guarded Steam Present hook failed to install Steam null-callback VEH recovery "
                "(context=%s reason=%s hook=%p bypass=%p streamlineStack=%d pluginGuard=%d err=%lu)",
                context ? context : "unknown", reason ? reason : "Present", hook, bypass, streamlineStackActive ? 1 : 0,
                pluginLookupGuardReady ? 1 : 0, GetLastError());
        }
    }

    ~ScopedSteamNullCallbackRecoveryGuard() {
        if (handle_) {
            RemoveVectoredExceptionHandler(handle_);
        }
        s_steamNullCallbackRecoveryContext = previousContext_;
    }

    ScopedSteamNullCallbackRecoveryGuard(const ScopedSteamNullCallbackRecoveryGuard&) = delete;
    ScopedSteamNullCallbackRecoveryGuard& operator=(const ScopedSteamNullCallbackRecoveryGuard&) = delete;

    bool IsInstalled() const {
        return handle_ != nullptr;
    }

private:
    SteamNullCallbackRecoveryContext previousContext_;
    PVOID handle_ = nullptr;
};

// Forward declaration — defined later in this translation unit.
static PFN_Present EnsurePresentBypassTrampoline();

enum class DX12StartupPresentMode {
    kNone,
    kPassThroughOriginal,
};

static bool IsSteamOverlayModule(const char* overlayModule) {
    return overlayModule && ce::overlay_compat::detail::ContainsInsensitive(overlayModule, "gameoverlayrenderer");
}

static bool IsStreamlineModuleHandle(HMODULE moduleHandle) {
    if (!moduleHandle) {
        return false;
    }

    char modulePath[MAX_PATH] = {};
    if (GetModuleFileNameA(moduleHandle, modulePath, MAX_PATH) == 0) {
        return false;
    }

    return ce::overlay_compat::detail::ContainsInsensitive(modulePath, "sl.interposer") ||
           ce::overlay_compat::detail::ContainsInsensitive(modulePath, "sl.common") ||
           ce::overlay_compat::detail::ContainsInsensitive(modulePath, "sl.dlss_g");
}

static bool IsCaptureHookModulePath(const char* modulePath) {
    return modulePath && ce::overlay_compat::detail::ContainsInsensitive(modulePath, "capture_hook");
}

#if defined(__clang__) || defined(__GNUC__)
#define CE_CAPTURE_RETURN_ADDRESS() __builtin_return_address(0)
#elif defined(_MSC_VER)
#define CE_CAPTURE_RETURN_ADDRESS() _ReturnAddress()
#else
#define CE_CAPTURE_RETURN_ADDRESS() nullptr
#endif

static bool IsCodeAddressFromStreamlineModule(const void* codeAddress) {
    if (!codeAddress) {
        return false;
    }

    HMODULE callerModule = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCSTR>(codeAddress), &callerModule)) {
        return false;
    }
    return IsStreamlineModuleHandle(callerModule);
}

static bool TryGetModulePathFromCodeAddress(const void* codeAddress, char* modulePathOut, size_t modulePathOutCount,
                                            HMODULE* moduleOut = nullptr) {
    return ce::overlay_compat::TryGetModulePathFromCodeAddress(codeAddress, modulePathOut, modulePathOutCount,
                                                               moduleOut);
}

static bool HasStartupBlockingOverlayModuleInCurrentStack() {
    constexpr USHORT kMaxFrames = 16;
    void* stackFrames[kMaxFrames] = {};
    const USHORT frameCount = CaptureStackBackTrace(0, kMaxFrames, stackFrames, nullptr);
    for (USHORT i = 0; i < frameCount; ++i) {
        char modulePath[MAX_PATH] = {};
        if (TryGetModulePathFromCodeAddress(stackFrames[i], modulePath, sizeof(modulePath)) &&
            ce::overlay_compat::IsStartupBlockingOverlayModulePath(modulePath)) {
            return true;
        }
    }

    return false;
}

static bool HasStreamlineModuleInCurrentStack() {
    constexpr USHORT kMaxFrames = 24;
    void* stackFrames[kMaxFrames] = {};
    const USHORT frameCount = CaptureStackBackTrace(0, kMaxFrames, stackFrames, nullptr);
    for (USHORT i = 0; i < frameCount; ++i) {
        HMODULE module = nullptr;
        char modulePath[MAX_PATH] = {};
        if (TryGetModulePathFromCodeAddress(stackFrames[i], modulePath, sizeof(modulePath), &module) &&
            IsStreamlineModuleHandle(module)) {
            return true;
        }
    }

    return false;
}

// SL worker thread detection removed — handled directly in DetourPresent
// via !HookIsPostSLOverlayConfirmedRendering() guard.

static bool IsWrappedSwapChainObject(IDXGISwapChain* pSwapChain) {
    if (!pSwapChain) {
        return false;
    }

    void* pWrapper = nullptr;
    if (SUCCEEDED(pSwapChain->QueryInterface(IID_CWrapDXGISwapChain, &pWrapper))) {
        ((IUnknown*)pWrapper)->Release();
        return true;
    }

    return false;
}

APIType DetectAPIType(IDXGISwapChain* pSwapChain);

static bool ShouldForceSteamDX12Bypass(IDXGISwapChain* pSwapChain, bool bypassAvailable, bool slLoaded,
                                       const char** overlayModuleOut = nullptr) {
    const char* overlayModule = ce::overlay_compat::GetLoadedThirdPartyOverlayModuleName();
    if (overlayModuleOut) {
        *overlayModuleOut = overlayModule;
    }
    if (!pSwapChain || !bypassAvailable || !IsSteamOverlayModule(overlayModule)) {
        return false;
    }

    return ShouldForceSteamDX12BypassForState(
        bypassAvailable, true, DetectAPIType(pSwapChain) == APIType::D3D12, IsInWrapperPresent(),
        IsWrappedSwapChainObject(pSwapChain), slLoaded, g_FGCompat.GetRuntimeMode(),
        g_StreamlineFGRunning.load(std::memory_order_acquire), g_FGCompat.IsNvPresentLoaded());
}

static bool ShouldForceThirdPartyOverlayBypass(IDXGISwapChain* pSwapChain, bool bypassAvailable,
                                               const char** overlayModuleOut = nullptr) {
    const char* overlayModule = ce::overlay_compat::GetLoadedThirdPartyOverlayModuleName();
    if (overlayModuleOut) {
        *overlayModuleOut = overlayModule;
    }
    if (!pSwapChain || !bypassAvailable || !overlayModule) {
        return false;
    }

    if (!IsInWrapperPresent() && !IsWrappedSwapChainObject(pSwapChain)) {
        return false;
    }

    return true;
}

static DX12StartupPresentMode GetDX12StartupPresentMode(bool bypassAvailable, const char** overlayModuleOut = nullptr,
                                                        int* passIndexOut = nullptr) {
    const char* overlayModule = ce::overlay_compat::GetLoadedThirdPartyOverlayModuleName();
    if (overlayModuleOut) {
        *overlayModuleOut = overlayModule;
    }
    const bool steamBypassShouldOwnPath = ShouldForceSteamDX12BypassForState(
        bypassAvailable, IsSteamOverlayModule(overlayModule), true, false, false,
        ce::overlay_compat::IsStreamlineInterposerModuleLoaded(), g_FGCompat.GetRuntimeMode(),
        g_StreamlineFGRunning.load(std::memory_order_acquire), g_FGCompat.IsNvPresentLoaded());
    const bool bypassReady = EnsurePresentBypassTrampoline() != nullptr;
    if (!DXGIShared::ShouldAllowDX12StartupPresentPassForState(overlayModule != nullptr, oPresentTrampoline != nullptr,
                                                               oPresent1Trampoline != nullptr, steamBypassShouldOwnPath,
                                                               bypassReady, g_FGCompat.GetRuntimeMode(),
                                                               g_StreamlineFGRunning.load(std::memory_order_acquire))) {
        static std::atomic<int> s_startupPassBlockLogCount{0};
        const int blockNum = s_startupPassBlockLogCount.fetch_add(1, std::memory_order_relaxed);
        if (blockNum < 5) {
            HookLogImportant(
                "GetDX12StartupPresentMode: Startup compat pass blocked "
                "(overlay=%d trampoline=%d bypass=%d steamBypassOwn=%d runtimeMode=%d slFG=%d)",
                overlayModule != nullptr ? 1 : 0, oPresentTrampoline != nullptr ? 1 : 0, bypassReady ? 1 : 0,
                steamBypassShouldOwnPath ? 1 : 0, static_cast<int>(g_FGCompat.GetRuntimeMode()),
                g_StreamlineFGRunning.load(std::memory_order_acquire) ? 1 : 0);
        }
        return DX12StartupPresentMode::kNone;
    }

    static std::atomic<int> s_startupPassCount{0};
    const bool steamOverlay = IsSteamOverlayModule(overlayModule);
    const int startupCompatFrames = (steamOverlay && bypassAvailable) ? 16 : 3;
    int expected = s_startupPassCount.load(std::memory_order_acquire);
    while (expected < startupCompatFrames) {
        if (s_startupPassCount.compare_exchange_weak(expected, expected + 1, std::memory_order_acq_rel,
                                                     std::memory_order_acquire)) {
            if (passIndexOut) {
                *passIndexOut = expected + 1;
            }
            return DX12StartupPresentMode::kPassThroughOriginal;
        }
    }
    return DX12StartupPresentMode::kNone;
}

// Vulkan detection via ICD layer - returns false since we use layer approach
bool IsVulkanPrimary() {
    // VK_LAYER_CE_overlay handles Vulkan separately
    return false;
}

static UINT ResolvePresentFrameLatencyOverride(const char** sourceOut) {
    const auto& cfg = GetActiveGraphicsConfig();

    if (cfg.frameLatency > 0) {
        if (sourceOut)
            *sourceOut = "frame_latency";
        return static_cast<UINT>(cfg.frameLatency);
    }
    if (cfg.cpuPrerenderLimit > 0) {
        if (sourceOut)
            *sourceOut = "cpu_prerender_limit";
        return static_cast<UINT>(cfg.cpuPrerenderLimit);
    }
    if (HasBackbufferCountOverride(cfg.backbufferCount)) {
        if (sourceOut)
            *sourceOut = "backbuffer_count-equivalent-depth";
        return static_cast<UINT>(cfg.backbufferCount - 1);
    }

    if (sourceOut)
        *sourceOut = nullptr;
    return 0;
}

// Wait for DWM flip queue room when backbuffer_count override is active.
// Uses DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT (applied at
// creation) to pace presents so the effective vsync queue depth matches
// the override count, without changing the physical BufferCount.
void WaitBackbufferFrameLatency(IDXGISwapChain* pSwapChain) {
    const auto& gfx = GetActiveGraphicsConfig();
    if (!HasBackbufferCountOverride(gfx.backbufferCount)) {
        static int s_logCount = 0;
        if (s_logCount++ < 3)
            HookLog("WaitBackbufferFrameLatency: no override (count=%d)", gfx.backbufferCount);
        return;
    }

    IDXGISwapChain2* pSC2 = nullptr;
    HRESULT hrQI = pSwapChain->QueryInterface(IID_PPV_ARGS(&pSC2));
    if (FAILED(hrQI) || !pSC2) {
        static int s_logCount = 0;
        if (s_logCount++ < 5)
            HookLog("WaitBackbufferFrameLatency: IDXGISwapChain2 QI failed hr=0x%08X", hrQI);
        return;
    }

    HANDLE hWaitable = pSC2->GetFrameLatencyWaitableObject();
    if (!hWaitable || hWaitable == INVALID_HANDLE_VALUE) {
        static int s_logCount = 0;
        if (s_logCount++ < 5)
            HookLog("WaitBackbufferFrameLatency: GetFrameLatencyWaitableObject returned invalid handle");
        pSC2->Release();
        return;
    }

    DWORD waitResult = WaitForSingleObject(hWaitable, INFINITE);
    if (waitResult == WAIT_OBJECT_0) {
        static int s_logCount = 0;
        if (s_logCount++ < 3)
            HookLog("WaitBackbufferFrameLatency: wait succeeded");
    } else {
        static std::atomic<int> s_waitFailLogCount{0};
        if (s_waitFailLogCount.fetch_add(1, std::memory_order_relaxed) < 10)
            HookLogImportant("WaitBackbufferFrameLatency: wait failed result=%lu error=%lu", waitResult,
                             GetLastError());
    }
    pSC2->Release();
}

// Apply user-configured present-queue latency overrides to an existing swapchain.
// NOTE: backbuffer_count is handled at swapchain creation and resize time.
void ApplyPresentFrameLatencyOverrides(IDXGISwapChain* pSwapChain) {
    if (!pSwapChain)
        return;

    const char* source = nullptr;
    UINT requested = ResolvePresentFrameLatencyOverride(&source);
    if (requested > 16)
        requested = 16;

    static std::mutex s_latencyOverrideMutex;
    static uint64_t s_lastSwapchain = 0;
    static UINT s_lastRequested = 0;

    const uint64_t scKey = reinterpret_cast<uint64_t>(pSwapChain);

    if (requested == 0) {
        return;
    }

    IDXGISwapChain2* sc2 = nullptr;
    if (FAILED(pSwapChain->QueryInterface(__uuidof(IDXGISwapChain2), (void**)&sc2)) || !sc2) {
        static std::atomic<int> s_qiFailLogCount{0};
        if (requested > 0 && s_qiFailLogCount.fetch_add(1, std::memory_order_relaxed) < 5) {
            HookLogImportant("ApplyPresentFrameLatencyOverrides: IDXGISwapChain2 unavailable for %s",
                             source ? source : "override");
        }
        return;
    }

    std::lock_guard<std::mutex> lock(s_latencyOverrideMutex);

    if (s_lastSwapchain == scKey && s_lastRequested == requested) {
        sc2->Release();
        return;
    }

    HRESULT hr = sc2->SetMaximumFrameLatency(requested);
    if (SUCCEEDED(hr)) {
        HookLogImportant("ApplyPresentFrameLatencyOverrides: SetMaximumFrameLatency(%u) OK (%s)", requested,
                         source ? source : "override");
    } else {
        HookLogImportant("ApplyPresentFrameLatencyOverrides: SetMaximumFrameLatency(%u) failed hr=0x%08X (%s)",
                         requested, hr, source ? source : "override");
    }

    s_lastSwapchain = scKey;
    s_lastRequested = requested;

    sc2->Release();
}

PerformanceMetrics* GetPerformanceMetrics() {
    return &g_DXGIPerfMetrics;
}

uint32_t GetLatestSourceFrameIndex() {
    return g_LatestSourceFrameIndex.load(std::memory_order_relaxed);
}

void SetLatestSourceFrameIndex(uint32_t frameIndex) {
    g_LatestSourceFrameIndex.store(frameIndex, std::memory_order_relaxed);
}

APIType DetectAPIType(IDXGISwapChain* pSwapChain) {
    if (!pSwapChain)
        return APIType::Unknown;

    // Fast path: avoid expensive GetDevice() calls every Present on the same
    // swapchain/thread.
    thread_local IDXGISwapChain* s_cachedSwapchain = nullptr;
    thread_local APIType s_cachedApi = APIType::Unknown;
    if (pSwapChain == s_cachedSwapchain && s_cachedApi != APIType::Unknown) {
        return s_cachedApi;
    }

    bool hasD3D12Device = false;
    bool hasD3D11Device = false;
    bool hasD3D10Device = false;

    ID3D12Device* d12Device = nullptr;
    if (SUCCEEDED(pSwapChain->GetDevice(__uuidof(ID3D12Device), (void**)&d12Device)) && d12Device) {
        d12Device->Release();
        hasD3D12Device = true;
    }

    // Always try all three — do NOT short-circuit when D3D11 succeeds.
    // On Windows 10+ the D3D10 runtime is implemented on D3D11
    // (D3D10-on-D3D11).  A D3D10 device will QI for BOTH ID3D11Device
    // and ID3D10Device, so checking D3D11 first and skipping D3D10
    // would wrongly classify the swapchain as D3D11.
    ID3D11Device* d11Device = nullptr;
    if (SUCCEEDED(pSwapChain->GetDevice(__uuidof(ID3D11Device), (void**)&d11Device)) && d11Device) {
        d11Device->Release();
        hasD3D11Device = true;
    }

    ID3D10Device* d10Device = nullptr;
    if (SUCCEEDED(pSwapChain->GetDevice(__uuidof(ID3D10Device), (void**)&d10Device)) && d10Device) {
        d10Device->Release();
        hasD3D10Device = true;
    } else {
        ID3D10Device1* d10Device1 = nullptr;
        if (SUCCEEDED(pSwapChain->GetDevice(__uuidof(ID3D10Device1), (void**)&d10Device1)) && d10Device1) {
            d10Device1->Release();
            hasD3D10Device = true;
        }
    }

    APIType detected = SelectPrimarySwapChainAPIType(hasD3D12Device, hasD3D11Device, hasD3D10Device);

    s_cachedSwapchain = pSwapChain;
    s_cachedApi = detected;
    return detected;
}

// Helper to get Present function address from a swapchain's vtable
static void* GetPresentAddress(IDXGISwapChain* pSwapChain) {
    if (!pSwapChain)
        return nullptr;
