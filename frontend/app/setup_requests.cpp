// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall
#include "frontend/app/setup_requests.h"

#include <algorithm>
#include <string>
#include <thread>

namespace ce::frontend {
namespace {

// Records a line in the runtime's log (the frontend's own lines follow the runtime's log, as everywhere else).
void Log(cengine::Runtime& runtime, cengine::LogLevel level, const std::string& text) {
    if (runtime)
        (void)runtime.log(level, text);
}

// The failure's text: the Windows code when the runtime reported one, otherwise the plain sentence.
std::wstring SettingFailedText(const char* message) {
    unsigned long code = 0;
    if (message && WindowsErrorOf(message, code))
        return L"The setting could not be applied (Windows error " + std::to_wstring(code) +
               L"). The previous preference was retained.";
    return L"The setting could not be applied. The previous preference was retained.";
}

}  // namespace

TraySetupState SetupRequests::State(const cengine::Runtime& runtime) const {
    // An unavailable runtime leaves the status zeroed: nothing installed, nothing checked.
    ce_setup_status_t status{};
    (void)runtime.setupStatus(status);
    return BuildTraySetupState(status, StartupBusy());
}

bool SetupRequests::StartupBusy() const {
    return std::any_of(pending_.begin(), pending_.end(), [](const Pending& item) { return item.kind == Kind::Startup; });
}

bool SetupRequests::Start(cengine::Runtime& runtime, int32_t action, Kind kind, TrayCommand command) {
    ce_request_t request = 0;
    const cengine::Status admitted = runtime.setup(action, &request);
    if (!admitted) {
        Log(runtime, cengine::LogLevel::Warning, std::string("[Setup] action ") + std::to_string(action) +
                                                     " not admitted: " + admitted.message());
        return false;
    }
    pending_.push_back(Pending{request, kind, command});
    Log(runtime, cengine::LogLevel::Info, "[Setup] action " + std::to_string(action) + " requested");
    return true;
}

void SetupRequests::Command(cengine::Runtime& runtime, TrayCommand command) {
    if (!runtime)
        return;
    ce_setup_status_t status{};
    const bool known = static_cast<bool>(runtime.setupStatus(status));
    const TraySetupState shown = BuildTraySetupState(status, false);
    switch (command) {
        case TrayCommand::PawnIoInstall:
            if (known && status.sensor_driver_installed != 0) {
                ShowDriverAlreadyInstalled();
                return;
            }
            Start(runtime, CE_SETUP_SENSOR_DRIVER_INSTALL, Kind::Driver, command);
            return;
        case TrayCommand::PawnIoUninstall:
            if (known && status.sensor_driver_installed == 0) {
                ShowDriverNotInstalled();
                return;
            }
            if (!ConfirmDriverUninstall())
                return;
            Start(runtime, CE_SETUP_SENSOR_DRIVER_UNINSTALL, Kind::Driver, command);
            return;
        case TrayCommand::ToggleService:
            if (StartupBusy())
                return;
            Start(runtime, shown.serviceChecked ? CE_SETUP_ELEVATION_SERVICE_REMOVE : CE_SETUP_ELEVATION_SERVICE_INSTALL,
                  Kind::Startup, command);
            return;
        case TrayCommand::ToggleElevated:
            if (StartupBusy())
                return;
            Start(runtime, shown.elevatedChecked ? CE_SETUP_LAUNCH_ELEVATED_OFF : CE_SETUP_LAUNCH_ELEVATED_ON,
                  Kind::Startup, command);
            return;
        case TrayCommand::ToggleAutostart:
            if (StartupBusy())
                return;
            Start(runtime, shown.autostartChecked ? CE_SETUP_CLIENT_AUTOSTART_OFF : CE_SETUP_CLIENT_AUTOSTART_ON,
                  Kind::Startup, command);
            return;
    }
}

bool SetupRequests::Completed(cengine::Runtime& runtime, uint64_t request, ce_status_t status, const char* message) {
    const auto found = std::find_if(pending_.begin(), pending_.end(),
                                    [request](const Pending& item) { return item.request == request; });
    if (found == pending_.end())
        return false;
    const Pending done = *found;
    pending_.erase(found);
    switch (done.kind) {
        case Kind::Driver:
            if (done.command == TrayCommand::PawnIoInstall) {
                if (status == CE_OK) {
                    ce_setup_status_t current{};
                    const bool known = static_cast<bool>(runtime.setupStatus(current));
                    OfferRestartAsAdministrator(known && current.elevation_service_preferred != 0);
                } else {
                    Log(runtime, cengine::LogLevel::Warning,
                        "[PawnIO] Driver installation did not complete (status=" + std::to_string(status) + ")");
                    ShowDriverInstallNotCompleted();
                }
            } else if (status == CE_OK) {
                ShowDriverRemoved();
            } else {
                Log(runtime, cengine::LogLevel::Warning,
                    "[PawnIO] Driver uninstallation did not complete (status=" + std::to_string(status) + ")");
                ShowDriverUninstallNotCompleted();
            }
            return true;
        case Kind::Offer:
            if (status == CE_OK) {
                Log(runtime, cengine::LogLevel::Info, "[PawnIO] Driver installed; offering restart as administrator");
                ce_setup_status_t current{};
                const bool known = static_cast<bool>(runtime.setupStatus(current));
                OfferRestartAsAdministrator(known && current.elevation_service_preferred != 0);
            } else {
                Log(runtime, cengine::LogLevel::Warning,
                    "[PawnIO] Driver installation did not complete (status=" + std::to_string(status) + ")");
            }
            return true;
        case Kind::Startup:
            if (status == CE_OK) {
                Log(runtime, cengine::LogLevel::Info,
                    "[Startup] Tray preference applied; process elevation changes on the next launch");
            } else if (status == CE_E_CANCELED) {
                Log(runtime, cengine::LogLevel::Info, "[Startup] Setup canceled; previous preferences retained");
            } else if (status == CE_E_BUSY) {
                Log(runtime, cengine::LogLevel::Info, "[Startup] Setup request ignored: another setup action is in flight");
            } else {
                Log(runtime, cengine::LogLevel::Warning,
                    std::string("[Startup] Setup failed (status=") + std::to_string(status) +
                        "); previous preferences retained");
                ShowSettingFailed(SettingFailedText(message).c_str());
            }
            return true;
        case Kind::Dismiss:
            if (status != CE_OK)
                Log(runtime, cengine::LogLevel::Warning,
                    "[PawnIO] Recording the driver offer answer failed (status=" + std::to_string(status) + ")");
            return true;
    }
    return true;
}

void SetupRequests::OnReady(cengine::Runtime& runtime) {
    ce_setup_status_t status{};
    if (!runtime.setupStatus(status) || status.sensor_driver_offer == 0)
        return;
    Log(runtime, cengine::LogLevel::Info, "[PawnIO] Driver absent; offering optional installation once");
    // The prompt waits for the user on its own thread; the answer comes back as a message to this thread.
    const DWORD frontendThread = GetCurrentThreadId();
    std::thread([frontendThread]() {
        const OfferAnswer answer = AskDriverOffer();
        PostThreadMessageW(frontendThread, kOfferAnswerMessage, static_cast<WPARAM>(answer), 0);
    }).detach();
}

void SetupRequests::OfferAnswered(cengine::Runtime& runtime, OfferAnswer answer) {
    switch (answer) {
        case OfferAnswer::Install:
            Start(runtime, CE_SETUP_SENSOR_DRIVER_INSTALL, Kind::Offer, TrayCommand::PawnIoInstall);
            return;
        case OfferAnswer::DontAskAgain:
            Start(runtime, CE_SETUP_SENSOR_OFFER_DISMISS, Kind::Dismiss, TrayCommand::PawnIoInstall);
            return;
        case OfferAnswer::NotNow:
            Log(runtime, cengine::LogLevel::Info,
                "[PawnIO] Driver installation deferred; CPU sensors stay unavailable this run");
            return;
        case OfferAnswer::None:
            return;
    }
}

}  // namespace ce::frontend
