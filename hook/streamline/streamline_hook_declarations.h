#pragma once

// Streamline hook installation helpers, detour and original-function declarations.
// Included by streamline_hook_internal.h at this point; not a standalone header.

template <typename T>
bool InstallInlineHookOnce(void* target, void* detour, T& original, std::atomic<bool>& installedFlag,
                           std::atomic<void*>& targetSlot, const char* hookName,
                           std::atomic<void*>* failedTargetSlot = nullptr,
                           std::atomic<uint32_t>* failedAttemptsSlot = nullptr) {
    if (!target) {
        return false;
    }

    if (target == detour) {
        original = nullptr;
        targetSlot.store(target, std::memory_order_release);
        installedFlag.store(true, std::memory_order_release);
        return true;
    }

    const void* installedTarget = targetSlot.load(std::memory_order_acquire);
    const bool slotInstalled = installedFlag.load(std::memory_order_acquire);
    if (slotInstalled && installedTarget == target) {
        return false;
    }

    const void* failedTarget = failedTargetSlot ? failedTargetSlot->load(std::memory_order_acquire) : nullptr;
    const uint32_t failedAttempts = failedAttemptsSlot ? failedAttemptsSlot->load(std::memory_order_acquire) : 0;
    if (!ce::streamline_runtime_policy::ShouldAttemptInlineHookOnTarget(target, failedTarget, failedAttempts)) {
        return false;
    }

    // A single process-global `original` cannot serve two live targets. Refuse the
    // newcomer while the installed target is still mapped; the live instance keeps
    // working and CE simply does not observe the duplicate.
    if (!ce::streamline_runtime_policy::ShouldRetargetStreamlineHookSlot(
            slotInstalled, installedTarget, target,
            installedTarget != nullptr &&
                DoesAddressBelongToLoadedModule(const_cast<void*>(installedTarget), nullptr, nullptr, 0, nullptr))) {
        static std::atomic<uint32_t> s_refusedRetargetCount{0};
        const uint32_t refusedCount = s_refusedRetargetCount.fetch_add(1, std::memory_order_relaxed) + 1;
        if (ce::log_meter::ShouldLogCadence(refusedCount, 10, 300)) {
            HookLogImportant(
                "Streamline Hook: Refusing to retarget %s from %p to %p — the installed target is still mapped, so a "
                "second live instance would take over CE's single forward pointer (count=%u)",
                hookName, installedTarget, target, refusedCount);
        }
        return false;
    }

    void* retainedTrampoline = nullptr;
    if (InlineHook::TryGetInstalledTrampoline(target, detour, &retainedTrampoline)) {
        original = reinterpret_cast<T>(retainedTrampoline);
        targetSlot.store(target, std::memory_order_release);
        installedFlag.store(true, std::memory_order_release);
        if (failedTargetSlot && failedAttemptsSlot) {
            failedTargetSlot->store(nullptr, std::memory_order_release);
            failedAttemptsSlot->store(0, std::memory_order_release);
        }
        HookLogImportant(
            "Streamline Hook: Reconciled rediscovered %s at %p with CE's retained live hook (trampoline=%p)",
            hookName, target, retainedTrampoline);
        return true;
    }

    StreamlineInlineHookPublication<T> publication{&original, original};
    void* trampoline = nullptr;
    if (!InlineHook::InstallPublished(target, detour, &trampoline, PublishStreamlineInlineHookTrampoline<T>,
                                      &publication)) {
        uint32_t currentFailures = 1;
        if (failedTargetSlot && failedAttemptsSlot) {
            if (failedTargetSlot->load(std::memory_order_acquire) != target) {
                failedTargetSlot->store(target, std::memory_order_release);
                failedAttemptsSlot->store(1, std::memory_order_release);
            } else {
                currentFailures = failedAttemptsSlot->fetch_add(1, std::memory_order_acq_rel) + 1;
            }
        }
        static std::atomic<uint32_t> s_installFailureCount{0};
        const uint32_t failureCount = s_installFailureCount.fetch_add(1, std::memory_order_relaxed) + 1;
        if (ce::log_meter::ShouldLogCadence(failureCount, 10, 300)) {
            HookLogImportant("Streamline Hook: Failed to inline hook %s at %p (attempt=%u targetFailures=%u)",
                             hookName, target, failureCount, currentFailures);
        }
        return false;
    }

    if (failedTargetSlot && failedAttemptsSlot) {
        failedTargetSlot->store(nullptr, std::memory_order_release);
        failedAttemptsSlot->store(0, std::memory_order_release);
    }
    targetSlot.store(target, std::memory_order_release);
    installedFlag.store(true, std::memory_order_release);
    HookLogImportant("Streamline Hook: Inline hook installed for %s at %p (trampoline=%p)", hookName, target,
                     trampoline);
    return true;
}
void LogFeatureImportFallbackUnavailableOnce(const char* moduleBaseName, const char* streamline_hook_functionName,
                                             void* exportedProc, const char* hookName, const char* reason);
bool InstallFeatureImportFallbackIfPresent(const char* moduleBaseName, const char* streamline_hook_functionName,
                                           void* detour, void* exportedProc, void** originalSlot, const char* hookName);
bool TryGetOwningModulePath(void* address, char* modulePath, DWORD modulePathCapacity, DWORD* outError);
bool TryInstallFeatureImportFallbackForOwningModule(void* streamline_hook_function,
                                                    const char* streamline_hook_functionName, void* detour,
                                                    void** originalSlot, std::atomic<void*>& attemptedTarget,
                                                    const char* hookName);
