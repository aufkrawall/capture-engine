#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace ce::startup {

struct Preferences {
    bool service = false;
    bool elevated = false;
    bool autostart = false;
};

enum class Registration { None, UserRun, ElevatedTask };

// The three independent tray settings.
enum class Setting { Service, Elevation, Autostart };

// What an elevated helper is asked to do. Values travel over the authenticated context pipe.
enum class Action : uint32_t { Launch, InstallService, RemoveService, Autostart };

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

// ---- Independently requestable setup actions -------------------------------------------------
//
// The elevation service, the application's autostart registration and the "request administrator
// on the next launch" preference are separate actions. A plan says what one request has to do; the
// callers (tray toggle today, the runtime setup component later) execute it identically.
struct SetupPlan {
    Preferences desired;               // the preference record to commit once the OS state changed
    Action action = Action::Autostart; // the OS change (unused when preferenceOnly)
    bool preferenceOnly = false;       // commit `desired` without touching any OS registration
    bool needsElevatedHelper = false;  // the OS change has to run in an elevated process
};

inline SetupPlan PlanElevationService(const Preferences& stored, bool install) {
    SetupPlan plan;
    plan.desired = stored;
    plan.desired.service = install;
    plan.action = install ? Action::InstallService : Action::RemoveService;
    plan.needsElevatedHelper = true;
    return plan;
}

// Changing the OS registration of the application's autostart: its kind follows the elevation
// preference, so an elevated task on either side needs the elevated helper.
inline SetupPlan PlanRegistrationChange(const Preferences& stored, const Preferences& desired,
                                        bool administratorAccount) {
    SetupPlan plan;
    plan.desired = desired;
    plan.action = Action::Autostart;
    plan.needsElevatedHelper = SelectRegistration(stored, administratorAccount) == Registration::ElevatedTask ||
                               SelectRegistration(desired, administratorAccount) == Registration::ElevatedTask;
    return plan;
}

inline SetupPlan PlanApplicationAutostart(const Preferences& stored, bool enable, bool administratorAccount) {
    Preferences desired = stored;
    desired.autostart = enable;
    return PlanRegistrationChange(stored, desired, administratorAccount);
}

// "Request administrator privileges on the next launch" is launch policy of the application. It only
// reaches the OS when autostart is on, because that registration kind follows the preference.
inline SetupPlan PlanLaunchElevated(const Preferences& stored, bool elevated, bool administratorAccount) {
    Preferences desired = stored;
    desired.elevated = elevated;
    if (stored.autostart)
        return PlanRegistrationChange(stored, desired, administratorAccount);
    SetupPlan plan;
    plan.desired = desired;
    plan.preferenceOnly = true;
    return plan;
}

// A tray click flips what the menu displays (registered service, matching autostart) or, for the
// elevation preference, what is stored.
inline SetupPlan PlanToggle(Setting setting, const Preferences& stored, const Preferences& displayed,
                            bool administratorAccount) {
    switch (setting) {
        case Setting::Service:
            return PlanElevationService(stored, !displayed.service);
        case Setting::Elevation:
            return PlanLaunchElevated(stored, !stored.elevated, administratorAccount);
        case Setting::Autostart:
            break;
    }
    return PlanApplicationAutostart(stored, !displayed.autostart, administratorAccount);
}

// Which path one plan takes: a preference-only commit, the elevated helper (when the process is not
// elevated yet) or the in-process transaction. The callables do the OS work.
template <typename Persist, typename Launch, typename ApplyNow>
unsigned long ExecutePlan(const SetupPlan& plan, bool processElevated, unsigned long persistenceError,
                          Persist persistPreferences, Launch launchHelper, ApplyNow applyInProcess) {
    if (plan.preferenceOnly)
        return persistPreferences(plan.desired) ? 0 : persistenceError;
    return plan.needsElevatedHelper && !processElevated ? launchHelper(plan) : applyInProcess(plan);
}

inline const char* ActionName(Action action) {
    switch (action) {
        case Action::Launch:
            return "launch";
        case Action::InstallService:
            return "service-install";
        case Action::RemoveService:
            return "service-remove";
        case Action::Autostart:
            return "autostart";
    }
    return "unknown";
}

// ---- Status ----------------------------------------------------------------------------------

// Numeric values are the ones the public setup status reports (elevation_service_state).
enum class ElevationServiceState : int { Unknown = -1, Absent = 0, Installed = 1, Running = 2 };

inline bool ServiceRegistered(ElevationServiceState state) {
    return state == ElevationServiceState::Installed || state == ElevationServiceState::Running;
}

