#include <gtest/gtest.h>

#include "common/ipc/role_channel_memory.h"

#include <atomic>
#include <cstring>
#include <string>
#include <thread>

namespace {
using namespace ce::role;

constexpr uint32_t kControllerPid = 4242;
constexpr DWORD kReplyTimeoutMs = 10000;  // only bounds a broken test; a healthy reply arrives at once

// Serves commands on its own thread the way a role loop does: wait for a command, poll, reply. The loop ends
// when the host disconnects, which every test does before returning.
class ServingRole {
public:
    explicit ServingRole(RoleChannel& channel) : channel_(channel) {}
    void Start() {
        thread_ = std::thread([this] { Run(); });
    }
    void Join() {
        if (thread_.joinable())
            thread_.join();
    }
    // Acks or answers each command by its type; Shutdown ends the loop after its Ack.
    std::atomic<bool> sawAudioOnly{false};
    std::atomic<uint32_t> commandsServed{0};

private:
    static ProcessResponse DefaultReply(ProcessCommand command) {
        switch (command) {
            case ProcessCommand::Ping:
                return ProcessResponse::Pong;
            case ProcessCommand::ReloadConfig:
            case ProcessCommand::Shutdown:
                return ProcessResponse::Ack;
            case ProcessCommand::StartRecording:
                return ProcessResponse::RecordingStarted;
            default:
                return ProcessResponse::Ack;
        }
    }
    void Run() {
        while (!channel_.HasFatalDisconnect()) {
            if (!channel_.WaitForCommand(kReplyTimeoutMs))
                continue;
            ProcessCommand command = ProcessCommand::None;
            char payload[PROCESS_MAX_PAYLOAD]{};
            while (channel_.PollCommand(command, payload, sizeof(payload))) {
                ++commandsServed;
                if (command == ProcessCommand::StartRecording && std::strcmp(payload, "audio_only") == 0)
                    sawAudioOnly = true;
                const ProcessResponse reply = DefaultReply(command);
                channel_.SendResponse(reply, nullptr);
                if (command == ProcessCommand::Shutdown)
                    return;
            }
        }
    }

