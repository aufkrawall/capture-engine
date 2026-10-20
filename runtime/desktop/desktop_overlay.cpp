#include "runtime/desktop/desktop_overlay.h"

#include <algorithm>

#include "common/logging/logging.h"

namespace ce::runtime {

std::vector<PseudoOverlayApplicationConfig> ResolveDesktopOverlayProfiles(const AppConfig& config) {
    std::vector<PseudoOverlayApplicationConfig> profiles;
    profiles.reserve(config.applicationProfiles.size());

    for (const ApplicationProfile& profile : config.applicationProfiles) {
        if (!profile.target.HasProcess())
            continue;

        PseudoOverlayApplicationConfig overlayProfile;
        overlayProfile.section = profile.section;
        overlayProfile.processName = profile.target.pattern;
        // Resolved by the config loader: the global settings plus this profile's overrides (the
        // process list is the global one).
        overlayProfile.settings = profile.desktopOverlay;
        overlayProfile.warningTarget = profile.resolvedVideoCapture != ApplicationVideoCapture::kNone;
        overlayProfile.captureUsesInjection = profile.resolvedVideoCapture == ApplicationVideoCapture::kInject;
        profiles.push_back(std::move(overlayProfile));
    }

    LogDebug("[Controller] Resolved DesktopOverlay settings for %zu process-backed application profiles",
             profiles.size());
    return profiles;
}

bool DesktopOverlayRequired(const AppConfig& config, const std::vector<PseudoOverlayApplicationConfig>& profiles) {
    if (config.pseudoOverlay.enabled)
        return true;
    return std::any_of(profiles.begin(), profiles.end(), [](const PseudoOverlayApplicationConfig& profile) {
        return profile.settings.enabled;
    });
}

DesktopOverlay::DesktopOverlay(HINSTANCE windowModule) : windowModule_(windowModule) {}

DesktopOverlay::~DesktopOverlay() {
    Stop();
}

bool DesktopOverlay::Apply(const AppConfig& config, const char* reason) {
    const std::vector<PseudoOverlayApplicationConfig> profiles = ResolveDesktopOverlayProfiles(config);
    if (overlay_) {
        overlay_->UpdateConfig(config.pseudoOverlay, profiles);
        return true;
    }
    if (!DesktopOverlayRequired(config, profiles))
        return true;

    LogInfo("[Controller] Initializing pseudo-overlay (%s)...", reason ? reason : "configuration");
    auto overlay = std::make_unique<PseudoOverlay>();
    overlay->UpdateConfig(config.pseudoOverlay, profiles);
    overlay->SetRecordingStartIntent(startIntent_);
    if (!overlay->Init(windowModule_)) {
        LogError("[Controller] Failed to initialize pseudo-overlay");
        return false;
    }
    overlay_ = std::move(overlay);
    LogInfo("[Controller] Pseudo-overlay initialized");
    return true;
}

void DesktopOverlay::OnStartIntent(RecordingStartIntent intent) {
    startIntent_ = intent;
    if (overlay_)
        overlay_->SetRecordingStartIntent(intent);
}

void DesktopOverlay::OnFinalizing() {
    if (overlay_)
        overlay_->ShowRecordingFinalizingNotification();
}

void DesktopOverlay::BeginCapture() {
    if (overlay_)
        overlay_->BeginScreenshotCapture();
}

void DesktopOverlay::EndCapture(bool saved) {
    if (!overlay_)
        return;
    overlay_->EndScreenshotCapture();
    overlay_->ShowScreenshotNotification(saved);
}

void DesktopOverlay::Stop() {
    if (!overlay_)
        return;
    overlay_->Shutdown();
    overlay_.reset();
}

bool DesktopOverlay::Active() const {
    return overlay_ != nullptr;
}

}  // namespace ce::runtime
