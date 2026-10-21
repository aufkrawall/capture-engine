#include "injection_internal.h"

#include "common/platform/ansi_path.h"
#include "common/platform/package_module.h"
#include "common/platform/startup_launch_control.h"
#include "injection_path_policy.h"
#include "common/logging/log_meter.h"

namespace {

// The package this code belongs to (captureengine.exe's or cengine.dll's folder, also when the helper process is a
// client's own executable): the default for the package host, which owns its process.
InjectionEnvironment ExecutableEnvironment() {
    return InjectionEnvironment{ce::package::PackageDirectory(), true};
}

}  // namespace

InjectionManager::InjectionManager(const AppConfig& config) : InjectionManager(config, ExecutableEnvironment()) {}

InjectionManager::InjectionManager(const AppConfig& config, InjectionEnvironment environment)
    : config(config), packageDir_(std::move(environment.packageDir)), ownsProcess_(environment.ownsProcess) {
    // Hook DLLs sit in the package directory. Resolved as UTF-16 so an install
    // directory outside the system ANSI code page still names the real file.
    const fs::path packageDir(packageDir_);

    // Absolute paths: relative ones would be resolved against the target's CWD.
    std::error_code ec;
    fs::path x64Path = fs::absolute(packageDir / L"capture_hook_x64.dll", ec);
    if (ec)
        x64Path = packageDir / L"capture_hook_x64.dll";
    fs::path x86Path = fs::absolute(packageDir / L"capture_hook_x86.dll", ec);
    if (ec)
        x86Path = packageDir / L"capture_hook_x86.dll";
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

    bool etwStarted = false;
    if (ce::elevation::ServiceEnabled()) {
        etwStarted = processStartEtwMonitor.Start([this](DWORD pid, const std::string& imageName) {
            HandleEtwProcessStart(pid, imageName);
        });
    }

    int64_t wmiTotalUs = 0;
    if (etwStarted) {
        etwMonitoringSelected = true;
        monitoringInitialized = true;
        LogInfo("[Inject] Real-time ETW process-start monitoring active via elevation service");
    } else {
        const int64_t wmiStartUs = Log_GetQpcUs();
        monitoringInitialized = InitializeWMI();
        wmiTotalUs = Log_GetQpcUs() - wmiStartUs;
    }
    ScanStartupLaunchHosts();

    const int64_t scanStartUs = Log_GetQpcUs();
    ScanExistingProcesses();
    const int64_t scanTotalUs = Log_GetQpcUs() - scanStartUs;

    LogInfo(
        "[StartupPerf] InjectionManager monitoring: %s=%.3f ms (ok=%d), "
        "ScanExistingProcesses=%.3f ms, "
        "total=%.3f ms",
        etwStarted ? "StartETW" : "InitializeWMI",
        etwStarted ? 0.0 : QpcDeltaToMs(wmiTotalUs),
        monitoringInitialized ? 1 : 0, QpcDeltaToMs(scanTotalUs),
        QpcDeltaToMs(Log_GetQpcUs() - startUs));
    return monitoringInitialized;
}

void InjectionManager::ServiceEtwFallback() {
    if (IsShuttingDown())
        return;
    ce::process_start::RecoverStoppedTrace(
        etwMonitoringSelected, processStartEtwMonitor.IsRunning(),
        [this] { processStartEtwMonitor.Stop(); },
        [this] {
            const bool started = processStartPoller.IsRunning() || processStartPoller.Start(
                [this](DWORD pid, const std::string& name) { HandlePolledProcessStart(pid, name); },
                ce::process_start::kDefaultPollIntervalMs);
            static ce::log_meter::ChangeGate recoveryLog;
            if (const auto verdict = recoveryLog.Observe(started)) {
                LogWarn("[Inject] ETW consumption stopped; native process discovery fallback %s "
                        "(%llu repeat(s) since last line)", started ? "active; rescanning" : "could not start; will retry",
                        static_cast<unsigned long long>(verdict.suppressed));
            }
            monitoringInitialized = started;
            return started;
        },
        [this] { ScanStartupLaunchHosts(); ScanExistingProcesses(); });
}

InjectionManager::~InjectionManager() {
    // Reject and drain WMI callbacks before joining raw-owner worker threads.
    // Launch and worker-list transfer are serialized by threadListMutex, so no
    // worker can appear after the list has been claimed for shutdown.
    try {
        RequestShutdown();
        processStartEtwMonitor.Stop();
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
    bool detachCreators = false;
    {
        std::lock_guard<std::mutex> lock(configMutex);
        const auto creatorsUsable = [](const AppConfig& target) {
            return target.graphics.streamlineDllPathConfigured &&
                   ce::startup_launch::ParseCreatorScope(target.startupCreatorHosts) !=
                       ce::startup_launch::CreatorScope::Off;
        };
        const bool wasUsable = creatorsUsable(config);
        const bool nowUsable = creatorsUsable(newConfig);
        if (config.gameWhitelist != newConfig.gameWhitelist || config.overlayWhitelist != newConfig.overlayWhitelist ||
            wasUsable != nowUsable)
            startupLaunchScanPending.store(true, std::memory_order_release);
        detachCreators = wasUsable && !nowUsable;
        config = newConfig;
    }
    // Outside configMutex: the detach path takes injectMutex and waits for the
    // creators' own confirmation.
    if (detachCreators) DetachStartupLaunchHosts("no substitution configured any more");
}