    RoleChannel& channel_;
    std::thread thread_;
};

TEST(InMemoryChannelTest, BothEndsStartConnectedAndTheRoleKnowsItsController) {
    auto pair = MakeInMemoryChannel(ProcessMode::Inject, kControllerPid);
    ASSERT_TRUE(pair.host);
    ASSERT_TRUE(pair.role);
    EXPECT_TRUE(pair.host->IsConnected());
    EXPECT_TRUE(pair.role->IsConnected());
    EXPECT_FALSE(pair.role->HasFatalDisconnect());
    EXPECT_EQ(pair.role->ControllerPid(), kControllerPid);
}

TEST(InMemoryChannelTest, RoundTripAcksPongsAndShutdownStopTheRole) {
    auto pair = MakeInMemoryChannel(ProcessMode::Media, kControllerPid);
    ServingRole role(*pair.role);
    role.Start();

    ProcessResponse response = ProcessResponse::None;
    EXPECT_TRUE(pair.host->SendCommand(ProcessCommand::Ping, nullptr, &response, kReplyTimeoutMs));
    EXPECT_EQ(response, ProcessResponse::Pong);
    EXPECT_TRUE(pair.host->SendCommand(ProcessCommand::ReloadConfig, nullptr, &response, kReplyTimeoutMs));
    EXPECT_EQ(response, ProcessResponse::Ack);
    EXPECT_TRUE(pair.host->SendCommand(ProcessCommand::Shutdown, nullptr, &response, kReplyTimeoutMs));
    EXPECT_EQ(response, ProcessResponse::Ack);
    role.Join();  // the loop returns after the Shutdown Ack
    EXPECT_EQ(role.commandsServed.load(), 3u);
    EXPECT_TRUE(pair.host->IsConnected());
}

TEST(InMemoryChannelTest, AnErrorReplyIsAcceptedForAnyCommand) {
    auto pair = MakeInMemoryChannel(ProcessMode::Media, kControllerPid);
    std::thread role([&pair] {
        ProcessCommand polled = ProcessCommand::None;
        while (!pair.role->WaitForCommand(kReplyTimeoutMs)) {
        }
        EXPECT_TRUE(pair.role->PollCommand(polled));
        EXPECT_EQ(polled, ProcessCommand::StopRecording);
        EXPECT_TRUE(pair.role->SendResponse(ProcessResponse::Error, "refused"));
    });
    ProcessResponse response = ProcessResponse::None;
    EXPECT_TRUE(pair.host->SendCommand(ProcessCommand::StopRecording, nullptr, &response, kReplyTimeoutMs));
    EXPECT_EQ(response, ProcessResponse::Error);
    role.join();
    EXPECT_TRUE(pair.host->IsConnected());
}

TEST(InMemoryChannelTest, PayloadRulesHoldOnBothEnds) {
    auto pair = MakeInMemoryChannel(ProcessMode::Media, kControllerPid);
    ServingRole role(*pair.role);
    role.Start();

    // Only StartRecording may carry a payload, and only audio_only.
    EXPECT_FALSE(pair.host->SendCommand(ProcessCommand::Ping, "payload", nullptr, 0));
    EXPECT_TRUE(pair.host->IsConnected()) << "a rejected payload is refused before anything is sent";
    std::string tooLong(PROCESS_MAX_PAYLOAD, 'x');
    EXPECT_FALSE(pair.host->SendCommand(ProcessCommand::StartRecording, tooLong.c_str(), nullptr, 0));
    ProcessResponse response = ProcessResponse::None;
    EXPECT_TRUE(pair.host->SendCommand(ProcessCommand::StartRecording, "audio_only", &response, kReplyTimeoutMs));
    EXPECT_EQ(response, ProcessResponse::RecordingStarted);
    EXPECT_TRUE(role.sawAudioOnly.load());
    EXPECT_TRUE(pair.host->SendCommand(ProcessCommand::Shutdown, nullptr, &response, kReplyTimeoutMs));
    role.Join();
}

TEST(InMemoryChannelTest, TheRoleCannotReplyWithAPayloadThatIsNotAnError) {
    auto pair = MakeInMemoryChannel(ProcessMode::Inject, kControllerPid);
    auto& role = *pair.role;
    std::thread host([&pair] {
        ProcessResponse response = ProcessResponse::None;
        EXPECT_TRUE(pair.host->SendCommand(ProcessCommand::ToggleOverlay, nullptr, &response, kReplyTimeoutMs));
        EXPECT_EQ(response, ProcessResponse::Ack);
    });
    ProcessCommand command = ProcessCommand::None;
    while (!role.WaitForCommand(kReplyTimeoutMs)) {
    }
    ASSERT_TRUE(role.PollCommand(command));
    EXPECT_FALSE(role.SendResponse(ProcessResponse::Ack, "not allowed"));
    EXPECT_FALSE(role.SendResponse(ProcessResponse::None, nullptr));
    EXPECT_TRUE(role.SendResponse(ProcessResponse::Ack, nullptr));
    host.join();
}

TEST(InMemoryChannelTest, ALateReplyToAToleratedCommandIsDiscardedAndTheChannelStays) {
    auto pair = MakeInMemoryChannel(ProcessMode::Media, kControllerPid);
    // The host gives up at once on ReloadConfig (tolerated): the role has not polled it yet.
    EXPECT_FALSE(pair.host->SendCommand(ProcessCommand::ReloadConfig, nullptr, nullptr, 0));
    EXPECT_TRUE(pair.host->IsConnected());

    ServingRole role(*pair.role);
    role.Start();
    // The next command's reply is in order: the late Ack to ReloadConfig is read and dropped before the Pong.
    ProcessResponse response = ProcessResponse::None;
    EXPECT_TRUE(pair.host->SendCommand(ProcessCommand::Ping, nullptr, &response, kReplyTimeoutMs));
    EXPECT_EQ(response, ProcessResponse::Pong);
    EXPECT_TRUE(pair.host->IsConnected());
    EXPECT_TRUE(pair.host->SendCommand(ProcessCommand::Shutdown, nullptr, &response, kReplyTimeoutMs));
    role.Join();
}

TEST(InMemoryChannelTest, ASecondMissedReplyBreaksTheChannel) {
    auto pair = MakeInMemoryChannel(ProcessMode::Media, kControllerPid);
    // No role serves the channel: the first Ping miss is tolerated, the second breaks it.
    EXPECT_FALSE(pair.host->SendCommand(ProcessCommand::Ping, nullptr, nullptr, 0));
    EXPECT_TRUE(pair.host->IsConnected());
    EXPECT_FALSE(pair.host->SendCommand(ProcessCommand::Ping, nullptr, nullptr, 0));
    EXPECT_FALSE(pair.host->IsConnected());
    EXPECT_TRUE(pair.role->HasFatalDisconnect()) << "a broken channel tells the role to stop";
    EXPECT_FALSE(pair.role->IsConnected());
    EXPECT_FALSE(pair.host->SendCommand(ProcessCommand::Ping, nullptr, nullptr, kReplyTimeoutMs));
}

TEST(InMemoryChannelTest, AMissedStateChangingReplyBreaksAtOnce) {
    auto pair = MakeInMemoryChannel(ProcessMode::Media, kControllerPid);
    EXPECT_FALSE(pair.host->SendCommand(ProcessCommand::StartRecording, nullptr, nullptr, 0));
    EXPECT_FALSE(pair.host->IsConnected());
    EXPECT_TRUE(pair.role->HasFatalDisconnect());
}

TEST(InMemoryChannelTest, AnInvalidReplyBreaksTheChannel) {
    auto pair = MakeInMemoryChannel(ProcessMode::Media, kControllerPid);
    std::thread host([&pair] {
        ProcessResponse response = ProcessResponse::None;
        EXPECT_FALSE(pair.host->SendCommand(ProcessCommand::ReloadConfig, nullptr, &response, kReplyTimeoutMs));
    });
    ProcessCommand command = ProcessCommand::None;
    while (!pair.role->WaitForCommand(kReplyTimeoutMs)) {
    }
    ASSERT_TRUE(pair.role->PollCommand(command));
    // Pong answers no ReloadConfig: the host rejects the reply and the channel is gone for both ends.
    EXPECT_TRUE(pair.role->SendResponse(ProcessResponse::Pong, nullptr));
    host.join();
    EXPECT_FALSE(pair.host->IsConnected());
    EXPECT_TRUE(pair.role->HasFatalDisconnect());
}

TEST(InMemoryChannelTest, AHostDisconnectWakesAWaitingRoleAndStopsItsPolling) {
    auto pair = MakeInMemoryChannel(ProcessMode::Inject, kControllerPid);
    std::thread role([&pair] {
        // Blocks on the command event with a timeout no test should reach: only the disconnect ends the wait.
        EXPECT_TRUE(pair.role->WaitForCommand(kReplyTimeoutMs));
        ProcessCommand command = ProcessCommand::None;
        EXPECT_FALSE(pair.role->PollCommand(command));
        EXPECT_TRUE(pair.role->HasFatalDisconnect());
    });
    pair.host->Disconnect();
    role.join();
    EXPECT_FALSE(pair.host->IsConnected());
    EXPECT_FALSE(pair.role->IsConnected());
    EXPECT_FALSE(pair.host->SendCommand(ProcessCommand::Ping, nullptr, nullptr, 0));
}

TEST(InMemoryChannelTest, ADestroyedRoleEndFailsAPendingCommandAtOnce) {
    auto pair = MakeInMemoryChannel(ProcessMode::Inject, kControllerPid);
    std::thread host([&pair] {
        // The role end is gone before, or while, this command waits. Either way the answer is a refusal.
        EXPECT_FALSE(pair.host->SendCommand(ProcessCommand::Ping, nullptr, nullptr, kReplyTimeoutMs));
    });
    pair.role.reset();
    host.join();
    EXPECT_FALSE(pair.host->IsConnected());
}

TEST(InMemoryChannelTest, AReplyQueuedBeforeTheRoleEndLeavesIsStillDelivered) {
    // A role replies and returns at once, as a role thread does. The host must still read that reply: a pipe keeps
    // what its writer wrote before the writer closed. The command is queued before the role polls it, so the host
    // has passed its admission check before the role end can exist.
    auto pair = MakeInMemoryChannel(ProcessMode::Inject, kControllerPid);
    std::thread host([&pair] {
        ProcessResponse response = ProcessResponse::None;
        EXPECT_TRUE(pair.host->SendCommand(ProcessCommand::Shutdown, nullptr, &response, kReplyTimeoutMs));
        EXPECT_EQ(response, ProcessResponse::Ack);
    });
    ProcessCommand command = ProcessCommand::None;
    while (!pair.role->WaitForCommand(kReplyTimeoutMs)) {
    }
    ASSERT_TRUE(pair.role->PollCommand(command));
    ASSERT_TRUE(pair.role->SendResponse(ProcessResponse::Ack, nullptr));
    pair.role.reset();
    host.join();
    EXPECT_FALSE(pair.host->IsConnected()) << "the role end is gone: the next command is refused";
}

TEST(InMemoryChannelTest, ADestroyedHostEndIsAFatalDisconnectForTheRole) {
    auto pair = MakeInMemoryChannel(ProcessMode::Media, kControllerPid);
    RoleChannel* role = pair.role.get();
    pair.host.reset();
    EXPECT_TRUE(role->HasFatalDisconnect());
    EXPECT_FALSE(role->IsConnected());
    EXPECT_TRUE(role->WaitForCommand(0));
}

TEST(InMemoryChannelTest, ARoleReplyWhileTheHostWaitsNeverDeadlocks) {
    // The host waits with the reply window still open while the role is already blocked in WaitForCommand;
    // the role's reply must go through without the host having to release anything.
    auto pair = MakeInMemoryChannel(ProcessMode::Media, kControllerPid);
    ServingRole role(*pair.role);
    role.Start();
    ProcessResponse response = ProcessResponse::None;
    for (int i = 0; i < 64; ++i) {
        ASSERT_TRUE(pair.host->SendCommand(ProcessCommand::Ping, nullptr, &response, kReplyTimeoutMs));
        EXPECT_EQ(response, ProcessResponse::Pong);
    }
    EXPECT_TRUE(pair.host->SendCommand(ProcessCommand::Shutdown, nullptr, &response, kReplyTimeoutMs));
    role.Join();
}

}  // namespace
