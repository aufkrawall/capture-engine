// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall
#pragma once

// The frontend's view of the platform setup: the tray's items and status texts, computed from the runtime's setup
// status (ce_setup_status_t). Pure: no OS or runtime calls, so the tests cover it directly.

#include "include/cengine/cengine.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace ce::frontend {

// The tray's setting items. The tray reports what the user picked; the setup requests are issued by SetupRequests.
enum class TrayCommand : uint8_t {
    PawnIoInstall,
    PawnIoUninstall,
    ToggleService,
    ToggleElevated,
    ToggleAutostart,
};

// What the tray draws. Checkmarks come from the preferences and the registrations the runtime reports.
struct TraySetupState {
    bool pawnIoInstalled = false;
    bool serviceChecked = false;
    bool elevatedChecked = false;
    bool autostartChecked = false;
    bool busy = false;  // a startup-family request is in flight: the setting items are grayed
    std::wstring serviceText = L"Use elevation service (not installed)";
};

// "Use elevation service (running)" and its siblings, for ce_setup_status_t::elevation_service_state.
std::wstring ElevationServiceLabel(int32_t state);

// Whether the elevation service is registered (installed or running).
bool ElevationServiceRegistered(int32_t state);

// The tray's items for a status snapshot. The service item is checked when its preference is on and the service is
// registered, as the tray always showed it.
TraySetupState BuildTraySetupState(const ce_setup_status_t& status, bool startupBusy);

// The Win32 code of a setup failure message ("Windows error N"). False when the message carries no code.
bool WindowsErrorOf(std::string_view message, unsigned long& code);

}  // namespace ce::frontend
