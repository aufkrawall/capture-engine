// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall
#pragma once

namespace ce::ipc {
// Local classification of a command attempt, not a wire/shared-memory enum.
// Unknown acknowledgement is distinct from an explicit rejection or acceptance.
enum class CommandOutcome { Accepted, Rejected, AcknowledgementUnknown };
}  // namespace ce::ipc