void LogReturnedWrapperFallbackOnce(std::atomic<bool>& loggedFlag, const char* hookName, void* target, void* wrapper,
                                    bool hookReady);
void LogProactiveFeatureHookGapOnce(std::atomic<bool>& loggedFlag, const char* hookName, void* target);
void LogFeatureLookupOutcomeOnce(std::atomic<bool>& loggedFlag, const char* hookName, void* originalTarget,
                                 void* returnedTarget, bool hookReady);
bool MaybeHookDLSSGSetOptions(void*& streamline_hook_function, bool fallbackToReturnedWrapper);
bool MaybeHookDLSSGGetState(void*& streamline_hook_function, bool fallbackToReturnedWrapper);
bool MaybeHookReflexSleep(void*& streamline_hook_function, bool fallbackToReturnedWrapper);
bool MaybeHookReflexSetOptions(void*& streamline_hook_function, bool fallbackToReturnedWrapper);
bool MaybeHookReflexSetConstants(void*& streamline_hook_function, bool fallbackToReturnedWrapper);
bool TryResolveDLSSGFeatureHooks(bool proactiveScan = false);
bool TryResolveReflexFeatureHooks(bool proactiveScan = false);
uint32_t QueryCapabilityMax(const slViewportHandle& viewport, const slDLSSGOptions* streamline_hook_options);
void RegisterDynamicHooksOnce();
bool InstallHooksForModule(HMODULE module, const char* moduleNameOrPath);
bool OpenLoadedModuleSnapshotWithRetry(HANDLE& snapshot, MODULEENTRY32& firstEntry, DWORD& error, int& attempts,
                                       bool& failedOnFirstEntry);
bool ScanLoadedStreamlineModules(bool pinFeatureResolution = false, bool* snapshotCompleted = nullptr);
void ResolveStreamlineFeatureHooks(bool pinFeatureResolution);
bool AreReflexFeatureHooksComplete();
bool IsPCLSetMarkerHookComplete();
void RetryResolveReflexFeatureHooksForRuntimeActivity(const char* source);
slResult Hooked_slDLSSGGetState(const slViewportHandle& viewport, slDLSSGState& state,
                                const slDLSSGOptions* streamline_hook_options);
slResult Hooked_slDLSSGSetOptions(const slViewportHandle& viewport, const slDLSSGOptions& streamline_hook_options);

// Safe no-op stub for SL function pointers that SL returned as NULL during
// re-entrant calls.  Steam's OverlayHookD3D3 may call slGetFeatureFunction
// from within SL's execution context (during DllMain or FG processing).
// If SL returns NULL, Steam calls through the NULL pointer → RIP=0 crash.
// Instead of returning an error (which Steam may ignore while still using the
// NULL pointer), substitute a safe stub that returns success and does nothing.
// This allows Steam to continue overlay rendering without crashing.
slResult SlNullFunctionStub();
void* Hooked_slGetPluginFunction(const char* streamline_hook_functionName);
slResult Hooked_slGetFeatureFunction(uint32_t feature, const char* streamline_hook_functionName,
                                     void*& streamline_hook_function);
slResult Hooked_slSetD3DDevice(void* streamline_hook_d3dDevice);
bool StructTypesEqual(const slStructType& lhs, const slStructType& rhs);
bool TryRecordOfficialUiResourceTag(const void* frameToken, const slResourceTag& tag,
                                    void* streamline_hook_commandBuffer);
uint32_t LogOfficialUiTagOpportunity(const char* tagApi, const void* frameToken, uint32_t viewportKey,
                                     const slResourceTag* tags, uint32_t numTags, void* streamline_hook_commandBuffer,
                                     uint32_t feature = UINT_MAX, uint32_t numInputs = 0,
                                     uint64_t localTagSignature = 0);
void TryRecordOfficialUiTag(const char* tagApi, const void* frameToken, const slViewportHandle& viewport,
                            const slResourceTag* tags, uint32_t numTags, void* streamline_hook_commandBuffer);
slResult Hooked_slSetTag(const slViewportHandle& viewport, const slResourceTag* tags, uint32_t numTags,
                         void* streamline_hook_commandBuffer);
slResult Hooked_slSetTagForFrame(const slBaseStructure& streamline_hook_frame, const slViewportHandle& viewport,
                                 const slResourceTag* tags, uint32_t numTags, void* streamline_hook_commandBuffer);
slResult Hooked_slEvaluateFeature(uint32_t feature, const slBaseStructure& streamline_hook_frame,
                                  const slBaseStructure** inputs, uint32_t numInputs,
                                  void* streamline_hook_commandBuffer);

// Hook for Streamline Reflex sleep. This lets CE observe game-owned Reflex
// pacing without patching NvAPI_D3D_Sleep inside nvapi64.dll.
slResult Hooked_slReflexSleep(const void* streamline_hook_frame);

// Hook for current Streamline Reflex options — detects low-latency and FPS limiter signals.
slResult Hooked_slReflexSetOptions(const slReflexOptions& streamline_hook_options);

// Hook for legacy slReflexSetConstants — detects when game activates Reflex via Streamline.
slResult Hooked_slReflexSetConstants(const SLReflexConstants& streamline_hook_consts);
