#pragma once

#include "common/config/config.h"

namespace ce::profile_window {

// Process-backed profiles are selected once for the actual window owner.
// A title match cannot turn a different process into that profile's target.
inline const ApplicationProfile* SelectedProfile(const std::vector<ApplicationProfile>& profiles,
                                                 const WhitelistEntry& target,
                                                 const ProcessTargetIdentity& identity) {
    if (target.HasProcess()) {
        const ApplicationProfile* selected = FindApplicationProfileForProcessIdentity(profiles, identity);
        return selected && selected->target == target ? selected : nullptr;
    }
    for (auto it = profiles.rbegin(); it != profiles.rend(); ++it) {
        if (it->target == target)
            return &*it;
    }
    return nullptr;
}

inline bool IsScreenCaptureTarget(const ApplicationProfile* profile) {
    return profile && profile->videoCaptureExplicit &&
           (profile->resolvedVideoCapture == ApplicationVideoCapture::kWgc ||
            profile->resolvedVideoCapture == ApplicationVideoCapture::kDxgiDup);
}

// Re-select from the newly loaded config: callers keep the target by value across
// reloads, never a pointer into the previous config's profile vector. Resolve the
// name before the media engine snapshots the config for recording.
inline bool ApplyRecordingOutputProfile(AppConfig& config, const WhitelistEntry& target,
                                        const ProcessTargetIdentity& identity) {
    if (!config.activeProfile.empty())
        return false;
    const ApplicationProfile* profile = SelectedProfile(config.applicationProfiles, target, identity);
    if (!IsScreenCaptureTarget(profile))
        return false;
    config.activeProfile = profile->Name();
    config.video.activeProfile = config.activeProfile;
    return true;
}

}  // namespace ce::profile_window
