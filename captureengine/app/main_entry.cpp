#include "main_internal.h"

#include "frontend/app/frontend_main.h"
#include "common/platform/path_utils.h"
#include "captureengine/elevation/startup_control.h"
#include "role_host.h"

// Main entry point. Every helper launch (setup actions, workers, the sensors bridge, the dump helper and the --mode=
// roles) goes through the shared role-host dispatcher; what is left is the app.
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    if (const std::optional<int> helperResult = ce::role_host::TryRun(lpCmdLine))
        return *helperResult;

    if (HasExactCommandLineArgument(L"--ce-service-integration"))
        return static_cast<int>(ce::startup::RunElevationIntegration());
    if (HasExactCommandLineArgument(L"--ce-service-fixture"))
        return static_cast<int>(ce::startup::RunElevationFixture(HasExactCommandLineArgument(L"--ce-fixture-wait")));

    // The app (llm-wiki/library/05) drives the runtime only through its C API. Settings, the session directory,
    // logging, crash handling and the one-runtime-per-session claim belong to the runtime it creates, so nothing
    // below runs in this process.
    const bool informational =
        HasExactCommandLineArgument(L"--list-monitors") || HasExactCommandLineArgument(L"--license");
    if (const auto startupResult = ce::startup::Bootstrap(!informational))
        return *startupResult;
    if (HasExactCommandLineArgument(L"--list-monitors"))
        return ce::monitor_selection::WriteMonitorListToStandardOutput();
    return FrontendMain(hInstance);
}
