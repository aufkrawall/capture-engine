#include "main_internal.h"

#include "common/platform/ansi_path.h"
#include "runtime/inject/target_launcher.h"

// Interim owner of the controller's launcher: the controller has no launcher member yet, so this
// lives until exit exactly like the injector it replaced. runtime/core owns a TargetLauncher and
// deletes this adapter. `path` is the active-code-page text of the process command line; the
// launcher takes UTF-8.
void LaunchGameSuspended(const std::string& path) {
    static ce::runtime::TargetLauncher launcher(RuntimeConfiguration());
    const std::wstring wide = ce::ansi_path::AnsiToWide(path);
    const int bytes = wide.empty() ? 0
                                   : WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()),
                                                         nullptr, 0, nullptr, nullptr);
    std::string commandLineUtf8(static_cast<size_t>(bytes), '\0');
    if (bytes > 0) {
        WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()), commandLineUtf8.data(), bytes,
                            nullptr, nullptr);
    }
    launcher.Launch(commandLineUtf8);
}

// Spawn authenticates IPC synchronously; there is no reconnect-by-name phase.
bool ConnectToChildProcesses(DWORD) {
    using namespace ce::runtime;
    return (!HostChildPresent(HostChild::Inject) || HostChildReady(HostChild::Inject)) &&
           (!HostChildPresent(HostChild::Media) || HostChildReady(HostChild::Media));
}

// Send command to all child processes
void SendCommandToAll(ProcessCommand command) {
    ce::runtime::SendHostCommandToAll(command);
}

namespace {

std::vector<PseudoOverlayApplicationConfig> ResolvePseudoOverlayApplicationConfigs(const AppConfig& baseConfig) {
    std::vector<PseudoOverlayApplicationConfig> profiles;
    profiles.reserve(baseConfig.applicationProfiles.size());

    for (const ApplicationProfile& profile : baseConfig.applicationProfiles) {
        if (!profile.target.HasProcess())
            continue;

        PseudoOverlayApplicationConfig overlayProfile;
        overlayProfile.section = profile.section;
        overlayProfile.processName = profile.target.pattern;
        // Resolved by the config loader: the global settings plus this profile's overrides (the
        // process list is the global one).
        overlayProfile.settings = profile.desktopOverlay;
        overlayProfile.warningTarget = profile.resolvedVideoCapture != ApplicationVideoCapture::kNone;
        overlayProfile.captureUsesInjection =
            profile.resolvedVideoCapture == ApplicationVideoCapture::kInject;
        profiles.push_back(std::move(overlayProfile));
    }

    LogDebug("[Controller] Resolved DesktopOverlay settings for %zu process-backed application profiles",
             profiles.size());
    return profiles;
}

}  // namespace

void SyncPseudoOverlayConfiguration(const char* reason) {
    std::vector<PseudoOverlayApplicationConfig> profiles = ResolvePseudoOverlayApplicationConfigs(RuntimeConfiguration());
    const bool anyProfileEnabled =
        std::any_of(profiles.begin(), profiles.end(), [](const PseudoOverlayApplicationConfig& profile) {
            return profile.settings.enabled;
        });

    if (!main_g_PseudoOverlay && !RuntimeConfiguration().pseudoOverlay.enabled && !anyProfileEnabled)
        return;

    if (!main_g_PseudoOverlay) {
        LogInfo("[Controller] Initializing pseudo-overlay (%s)...", reason ? reason : "configuration");
        auto overlay = std::make_unique<PseudoOverlay>();
        overlay->UpdateConfig(RuntimeConfiguration().pseudoOverlay, profiles);
        overlay->SetRecordingStartIntent(ControllerRecordingSnapshot().pendingIntent);
        HMODULE hMod = GetModuleHandle(NULL);
        if (!overlay->Init(reinterpret_cast<HINSTANCE>(hMod))) {
            LogError("[Controller] Failed to initialize pseudo-overlay");
            return;
        }
        main_g_PseudoOverlay = std::move(overlay);
        LogInfo("[Controller] Pseudo-overlay initialized");
        return;
    }

    main_g_PseudoOverlay->UpdateConfig(RuntimeConfiguration().pseudoOverlay, profiles);
}
