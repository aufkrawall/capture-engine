#include "streamline_hook_internal.h"


void LogFeatureImportFallbackUnavailableOnce(const char* moduleBaseName,  const char* streamline_hook_functionName,  void* exportedProc, 
                                             const char* hookName,  const char* reason) {


    const char* effectiveHookName = hookName ? hookName : streamline_hook_functionName;
    if (!moduleBaseName || !effectiveHookName) {
        return;
    }

    static std::mutex s_logMutex;
    static std::unordered_map<std::string, bool> s_loggedUnavailable;

    std::string key = effectiveHookName;
    key += '|';
    key += moduleBaseName;
    key += '|';
    key += std::to_string(reinterpret_cast<uintptr_t>(exportedProc));

    {
        std::lock_guard<std::mutex> lock(s_logMutex);
        if (s_loggedUnavailable.find(key) != s_loggedUnavailable.end()) {
            return;
        }
        s_loggedUnavailable.emplace(key, true);
    }

    HookLogImportant("Streamline Hook: Direct import fallback unavailable for %s via %s (export=%p): %s",
                     effectiveHookName, moduleBaseName, exportedProc, reason ? reason : "no matching loaded import");

}


bool InstallFeatureImportFallbackIfPresent(const char* moduleBaseName,  const char* streamline_hook_functionName,  void* detour, 
                                           void* exportedProc,  void** originalSlot,  const char* hookName) {


    if (!moduleBaseName || !streamline_hook_functionName || !detour || !exportedProc) {
        return false;
    }

    if (originalSlot && *originalSlot == nullptr) {
        *originalSlot = exportedProc;
    }

    void* patchedOriginal = nullptr;
    if (!IATHook::PatchIATAllModules(moduleBaseName, streamline_hook_functionName, detour, &patchedOriginal)) {
        LogFeatureImportFallbackUnavailableOnce(
            moduleBaseName, streamline_hook_functionName, exportedProc, hookName,
            "no loaded module currently imports this feature directly; retrying on later Streamline module scans");
        return false;
    }

    if (originalSlot && *originalSlot == nullptr) {
        *originalSlot = patchedOriginal ? patchedOriginal : exportedProc;
    }

    HookLogImportant("Streamline Hook: Installed direct import fallback for %s via %s (export=%p original=%p)",
                     hookName ? hookName : streamline_hook_functionName, moduleBaseName, exportedProc,
                     originalSlot ? *originalSlot : patchedOriginal);
    return true;

}


bool TryInstallFeatureImportFallbackForOwningModule(void* streamline_hook_function,  const char* streamline_hook_functionName,  void* detour, 
                                                    void** originalSlot,  std::atomic<void*>& attemptedTarget, 
                                                    const char* hookName) {


    if (!streamline_hook_function || !streamline_hook_functionName || !detour) {
        return false;
    }

    if (attemptedTarget.load(std::memory_order_acquire) == streamline_hook_function) {
        return false;
    }

    char ownerPath[MAX_PATH] = {};
    DWORD ownerError = ERROR_SUCCESS;
    if (!TryGetOwningModulePath(streamline_hook_function, ownerPath, MAX_PATH, &ownerError)) {
        attemptedTarget.store(streamline_hook_function, std::memory_order_release);
        HookLogImportant("Streamline Hook: Direct import fallback owner resolution failed for %s target=%p error=%lu",
                         hookName ? hookName : streamline_hook_functionName, streamline_hook_function, static_cast<unsigned long>(ownerError));
        return false;
    }

    attemptedTarget.store(streamline_hook_function, std::memory_order_release);

    const char* ownerBaseName = GetModuleBaseName(ownerPath);
    if (!ownerBaseName || !ownerBaseName[0]) {
        return false;
    }

    return InstallFeatureImportFallbackIfPresent(ownerBaseName, streamline_hook_functionName, detour, streamline_hook_function, originalSlot, hookName);

}


void LogReturnedWrapperFallbackOnce(std::atomic<bool>& loggedFlag,  const char* hookName,  void* target,  void* wrapper, 
                                    bool hookReady) {


    if (loggedFlag.exchange(true, std::memory_order_acq_rel)) {
        return;
    }

    HookLogImportant(
        "Streamline Hook: Using returned-pointer wrapper fallback for %s (target=%p wrapper=%p hookReady=%d). "
        "Callers that cache slGetFeatureFunction results remain intercepted even if Streamline later reloads, "
        "repairs, or bypasses the feature export patch.",
        hookName ? hookName : "<unknown>", target, wrapper, hookReady ? 1 : 0);

}


void LogProactiveFeatureHookGapOnce(std::atomic<bool>& loggedFlag,  const char* hookName,  void* target) {


    if (loggedFlag.exchange(true, std::memory_order_acq_rel)) {
        return;
    }

    HookLogImportant(
        "Streamline Hook: Proactively resolved %s at %p but could not patch the export/import path yet; "
        "waiting for an intercepted slGetFeatureFunction lookup to return the wrapper fallback or for a later module "
        "scan to find a direct import.",
        hookName ? hookName : "<unknown>", target);

}


void LogFeatureLookupOutcomeOnce(std::atomic<bool>& loggedFlag,  const char* hookName,  void* originalTarget, 
                                 void* returnedTarget,  bool hookReady) {


    if (loggedFlag.exchange(true, std::memory_order_acq_rel)) {
        return;
    }

    HookLogImportant(
        "Streamline Hook: slGetFeatureFunction returned %s target=%p delivered=%p hookReady=%d "
        "wrapperSubstituted=%d",
        hookName ? hookName : "<unknown>", originalTarget, returnedTarget, hookReady ? 1 : 0,
        originalTarget != returnedTarget ? 1 : 0);

}
