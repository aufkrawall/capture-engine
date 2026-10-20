// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall
#pragma once

// The PawnIO dialogs of the app: the startup offer, the tray's install and uninstall answers and the restart-as-
// administrator offer. Texts and flows are the ones the controller always showed. The driver itself is changed only
// through the runtime's setup API (ce_setup_request); nothing here touches the driver.

#include <windows.h>

namespace ce::frontend {

enum class OfferAnswer : int { Install = 1, NotNow = 2, DontAskAgain = 3, None = 0 };

// The startup offer. Blocks until answered: call it from a dedicated thread.
OfferAnswer AskDriverOffer();

// After a successful install: the restart-as-administrator offer. `serviceEnabled` is the elevation service preference.
void OfferRestartAsAdministrator(bool serviceEnabled);

void ShowDriverAlreadyInstalled();
void ShowDriverInstallNotCompleted();
void ShowDriverNotInstalled();
bool ConfirmDriverUninstall();
void ShowDriverRemoved();
void ShowDriverUninstallNotCompleted();
void ShowSettingFailed(const wchar_t* text);

}  // namespace ce::frontend
