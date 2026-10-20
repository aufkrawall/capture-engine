// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall
#include "frontend/app/setup_model.h"

#include <cctype>

namespace ce::frontend {
namespace {

// ce_setup_status_t::elevation_service_state values (include/cengine/cengine.h).
constexpr int32_t kServiceInstalled = 1;
constexpr int32_t kServiceRunning = 2;

}  // namespace

std::wstring ElevationServiceLabel(int32_t state) {
    if (state == kServiceRunning)
        return L"Use elevation service (running)";
    if (state == kServiceInstalled)
        return L"Use elevation service (installed, stopped)";
    return L"Use elevation service (not installed)";
}

bool ElevationServiceRegistered(int32_t state) {
    return state == kServiceInstalled || state == kServiceRunning;
}

TraySetupState BuildTraySetupState(const ce_setup_status_t& status, bool startupBusy) {
    TraySetupState state;
    state.pawnIoInstalled = status.sensor_driver_installed != 0;
    state.serviceChecked = status.elevation_service_preferred != 0 &&
                           ElevationServiceRegistered(status.elevation_service_state);
    state.elevatedChecked = status.launch_elevated != 0;
    state.autostartChecked = status.client_autostart != 0;
    state.busy = startupBusy;
    state.serviceText = ElevationServiceLabel(status.elevation_service_state);
    return state;
}

bool WindowsErrorOf(std::string_view message, unsigned long& code) {
    constexpr std::string_view kPrefix = "Windows error ";
    const size_t at = message.find(kPrefix);
    if (at == std::string_view::npos)
        return false;
    size_t index = at + kPrefix.size();
    if (index >= message.size() || !std::isdigit(static_cast<unsigned char>(message[index])))
        return false;
    unsigned long value = 0;
    for (; index < message.size() && std::isdigit(static_cast<unsigned char>(message[index])); ++index) {
        value = value * 10 + static_cast<unsigned long>(message[index] - '0');
    }
    code = value;
    return true;
}

}  // namespace ce::frontend
