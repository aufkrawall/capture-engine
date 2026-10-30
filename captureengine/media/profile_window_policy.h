#pragma once

#include "common/config/application_profile.h"

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

}  // namespace ce::profile_window
