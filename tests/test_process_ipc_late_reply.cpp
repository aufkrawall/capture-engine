#include <gtest/gtest.h>

#include <windows.h>

#include <functional>
#include <string>
#include <thread>

#include "../common/process_ipc_internal.h"

namespace {

// Drives a real ProcessIPCClient against an in-process fake child on the
// inherited endpoint. The fake child never answers a command until the test
// says so, so every "missed reply" below is deterministic: nothing depends on
// how fast either side runs.
class FakeChild {
public:
    explicit FakeChild(ProcessIPCClient& client) {
        std::wstring arguments;
        EXPECT_TRUE(client.PrepareChildEndpoint(endpoint_, arguments));
        const size_t noncePos = arguments.find(L"--ipc-nonce=");
        EXPECT_NE(noncePos, std::wstring::npos);
        const std::wstring hex = arguments.substr(noncePos + wcslen(L"--ipc-nonce="), nonce_.size() * 2);
        for (size_t index = 0; index < nonce_.size(); ++index)
            nonce_[index] = static_cast<uint8_t>(std::stoul(hex.substr(index * 2, 2), nullptr, 16));

        const ProcessMessage startup = BuildMessage(ProcessMessageKind::Startup, 0, ProcessMode::Media, 0,
                                                    GetCurrentProcessId(), nonce_, nullptr);
        EXPECT_TRUE(Write(startup));
        EXPECT_TRUE(client.CompleteChildSpawn(GetCurrentProcessId()));
    }

    ~FakeChild() {
        if (endpoint_ != INVALID_HANDLE_VALUE)
            CloseHandle(endpoint_);
    }

    bool ReadCommand(ProcessMessage& command) {
        DWORD bytesRead = 0;
        return ReadFile(endpoint_, &command, sizeof(command), &bytesRead, nullptr) != FALSE;
    }

    bool Reply(const ProcessMessage& command, ProcessResponse response) {
        return Write(BuildMessage(ProcessMessageKind::Response, static_cast<uint16_t>(response), ProcessMode::Media,
                                  command.sequence, GetCurrentProcessId(), nonce_, nullptr));
    }

private:
    bool Write(const ProcessMessage& message) {
        DWORD written = 0;
        return WriteFile(endpoint_, &message, message.totalSize, &written, nullptr) && written == message.totalSize;
    }

    HANDLE endpoint_ = INVALID_HANDLE_VALUE;
    ProcessChannelNonce nonce_{};
};

}  // namespace

TEST(ProcessIPCLateReplyTest, OnlyIdempotentCommandsSurviveOneMissedReply) {
    EXPECT_EQ(ClassifyReplyTimeout(ProcessCommand::ReloadConfig, false), ReplyTimeoutOutcome::kKeepChannel);
    EXPECT_EQ(ClassifyReplyTimeout(ProcessCommand::Ping, false), ReplyTimeoutOutcome::kKeepChannel);
    EXPECT_EQ(ClassifyReplyTimeout(ProcessCommand::ReloadConfig, true), ReplyTimeoutOutcome::kBreakChannel);
    EXPECT_EQ(ClassifyReplyTimeout(ProcessCommand::StartRecording, false), ReplyTimeoutOutcome::kBreakChannel);
    EXPECT_EQ(ClassifyReplyTimeout(ProcessCommand::StopRecording, false), ReplyTimeoutOutcome::kBreakChannel);
    EXPECT_EQ(ClassifyReplyTimeout(ProcessCommand::ToggleOverlay, false), ReplyTimeoutOutcome::kBreakChannel);
    EXPECT_EQ(ClassifyReplyTimeout(ProcessCommand::Shutdown, false), ReplyTimeoutOutcome::kBreakChannel);
}

// Regression: one ReloadConfig ack that missed the 1 s window closed the pipe,
// the media child took that as a fatal disconnect and stopped the live
// recording. The late ack must now be discarded and the channel stay usable.
TEST(ProcessIPCLateReplyTest, LateReloadAckIsDiscardedAndTheChannelKeepsWorking) {
    ProcessIPCClient client(ProcessMode::Media);
    FakeChild child(client);
    ASSERT_TRUE(client.IsConnected());

    HANDLE answerLate = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    ASSERT_NE(answerLate, nullptr);
    bool childOk = true;
    ProcessCommand secondCommand = ProcessCommand::None;
    std::thread childThread([&]() {
        ProcessMessage reload{};
        childOk = child.ReadCommand(reload);
        WaitForSingleObject(answerLate, INFINITE);
        childOk = childOk && child.Reply(reload, ProcessResponse::Ack);  // arrives after the client gave up
        ProcessMessage ping{};
        childOk = childOk && child.ReadCommand(ping);
        secondCommand = static_cast<ProcessCommand>(ping.opcode);
        childOk = childOk && child.Reply(ping, ProcessResponse::Pong);
    });

    EXPECT_FALSE(client.SendCommand(ProcessCommand::ReloadConfig, nullptr, nullptr, 20));
    EXPECT_TRUE(client.IsConnected()) << "one missed ReloadConfig ack must not tear the channel down";
    SetEvent(answerLate);

    ProcessResponse response = ProcessResponse::None;
    EXPECT_TRUE(client.SendCommand(ProcessCommand::Ping, nullptr, &response, 10000));
    EXPECT_EQ(response, ProcessResponse::Pong) << "the stale Ack must not be taken as the Ping reply";
    EXPECT_TRUE(client.IsConnected());

    childThread.join();
    CloseHandle(answerLate);
    EXPECT_TRUE(childOk);
    EXPECT_EQ(secondCommand, ProcessCommand::Ping);
}

TEST(ProcessIPCLateReplyTest, SecondMissedReplyStillBreaksTheChannel) {
    ProcessIPCClient client(ProcessMode::Media);
    FakeChild child(client);
    ASSERT_TRUE(client.IsConnected());

    // The fake child reads both commands and answers neither.
    std::thread childThread([&]() {
        ProcessMessage command{};
        child.ReadCommand(command);
        child.ReadCommand(command);
    });
    EXPECT_FALSE(client.SendCommand(ProcessCommand::ReloadConfig, nullptr, nullptr, 20));
    EXPECT_TRUE(client.IsConnected());
    EXPECT_FALSE(client.SendCommand(ProcessCommand::Ping, nullptr, nullptr, 20));
    EXPECT_FALSE(client.IsConnected()) << "a child that answers nothing is not serving its pipe";
    childThread.join();
}

TEST(ProcessIPCLateReplyTest, StateChangingCommandTimeoutStillBreaksTheChannel) {
    ProcessIPCClient client(ProcessMode::Media);
    FakeChild child(client);
    ASSERT_TRUE(client.IsConnected());
    std::thread childThread([&]() {
        ProcessMessage command{};
        child.ReadCommand(command);
    });
    EXPECT_FALSE(client.SendCommand(ProcessCommand::StartRecording, nullptr, nullptr, 20));
    EXPECT_FALSE(client.IsConnected());
    childThread.join();
}
