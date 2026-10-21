// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall
// The production assembly of a Runtime: real helper processes, the process's settings owner, the desktop
// overlay, the system hotkeys, desktop screenshots and the injecting launcher. Product image only; the
// core tests assemble the engine from fakes (runtime_internal.h).
#include "runtime/core/runtime_internal.h"

#include "captureengine/roles/role_entries.h"
#include "common/logging/logging.h"
#include "common/platform/ansi_path.h"
#include "common/platform/runtime_package_paths.h"
#include "runtime/core/helper_transport.h"
#include "runtime/core/service_policy.h"
#include "runtime/core/store_settings_source.h"
#include "runtime/desktop/desktop_overlay.h"
#include "runtime/desktop/global_hotkeys.h"
#include "runtime/screenshot/screenshot_capture.h"

namespace ce::engine {
namespace {
// The thread entries of the helper roles (DR-36, DR-45): the role code compiled into cengine.dll. A role runs on a
// runtime thread unless the runtime is isolated; this is the only place that names the entries.
constexpr ce::runtime::RoleEntries kProductionRoleEntries{&ce::role::RunInjectRole, &ce::role::RunMediaRole,
                                                          &ce::role::RunLoggerRole};

class DesktopOverlayPresence final : public DesktopPresence {
public:
    explicit DesktopOverlayPresence(HINSTANCE windowModule) : overlay_(windowModule) {}
    bool Apply(const AppConfig& config, const char* reason) override { return overlay_.Apply(config, reason); }
    void OnStartIntent(RecordingStartIntent intent) override { overlay_.OnStartIntent(intent); }
    void OnFinalizing() override { overlay_.OnFinalizing(); }
    void BeginCapture() override { overlay_.BeginCapture(); }
    void EndCapture(bool saved) override { overlay_.EndCapture(saved); }
    void Stop() override { overlay_.Stop(); }

private:
    ce::runtime::DesktopOverlay overlay_;
};
}  // namespace

std::unique_ptr<Runtime> Runtime::Create(const RuntimeOptions& options, ce_status_t* status) {
    // One runtime per Windows session, refused before anything else exists (no session directory, no log).
    auto session = std::make_unique<ProcessSession>();
    ce::runtime::SessionClaim::Refusal refusal = ce::runtime::SessionClaim::Refusal::None;
    DWORD claimError = ERROR_SUCCESS;
    session->claim = ce::runtime::SessionClaim::TryAcquire(ce::runtime::kSessionClaimName, &refusal, &claimError);
    if (!session->claim) {
        if (refusal == ce::runtime::SessionClaim::Refusal::Held) {
            OutputDebugStringA("[Runtime] another CaptureEngine runtime holds this Windows session\n");
            if (status)
                *status = CE_E_BUSY;
        } else {
            char text[96];
            snprintf(text, sizeof(text), "[Runtime] the session claim cannot be created (error=%lu)\n", claimError);
            OutputDebugStringA(text);
            if (status)
                *status = CE_E_INTERNAL;
        }
        return nullptr;
    }

    RuntimeParts parts;
    parts.options = options;
    // Not const: a const capture makes the closure's move constructor copy the strings, and that copy may throw.
    std::string configPath = options.configPath;
    std::wstring roleHost = options.roleHost;
    // The store opens the file the helpers load. A caller that gave only the active-code-page path gets its conversion.
    const std::wstring configPathWide =
        options.configPathWide.empty() ? ce::runtime::ActiveCodePagePathToWide(options.configPath) : options.configPathWide;
    parts.store = std::make_shared<ce::runtime::SettingsStore>(configPathWide);
    parts.settings = MakeStoreSettings(parts.store);
    // The helper transports (DR-45) are decided once, from the feature bits and the setting read just now.
    const ce::engine::HelperTransportChoice transports = ce::engine::ChooseHelperTransports(
        options.features, parts.settings->Current().helperIsolation, kProductionRoleEntries);
    ce::runtime::ThreadRoleSetup threads;
    threads.entries = kProductionRoleEntries;
    threads.transports = transports.transports;
    threads.processTuning = (options.features & CE_FEATURE_PROCESS_TUNING) != 0;
    threads.roleHostPath = roleHost.empty() ? ce::ansi_path::ModulePathW(nullptr) : roleHost;
    threads.packageDir = ce::ansi_path::ParentDirectoryW(threads.roleHostPath);
    threads.logsRoot = options.logsRoot;
    threads.currentConfig = [store = parts.store]() { return *store->Current(); };
    // The session directory, the runtime log and crash dumps follow the settings just loaded (log_level).
    if (!options.logsRoot.empty()) {
        const AppConfig& config = parts.settings->Current();
        ce::runtime::DiagnosticsOptions diagnostics;
        diagnostics.logsRoot = options.logsRoot;
        diagnostics.crashHandler = (options.features & CE_FEATURE_CRASH_HANDLER) != 0;
        diagnostics.loggerEnabled = ShouldStartLoggerProcess(config);
        diagnostics.sensorEnabled = ShouldStartSensorProcess(config, false);
        session->diagnostics = ce::runtime::DiagnosticsSession::Open(diagnostics, config);
        // Role threads write their own log files in the session directory; without logging there are none.
        if (session->diagnostics && IsAnyLoggingEnabled(config.logLevel))
            threads.sessionDir = options.logsRoot + "\\" + session->diagnostics->SessionName();
    }
    session->residueBaseline = ce::runtime::SampleThisProcess();
    LogInfo("%s", ce::engine::DescribeHelperTransports(transports).c_str());
    parts.makeChildren = [configPath, roleHost, threads](void (*pump)(), bool (*accepting)()) {
        return std::unique_ptr<ce::runtime::HostChildren>(std::make_unique<ce::runtime::ChildSupervisor>(
            configPath.c_str(), pump, accepting, roleHost.empty() ? nullptr : roleHost.c_str(), threads));
    };
    parts.session = std::move(session);
    const HINSTANCE windowModule = options.windowModule ? options.windowModule : GetModuleHandleW(nullptr);
    parts.makeDesktop = [windowModule]() {
        return std::unique_ptr<DesktopPresence>(std::make_unique<DesktopOverlayPresence>(windowModule));
    };
    parts.hotkeys = &SystemHotkeys();
    parts.setup = &ce::runtime::SystemSetupBackend();
    parts.capture = &ce::runtime::CaptureDesktopScreenshot;
    parts.makeLauncher = [packageDir = threads.packageDir](const AppConfig& snapshot) {
        return std::make_unique<ce::runtime::TargetLauncher>(snapshot, packageDir);
    };
    parts.clock = SystemClock();
    parts.vulkanResidency = true;
    parts.heartbeat = true;
    return RuntimeFactory::Assemble(std::move(parts), status);
}

}  // namespace ce::engine
