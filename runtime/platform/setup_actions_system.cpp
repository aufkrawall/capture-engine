#include "setup_actions.h"

#include <windows.h>

#include "captureengine/elevation/startup_control.h"
#include "captureengine/sensors/pawnio_setup.h"
#include "common/ipc/elevation_windows.h"

#include <cwchar>
#include <filesystem>

namespace ce::runtime {
namespace {

// The application's own executable (captureengine.exe) registers the application's autostart entry, as the tray always
// has. Any other process that loads the runtime registers itself as a client.
ce::startup::AutostartTarget SystemClientTarget() {
    const std::wstring executable = ce::elevation::ExecutablePath();
    if (_wcsicmp(std::filesystem::path(executable).filename().c_str(), L"captureengine.exe") == 0)
        return ce::startup::ApplicationAutostartTarget();
    return ClientAutostartTarget(executable, L"");
}

// Forwards every seam call to the startup and PawnIO setup entry points the tray uses.
class SystemSetupBackendImpl final : public SetupBackend {
public:
    ce::startup::AutostartTarget ClientTarget() override {
        return SystemClientTarget();
    }
    unsigned long RequestElevationService(bool install) override {
        return ce::startup::RequestElevationService(install);
    }
    unsigned long RequestClientAutostart(const ce::startup::AutostartTarget& target, bool enable) override {
        return ce::startup::RequestClientAutostart(target, enable);
    }
    std::optional<unsigned long> TakeSetupResult() override {
        return ce::startup::TakeSetupResult();
    }
    std::optional<unsigned long> WaitSetupResult() override {
        return ce::startup::WaitSetupResult();
    }
    ce::startup::ElevationServiceState QueryElevationServiceState() override {
        return ce::startup::QueryElevationServiceState();
    }
    ce::startup::AutostartState QueryClientAutostart(const ce::startup::AutostartTarget& target) override {
        return ce::startup::QueryClientAutostart(target);
    }

    unsigned long RequestDriverInstall() override {
        return ce::pawnio::RequestDriverInstall();
    }
    unsigned long RequestDriverUninstall() override {
        return ce::pawnio::RequestDriverUninstall();
    }
    std::optional<unsigned long> TakeDriverSetupResult() override {
        return ce::pawnio::TakeDriverSetupResult();
    }
    std::optional<unsigned long> WaitDriverSetupResult() override {
        return ce::pawnio::WaitDriverSetupResult();
    }
    bool IsDriverInstalled() override {
        return ce::pawnio::IsDriverInstalled();
    }
};

}  // namespace

SetupBackend& SystemSetupBackend() {
    static SystemSetupBackendImpl backend;
    return backend;
}

}  // namespace ce::runtime
