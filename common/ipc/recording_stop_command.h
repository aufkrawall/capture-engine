// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall
#pragma once

#include "command_outcome.h"
#include "process_ipc.h"

namespace ce::ipc {
// Applies the existing stop acknowledgement contract to native or controlled
// child channels without importing recording state or controller ownership.
template <class ChildChannel>
CommandOutcome RequestChildRecordingStop(ChildChannel* client, uint32_t timeoutMs) {
    if (!client || !client->IsConnected())
        return CommandOutcome::AcknowledgementUnknown;
    ProcessResponse response = ProcessResponse::Error;
    if (!client->SendCommand(ProcessCommand::StopRecording, nullptr, &response, timeoutMs))
        return CommandOutcome::AcknowledgementUnknown;
    return response == ProcessResponse::Error ? CommandOutcome::Rejected : CommandOutcome::Accepted;
}
}  // namespace ce::ipc
