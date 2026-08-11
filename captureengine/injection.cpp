#include "injection_internal.h"

InjectionManager::InjectionManager(const AppConfig& config) : config(config) {
    const int64_t constructorStartUs = Log_GetQpcUs();

    // Determine DLL paths (assume next to exe)
    char buffer[MAX_PATH];
    GetModuleFileNameA(NULL, buffer, MAX_PATH);
    fs::path exePath(buffer);

    hookDllPathX64 = (exePath.parent_path() / "capture_hook_x64.dll").string();
    hookDllPathX86 = (exePath.parent_path() / "capture_hook_x86.dll").string();

    // FIX: Force absolute path resolution to ensure the correct DLL is injected.
    // Relative paths can be ambiguous if the target process has a different CWD.
    try {
        if (fs::exists(hookDllPathX64))
            hookDllPathX64 = fs::absolute(hookDllPathX64).string();
        if (fs::exists(hookDllPathX86))
            hookDllPathX86 = fs::absolute(hookDllPathX86).string();
    } catch (const fs::filesystem_error& e) {
        LogError("Filesystem error resolving absolute paths: %s", e.what());
    }

    // Check if DLLs exist
    if (!fs::exists(hookDllPathX64))
        LogError("Capture Hook X64 DLL not found: %s", hookDllPathX64.c_str());
    if (!fs::exists(hookDllPathX86))
        LogError("Capture Hook X86 DLL not found: %s", hookDllPathX86.c_str());

    const int64_t wmiStartUs = Log_GetQpcUs();
    bool wmiInitialized = InitializeWMI();
    const int64_t wmiTotalUs = Log_GetQpcUs() - wmiStartUs;

    const int64_t scanStartUs = Log_GetQpcUs();
    ScanExistingProcesses();
    const int64_t scanTotalUs = Log_GetQpcUs() - scanStartUs;

    LogInfo(
        "[StartupPerf] InjectionManager startup: InitializeWMI=%.3f ms (ok=%d), ScanExistingProcesses=%.3f ms, "
        "total=%.3f ms",
        QpcDeltaToMs(wmiTotalUs), wmiInitialized ? 1 : 0, QpcDeltaToMs(scanTotalUs),
        QpcDeltaToMs(Log_GetQpcUs() - constructorStartUs));
}

InjectionManager::~InjectionManager() {
    // Reject and drain WMI callbacks before joining raw-owner worker threads.
    // Launch and worker-list transfer are serialized by threadListMutex, so no
    // worker can appear after the list has been claimed for shutdown.
    try {
        RequestShutdown();
        ShutdownWMI();
        WaitForInjectionThreads(5000);
        EjectAll();
    } catch (...) {
        LogWarn("[Injection] Suppressed exception during manager destruction");
    }
}

void InjectionManager::SetOnInjectCallback(std::function<void(const std::string&)> callback) {
    this->onInjectCallback = std::move(callback);
}

void InjectionManager::UpdateConfig(const AppConfig& newConfig) {
    std::lock_guard<std::mutex> lock(configMutex);
    config = newConfig;
}
