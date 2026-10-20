// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall
#pragma once

// What the app asks the runtime for in its setup (the tray's items and the startup driver offer) and what each answer
// does: the dialogs and the log lines. A request is tracked here until its COMMAND_COMPLETED arrives. Everything runs on
// the frontend's thread; the offer's prompt thread only posts its answer back (kOfferAnswerMessage).

#include "include/cengine/cengine.hpp"
#include "frontend/app/driver_prompts.h"
#include "frontend/app/setup_model.h"

#include <windows.h>

#include <cstdint>
#include <vector>

namespace ce::frontend {

// The message the offer's prompt thread posts to the frontend thread (wParam: OfferAnswer).
inline constexpr UINT kOfferAnswerMessage = WM_APP + 0x31;

class SetupRequests {
public:
    // The tray's items from the runtime's status. An unavailable runtime reads as nothing installed and nothing checked.
    TraySetupState State(const cengine::Runtime& runtime) const;

    // A tray item was chosen. Pre-checks what the item means (installed or not, the current checkmark) before a request.
    void Command(cengine::Runtime& runtime, TrayCommand command);

    // A COMMAND_COMPLETED. False when the request is not one of the frontend's.
    bool Completed(cengine::Runtime& runtime, uint64_t request, ce_status_t status, const char* message);

    // READY: starts the startup offer when the runtime reports that it is due.
    void OnReady(cengine::Runtime& runtime);

    // The offer's answer, posted back to the frontend thread.
    void OfferAnswered(cengine::Runtime& runtime, OfferAnswer answer);

private:
    enum class Kind : uint8_t { Driver, Startup, Offer, Dismiss };
    struct Pending {
        uint64_t request = 0;
        Kind kind = Kind::Startup;
        TrayCommand command = TrayCommand::PawnIoInstall;
    };

    bool Start(cengine::Runtime& runtime, int32_t action, Kind kind, TrayCommand command);
    bool StartupBusy() const;

    std::vector<Pending> pending_;
};

}  // namespace ce::frontend
