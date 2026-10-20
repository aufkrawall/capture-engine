#include "setup_actions.h"

#include "common/logging/logging.h"

#include <windows.h>

#include <cstddef>
#include <string_view>
#include <utility>

namespace ce::runtime {
namespace {

constexpr size_t kMaxClientNameStem = 40;

const char* ActionName(SetupAction action) {
    switch (action) {
        case SetupAction::SensorDriverInstall:
            return "sensor-driver-install";
        case SetupAction::SensorDriverUninstall:
            return "sensor-driver-uninstall";
        case SetupAction::ElevationServiceInstall:
            return "elevation-service-install";
        case SetupAction::ElevationServiceRemove:
            return "elevation-service-remove";
        case SetupAction::ClientAutostartOn:
            return "client-autostart-on";
        case SetupAction::ClientAutostartOff:
            return "client-autostart-off";
    }
    return "unknown";
}

bool IsKnownAction(SetupAction action) {
    return IsKnownSetupAction(static_cast<int32_t>(action));
}

// The PawnIO worker runs the sensor driver actions; the startup worker runs elevation service and autostart.
bool IsDriverAction(SetupAction action) {
    return action == SetupAction::SensorDriverInstall || action == SetupAction::SensorDriverUninstall;
}

bool IsRegistrationNameCharacter(wchar_t character) {
    return (character >= L'0' && character <= L'9') || (character >= L'a' && character <= L'z') ||
           (character >= L'A' && character <= L'Z') || character == L'.' || character == L'-' || character == L'_';
}

// "Client-" plus the executable's file name without extension, limited to the registration name alphabet. The prefix
// keeps a client's Run value apart from the application's own "CaptureEngine" registration.
std::wstring ClientRegistrationName(std::wstring_view executable) {
    const size_t separator = executable.find_last_of(L"\\/");
    std::wstring stem(executable.substr(separator == std::wstring_view::npos ? 0 : separator + 1));
    const size_t dot = stem.find_last_of(L'.');
    if (dot != std::wstring::npos && dot > 0)
        stem.resize(dot);
    if (stem.size() > kMaxClientNameStem)
        stem.resize(kMaxClientNameStem);
    for (wchar_t& character : stem) {
        if (!IsRegistrationNameCharacter(character))
            character = L'_';
    }
    return L"Client-" + stem;
}

ce::startup::AutostartTarget MakeClientTarget(std::wstring executable, std::wstring arguments) {
    ce::startup::AutostartTarget target;
    target.name = ClientRegistrationName(executable);
    target.executable = std::move(executable);
    target.arguments = std::move(arguments);
    return target;
}

unsigned long Dispatch(SetupBackend& backend, SetupAction action) {
    switch (action) {
        case SetupAction::SensorDriverInstall:
            return backend.RequestDriverInstall();
        case SetupAction::SensorDriverUninstall:
            return backend.RequestDriverUninstall();
        case SetupAction::ElevationServiceInstall:
            return backend.RequestElevationService(true);
        case SetupAction::ElevationServiceRemove:
            return backend.RequestElevationService(false);
        case SetupAction::ClientAutostartOn:
            return backend.RequestClientAutostart(backend.ClientTarget(), true);
        case SetupAction::ClientAutostartOff:
            return backend.RequestClientAutostart(backend.ClientTarget(), false);
    }
    return ERROR_INVALID_PARAMETER;
}

std::optional<unsigned long> TakeResult(SetupBackend& backend, SetupAction action) {
    return IsDriverAction(action) ? backend.TakeDriverSetupResult() : backend.TakeSetupResult();
}

std::optional<unsigned long> WaitResult(SetupBackend& backend, SetupAction action) {
    return IsDriverAction(action) ? backend.WaitDriverSetupResult() : backend.WaitSetupResult();
}

void LogRefused(uint64_t request, SetupAction action, unsigned long status) {
    LogWarn("[Setup] request=%llu action=%s result=%lu", static_cast<unsigned long long>(request), ActionName(action),
            status);
}

void LogStarted(uint64_t request, SetupAction action) {
    LogInfo("[Setup] request=%llu action=%s started", static_cast<unsigned long long>(request), ActionName(action));
}

void LogCompletion(uint64_t request, SetupAction action, unsigned long result) {
    LogInfo("[Setup] request=%llu action=%s result=%lu", static_cast<unsigned long long>(request), ActionName(action),
            result);
}

}  // namespace

bool IsKnownSetupAction(int32_t value) {
    return value >= static_cast<int32_t>(SetupAction::SensorDriverInstall) &&
           value <= static_cast<int32_t>(SetupAction::ClientAutostartOff);
}

ce::startup::AutostartTarget ClientAutostartTarget(std::wstring executable, std::wstring arguments) {
    return MakeClientTarget(std::move(executable), std::move(arguments));
}

SetupStatus ReadSetupStatus(SetupBackend& backend) {
    SetupStatus status;
    status.sensorDriverInstalled = backend.IsDriverInstalled();
    status.elevationServiceState = static_cast<int32_t>(backend.QueryElevationServiceState());
    const ce::startup::AutostartState autostart = backend.QueryClientAutostart(backend.ClientTarget());
    status.clientAutostart = autostart == ce::startup::AutostartState::UserRun ||
                             autostart == ce::startup::AutostartState::ElevatedTask;
    return status;
}

SetupActions::SetupActions(SetupBackend& backend) : backend_(backend) {}

unsigned long SetupActions::Start(uint64_t request, SetupAction action) {
    if (!IsKnownAction(action)) {
        LogRefused(request, action, ERROR_INVALID_PARAMETER);
        return ERROR_INVALID_PARAMETER;
    }
    if (inFlight_) {
        LogRefused(request, action, ERROR_BUSY);
        return ERROR_BUSY;
    }
    const unsigned long status = Dispatch(backend_, action);
    if (status != ERROR_SUCCESS) {
        LogRefused(request, action, status);
        return status;
    }
    inFlight_ = InFlight{request, action};
    LogStarted(request, action);
    return ERROR_SUCCESS;
}

std::vector<SetupCompletion> SetupActions::Collect() {
    std::vector<SetupCompletion> completions;
    if (!inFlight_)
        return completions;
    const std::optional<unsigned long> result = TakeResult(backend_, inFlight_->action);
    if (!result)
        return completions;
    completions.push_back(SetupCompletion{inFlight_->request, *result});
    LogCompletion(inFlight_->request, inFlight_->action, *result);
    inFlight_.reset();
    return completions;
}

SetupStatus SetupActions::Status() {
    return ReadSetupStatus(backend_);
}

bool SetupActions::Busy() const {
    return inFlight_.has_value();
}

std::optional<SetupCompletion> SetupActions::Shutdown() {
    if (!inFlight_)
        return std::nullopt;
    const InFlight pending = *inFlight_;
    const std::optional<unsigned long> result = WaitResult(backend_, pending.action);
    inFlight_.reset();
    if (!result) {
        LogWarn("[Setup] request=%llu action=%s abandoned at shutdown without a result",
                static_cast<unsigned long long>(pending.request), ActionName(pending.action));
        return std::nullopt;
    }
    LogCompletion(pending.request, pending.action, *result);
    return SetupCompletion{pending.request, *result};
}

}  // namespace ce::runtime
