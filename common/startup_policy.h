#pragma once

namespace ce::startup {

struct Preferences {
    bool service = false;
    bool elevated = false;
    bool autostart = false;
};

enum class Registration { None, UserRun, ElevatedTask };

inline Registration SelectRegistration(const Preferences& preferences, bool administratorAccount) {
    if (!preferences.autostart)
        return Registration::None;
    return preferences.elevated && administratorAccount ? Registration::ElevatedTask : Registration::UserRun;
}

inline bool ShouldRequestElevation(bool controllerLaunch, bool alreadyElevated, const Preferences& preferences) {
    return controllerLaunch && !alreadyElevated && preferences.elevated;
}

// OS registration must succeed before committing a preference. Failed persistence
// restores the preceding registration; cancellation never reaches persistence.
template <typename Apply, typename Persist, typename Rollback>
unsigned long ApplyTransaction(Apply apply, Persist persist, Rollback rollback, unsigned long persistenceError,
                               unsigned long& rollbackError) {
    rollbackError = 0;
    const unsigned long error = apply();
    if (error)
        return error;
    if (persist())
        return 0;
    rollbackError = rollback();
    return persistenceError;
}

}  // namespace ce::startup
