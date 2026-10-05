#include <gtest/gtest.h>

#include "captureengine/app/child_recording_stop.h"

namespace {
struct Child {
    bool connected = true;
    bool transportAccepted = true;
    ProcessResponse response = ProcessResponse::Ack;
    int commands = 0;
    uint32_t timeout = 0;
    bool IsConnected() const { return connected; }
    bool SendCommand(ProcessCommand command, const char* payload, ProcessResponse* result, uint32_t timeoutMs) {
        EXPECT_EQ(command, ProcessCommand::StopRecording);
        EXPECT_EQ(payload, nullptr);
        ++commands;
        timeout = timeoutMs;
        *result = response;
        return transportAccepted;
    }
};
using ce::controller::CommandOutcome;
using ce::controller::detail::RequestChildRecordingStop;
}

TEST(ChildRecordingStopTest, MissingOrDisconnectedEndpointRetainsAcknowledgementUncertainty) {
    EXPECT_EQ(RequestChildRecordingStop<Child>(nullptr, 1000), CommandOutcome::AcknowledgementUnknown);
    Child child;
    child.connected = false;
    EXPECT_EQ(RequestChildRecordingStop(&child, 1000), CommandOutcome::AcknowledgementUnknown);
    EXPECT_EQ(child.commands, 0);
}

TEST(ChildRecordingStopTest, TransportFailureCannotBecomeAChildRejectionOrAcceptance) {
    Child child;
    child.transportAccepted = false;
    for (auto response : {ProcessResponse::Error, ProcessResponse::Ack, ProcessResponse::RecordingStopped}) {
        child.response = response;
        EXPECT_EQ(RequestChildRecordingStop(&child, 5000), CommandOutcome::AcknowledgementUnknown);
    }
    EXPECT_EQ(child.commands, 3);
    EXPECT_EQ(child.timeout, 5000u);
}

TEST(ChildRecordingStopTest, ReceivedErrorIsRejectedAndAcknowledgementsPreserveAcceptance) {
    Child child;
    child.response = ProcessResponse::Error;
    EXPECT_EQ(RequestChildRecordingStop(&child, 1000), CommandOutcome::Rejected);
    for (auto response : {ProcessResponse::Ack, ProcessResponse::RecordingStopped}) {
        child.response = response;
        EXPECT_EQ(RequestChildRecordingStop(&child, 1000), CommandOutcome::Accepted);
    }
    EXPECT_EQ(child.commands, 3);
    EXPECT_EQ(child.timeout, 1000u);
}
