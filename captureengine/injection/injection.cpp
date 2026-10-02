#include "injection_internal.h"

#include "injection_path_policy.h"

InjectionManager::InjectionManager(const AppConfig& config) : config(config) {
    // Determine DLL paths (next to the exe). Resolved as UTF-16 so an install
    // directory outside the system ANSI code page still names the real file.
    std::wstring exePath(32768, L'\0');
    const DWORD exePathLength = GetModuleFileNameW(NULL, exePath.data(), static_cast<DWORD>(exePath.size()));
    exePath.resize(exePathLength < exePath.size() ? exePathLength : 0);
    const fs::path exeDir = fs::path(exePath).parent_path();

    // Absolute paths: relative ones would be resolved against the target's CWD.
    std::error_code ec;
    fs::path x64Path = fs::absolute(exeDir / L"capture_hook_x64.dll", ec);
    if (ec)
        x64Path = exeDir / L"capture_hook_x64.dll";
    fs::path x86Path = fs::absolute(exeDir / L"capture_hook_x86.dll", ec);
    if (ec)
        x86Path = exeDir / L"capture_hook_x86.dll";
    hookDllPathX64W = x64Path.wstring();
    hookDllPathX86W = x86Path.wstring();
    hookDllPathX64 = ce::injection::WidePathToUtf8(hookDllPathX64W);
    hookDllPathX86 = ce::injection::WidePathToUtf8(hookDllPathX86W);

    if (!fs::exists(x64Path, ec))
        LogError("Capture Hook X64 DLL not found: %s", hookDllPathX64.c_str());
    if (!fs::exists(x86Path, ec))
        LogError("Capture Hook X86 DLL not found: %s", hookDllPathX86.c_str());
}

bool InjectionManager::StartMonitoring() {
    std::lock_guard<std::mutex> lock(monitoringMutex);
    if (monitoringStarted) {
        LogInfo("[Inject] Process monitoring is already active");
        return monitoringInitialized;
    }
    monitoringStarted = true;
    const int64_t startUs = Log_GetQpcUs();
    const int64_t wmiStartUs = Log_GetQpcUs();
    monitoringInitialized = InitializeWMI();
    const int64_t wmiTotalUs = Log_GetQpcUs() - wmiStartUs;

    const int64_t scanStartUs = Log_GetQpcUs();
    ScanExistingProcesses();
    const int64_t scanTotalUs = Log_GetQpcUs() - scanStartUs;

    LogInfo(
        "[StartupPerf] InjectionManager monitoring: InitializeWMI=%.3f ms (ok=%d), "
        "ScanExistingProcesses=%.3f ms, "
        "total=%.3f ms",
        QpcDeltaToMs(wmiTotalUs), monitoringInitialized ? 1 : 0, QpcDeltaToMs(scanTotalUs),
        QpcDeltaToMs(Log_GetQpcUs() - startUs));
    return monitoringInitialized;
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

void InjectionManager::SetOnInjectCallback(std::function<void(DWORD, const std::string&)> callback) {
    std::lock_guard<std::mutex> lock(injectCallbackMutex);
    this->onInjectCallback = std::move(callback);
}

void InjectionManager::UpdateConfig(const AppConfig& newConfig) {
    std::lock_guard<std::mutex> lock(configMutex);
    config = newConfig;
}