inline const wchar_t* ElevationServiceLabel(ElevationServiceState state) {
    switch (state) {
        case ElevationServiceState::Running:
            return L"Use elevation service (running)";
        case ElevationServiceState::Installed:
            return L"Use elevation service (installed, stopped)";
        case ElevationServiceState::Absent:
        case ElevationServiceState::Unknown:
            break;
    }
    return L"Use elevation service (not installed)";
}

enum class AutostartState : int {
    Unknown = -1,  // the registrations could not be read
    Absent = 0,
    UserRun = 1,        // only the Run value, naming the target
    ElevatedTask = 2,   // only the enabled logon task, naming the target
    Mismatch = 3,       // present but not exactly the registration of the target (stale path, both kinds, ...)
};

inline AutostartState ClassifyAutostart(bool hasRun, bool runMatches, bool hasTask, bool taskMatches) {
    if (!hasRun && !hasTask)
        return AutostartState::Absent;
    if (hasRun && runMatches && !hasTask)
        return AutostartState::UserRun;
    if (hasTask && taskMatches && !hasRun)
        return AutostartState::ElevatedTask;
    return AutostartState::Mismatch;
}

inline bool AutostartSatisfies(AutostartState state, Registration expected) {
    switch (expected) {
        case Registration::None:
            return state == AutostartState::Absent;
        case Registration::UserRun:
            return state == AutostartState::UserRun;
        case Registration::ElevatedTask:
            return state == AutostartState::ElevatedTask;
    }
    return false;
}

inline bool PreferencesEqual(const Preferences& left, const Preferences& right) {
    return left.service == right.service && left.elevated == right.elevated && left.autostart == right.autostart;
}

// An autostart plan whose record is already stored and whose registration already is what the record
// selects has nothing to do, so a repeated request neither prompts for elevation nor rewrites anything.
inline bool AutostartPlanApplied(const SetupPlan& plan, const Preferences& stored, AutostartState current,
                                 bool administratorAccount) {
    return plan.action == Action::Autostart && !plan.preferenceOnly && PreferencesEqual(plan.desired, stored) &&
           AutostartSatisfies(current, SelectRegistration(plan.desired, administratorAccount));
}

// Plain registration of a client other than the application: nothing when already in the requested
// state, otherwise the registration to configure. Never an elevated task; that kind belongs to the
// application's own preference.
inline std::optional<Registration> PlainAutostartChange(AutostartState current, bool enable) {
    if (enable)
        return current == AutostartState::UserRun ? std::nullopt : std::optional(Registration::UserRun);
    return current == AutostartState::Absent ? std::nullopt : std::optional(Registration::None);
}

// ---- Autostart target ------------------------------------------------------------------------

inline constexpr wchar_t kApplicationAutostartName[] = L"CaptureEngine";
inline constexpr wchar_t kApplicationAutostartArguments[] = L"--autostart";

// The program an autostart registration starts at logon: the calling client, not a fixed executable.
struct AutostartTarget {
    std::wstring executable;                          // absolute path
    std::wstring arguments;                           // raw command-line tail, empty for none
    std::wstring name = kApplicationAutostartName;    // Run value / task name; one registration per name
};

inline bool IsValidAutostartName(std::wstring_view name) {
    if (name.empty() || name.size() > 64)
        return false;
    for (const wchar_t character : name) {
        const bool plain = (character >= L'0' && character <= L'9') || (character >= L'a' && character <= L'z') ||
                           (character >= L'A' && character <= L'Z');
        if (!plain && character != L'.' && character != L'-' && character != L'_')
            return false;
    }
    return true;
}

// Shape only (the file's existence is the caller's check): an absolute drive or UNC path, no quotes or
// control characters, and a command line inside the CreateProcess limit.
inline bool IsValidAutostartTarget(const AutostartTarget& target) {
    const std::wstring_view path = target.executable;
    const bool drive = path.size() >= 4 && ((path[0] >= L'a' && path[0] <= L'z') || (path[0] >= L'A' && path[0] <= L'Z')) &&
                       path[1] == L':' && path[2] == L'\\';
    const bool unc = path.size() >= 4 && path[0] == L'\\' && path[1] == L'\\';
    if (!IsValidAutostartName(target.name) || (!drive && !unc) || path.size() + target.arguments.size() > 32000)
        return false;
    for (const wchar_t character : path) {
        if (character < L' ' || character == L'"')
            return false;
    }
    for (const wchar_t character : target.arguments) {
        if (character < L' ')
            return false;
    }
    return true;
}

}  // namespace ce::startup
