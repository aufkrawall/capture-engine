#include <gtest/gtest.h>
#include <chrono>
#include <thread>
#include "../common/process_ipc.h"

TEST(ProcessIPCTest, PollCommandCompletesPendingConnectionAcrossCalls) {
    ProcessIPCServer server(ProcessMode::Media);
    if (!server.Init()) {
        GTEST_SKIP() << "Media IPC pipe is already in use in this shared environment";
    }

    ProcessCommand command = ProcessCommand::None;
    EXPECT_FALSE(server.PollCommand(command));

    ProcessIPCClient client(ProcessMode::Media);
    ASSERT_TRUE(client.Connect(1000));

    ProcessResponse response = ProcessResponse::None;
    bool sendOk = false;
    std::thread sender([&]() { sendOk = client.SendCommand(ProcessCommand::Ping, nullptr, &response, 1000); });

    bool received = false;
    for (int attempt = 0; attempt < 100; ++attempt) {
        if (server.PollCommand(command)) {
            received = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    ASSERT_TRUE(received);
    EXPECT_EQ(command, ProcessCommand::Ping);
    EXPECT_TRUE(server.SendResponse(ProcessResponse::Pong));

    sender.join();
    EXPECT_TRUE(sendOk);
    EXPECT_EQ(response, ProcessResponse::Pong);

    client.Disconnect();
    server.Shutdown();
}
