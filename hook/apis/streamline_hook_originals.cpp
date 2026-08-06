#include "streamline_hook_internal.h"

void LogStaleStreamlineOriginalBlockedOnce(const char* streamline_hook_functionName,  void* original,  void* validationAddress, 
                                           const char* expectedModuleRole,  DWORD error) {


    static std::mutex s_logMutex;
    static std::unordered_map<std::string, bool> s_loggedOriginals;

    std::string key = streamline_hook_functionName ? streamline_hook_functionName : "<unknown>";
    key += '|';
    key += std::to_string(reinterpret_cast<uintptr_t>(original));
    key += '|';
    key += std::to_string(reinterpret_cast<uintptr_t>(validationAddress));

    {
        std::lock_guard<std::mutex> lock(s_logMutex);
        if (s_loggedOriginals.find(key) != s_loggedOriginals.end()) {
            return;
        }
        s_loggedOriginals.emplace(key, true);
    }

    HookLogImportant(
        "Streamline Hook: Blocking stale original forward for %s original=%p validation=%p expected=%s "
        "ownerLoaded=0 error=%lu",
        streamline_hook_functionName ? streamline_hook_functionName : "<unknown>", original, validationAddress,
        expectedModuleRole ? expectedModuleRole : "loaded Streamline module", static_cast<unsigned long>(error));

}


bool IsSavedStreamlineOriginalCallable(const char* streamline_hook_functionName,  void* original,  void* validationAddress, 
                                       const char* expectedModuleRole) {


    const void* addressToValidate = validationAddress ? validationAddress : original;
    DWORD ownerError = ERROR_SUCCESS;
    const bool ownerLoaded =
        DoesAddressBelongToLoadedModule(const_cast<void*>(addressToValidate), nullptr, nullptr, 0, &ownerError);
    if (ce::streamline_runtime_policy::ShouldForwardSavedStreamlineOriginal(original != nullptr, ownerLoaded)) {
        return true;
    }

    if (original) {
        LogStaleStreamlineOriginalBlockedOnce(streamline_hook_functionName, original, const_cast<void*>(addressToValidate),
                                              expectedModuleRole, ownerError);
    }
    return false;

}


PFN_slGetFeatureFunction GetCallableOriginalGetFeatureFunction() {


    auto original = streamline_hook_g_Original_slGetFeatureFunction;
    return IsSavedStreamlineOriginalCallable("slGetFeatureFunction", reinterpret_cast<void*>(original),
                                             streamline_hook_g_SLGetFeatureFunctionTarget.load(std::memory_order_acquire),
                                             "core Streamline module")
               ? original
               : nullptr;

}


PFN_slGetPluginFunction GetCallableOriginalGetPluginFunction() {


    auto original = streamline_hook_g_Original_slGetPluginFunction;
    return IsSavedStreamlineOriginalCallable("slGetPluginFunction", reinterpret_cast<void*>(original),
                                             streamline_hook_g_SLGetPluginFunctionTarget.load(std::memory_order_acquire),
                                             "core Streamline module")
               ? original
               : nullptr;

}


PFN_slSetD3DDevice GetCallableOriginalSetD3DDevice() {


    auto original = streamline_hook_g_Original_slSetD3DDevice;
    return IsSavedStreamlineOriginalCallable("slSetD3DDevice", reinterpret_cast<void*>(original),
                                             streamline_hook_g_SLSetD3DDeviceTarget.load(std::memory_order_acquire),
                                             "core Streamline module")
               ? original
               : nullptr;

}


PFN_slSetTag GetCallableOriginalSetTag() {


    auto original = streamline_hook_g_Original_slSetTag;
    return IsSavedStreamlineOriginalCallable("slSetTag", reinterpret_cast<void*>(original),
                                             streamline_hook_g_SLSetTagTarget.load(std::memory_order_acquire), "core Streamline module")
               ? original
               : nullptr;

}


PFN_slSetTagForFrame GetCallableOriginalSetTagForFrame() {


    auto original = streamline_hook_g_Original_slSetTagForFrame;
    return IsSavedStreamlineOriginalCallable("slSetTagForFrame", reinterpret_cast<void*>(original),
                                             streamline_hook_g_SLSetTagForFrameTarget.load(std::memory_order_acquire),
                                             "core Streamline module")
               ? original
               : nullptr;

}


PFN_slEvaluateFeature GetCallableOriginalEvaluateFeature() {


    auto original = streamline_hook_g_Original_slEvaluateFeature;
    return IsSavedStreamlineOriginalCallable("slEvaluateFeature", reinterpret_cast<void*>(original),
                                             streamline_hook_g_SLEvaluateFeatureTarget.load(std::memory_order_acquire),
                                             "core Streamline module")
               ? original
               : nullptr;

}


PFN_slDLSSGSetOptions GetCallableOriginalDLSSGSetOptions() {


    auto original = streamline_hook_g_Original_slDLSSGSetOptions;
    return IsSavedStreamlineOriginalCallable("slDLSSGSetOptions", reinterpret_cast<void*>(original),
                                             streamline_hook_g_DLSSGSetOptionsTarget.load(std::memory_order_acquire),
                                             "DLSSG feature module")
               ? original
               : nullptr;

}


PFN_slDLSSGGetState GetCallableOriginalDLSSGGetState() {


    auto original = streamline_hook_g_Original_slDLSSGGetState;
    return IsSavedStreamlineOriginalCallable("slDLSSGGetState", reinterpret_cast<void*>(original),
                                             streamline_hook_g_DLSSGGetStateTarget.load(std::memory_order_acquire),
                                             "DLSSG feature module")
               ? original
               : nullptr;

}


PFN_slReflexSleep GetCallableOriginalReflexSleep() {


    auto original = streamline_hook_g_Original_slReflexSleep;
    return IsSavedStreamlineOriginalCallable("slReflexSleep", reinterpret_cast<void*>(original),
                                             streamline_hook_g_ReflexSleepTarget.load(std::memory_order_acquire),
                                             "Reflex feature module")
               ? original
               : nullptr;

}


PFN_slReflexSetOptions GetCallableOriginalReflexSetOptions() {


    auto original = streamline_hook_g_Original_slReflexSetOptions;
    return IsSavedStreamlineOriginalCallable("slReflexSetOptions", reinterpret_cast<void*>(original),
                                             streamline_hook_g_ReflexSetOptionsTarget.load(std::memory_order_acquire),
                                             "Reflex feature module")
               ? original
               : nullptr;

}


PFN_slReflexSetConstants GetCallableOriginalReflexSetConstants() {


    auto original = streamline_hook_g_Original_slReflexSetConstants;
    return IsSavedStreamlineOriginalCallable("slReflexSetConstants", reinterpret_cast<void*>(original),
                                             streamline_hook_g_ReflexSetConstantsTarget.load(std::memory_order_acquire),
                                             "Reflex feature module")
               ? original
               : nullptr;

}

