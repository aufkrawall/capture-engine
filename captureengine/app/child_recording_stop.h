#pragma once

#include "recording_session.h"
#include "common/ipc/process_ipc.h"

namespace ce::controller::detail {
// Private recording command adapter, used by the real process client and
// controlled unit clients. It exposes no mapping or borrowed transport payload.
template<class ChildChannel>
CommandOutcome RequestChildRecordingStop(ChildChannel* client, uint32_t timeoutMs) {
    if (!client || !client->IsConnected()) return CommandOutcome::AcknowledgementUnknown;
    ProcessResponse response = ProcessResponse::Error;
    if (!client->SendCommand(ProcessCommand::StopRecording, nullptr, &response, timeoutMs))
        return CommandOutcome::AcknowledgementUnknown;
    return response == ProcessResponse::Error ? CommandOutcome::Rejected : CommandOutcome::Accepted;
}
}  // namespace ce::controller::detail
