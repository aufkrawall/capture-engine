#include <gtest/gtest.h>
#include "../common/elevation_protocol.h"
#include "../common/startup_policy.h"
#include "../common/elevation_windows.h"
#include <shellapi.h>
#include <thread>
#include <atomic>
#include "../elevationservice/service_internal.h"
#include "../common/elevation_lifetime.h"

using namespace ce::elevation;

TEST(ElevationProtocolTest, ValidatesEveryRequestShapeBeforeReadingPayload) {
    Header header;
    header.sequence = 1;
    header.size = sizeof(Hello);
    Hello hello{123, 1};
    EXPECT_TRUE(ValidateRequest(header, &hello, sizeof(hello)));
    EXPECT_FALSE(ValidateRequest(header, nullptr, sizeof(hello)));
    EXPECT_FALSE(ValidateRequest(header, &hello, sizeof(hello) - 1));
    hello.controllerPid = 0;
    EXPECT_FALSE(ValidateRequest(header, &hello, sizeof(hello)));
    for (auto operation : {Operation::SampleSensors, Operation::AcquireTrace, Operation::ReleaseTrace}) {
        header.operation = operation;
        header.size = 0;
        EXPECT_TRUE(ValidateRequest(header, nullptr, 0));
        header.size = 1;
        EXPECT_FALSE(ValidateRequest(header, &hello, 1));
    }
}

TEST(ElevationProtocolTest, RefusesVersionsOpcodesSizesAndReplyErrorsInRequests) {
    Header valid;
    valid.operation = Operation::AcquireTrace;
    valid.sequence = 9;
    EXPECT_TRUE(ValidateHeader(valid, false));
    for (int field = 0; field < 6; ++field) {
        Header bad = valid;
        switch (field) {
            case 0:
                bad.magic = 0;
                break;
            case 1:
                ++bad.version;
                break;
            case 2:
                bad.operation = static_cast<Operation>(99);
                break;
            case 3:
                bad.size = kMaximumPayload + 1;
                break;
            case 4:
                bad.error = 5;
                break;
            case 5:
                bad.sequence = 0;
                break;
        }
        EXPECT_FALSE(ValidateHeader(bad, false));
    }
}

TEST(ElevationProtocolTest, SensorSelectorsAreBoundedAndCannotCarryPathsOrCommands) {
    Header header;
    header.sequence = 1;
    header.operation = Operation::SubscribeSensors;
    header.size = sizeof(SensorRequest);
    SensorRequest request;
    for (auto& selector : request.selectors)
        std::memcpy(selector.data(), "auto", 5);
    EXPECT_TRUE(ValidateRequest(header, &request, sizeof(request)));
    request.pollIntervalMs = 249;
    EXPECT_FALSE(ValidateRequest(header, &request, sizeof(request)));
    request.pollIntervalMs = 10001;
    EXPECT_FALSE(ValidateRequest(header, &request, sizeof(request)));
    request.pollIntervalMs = 250;
    for (const char* selector : {"", "C:\\evil.dll", "/gpu/0;launch", "/cpu/0\n"}) {
        request.selectors[0].fill(0);
        std::memcpy(request.selectors[0].data(), selector, std::strlen(selector));
        EXPECT_FALSE(ValidateRequest(header, &request, sizeof(request)));
    }
    request.selectors[0].fill('x');
    EXPECT_FALSE(ValidateRequest(header, &request, sizeof(request)));
    request.selectors[0].fill(0);
    std::memcpy(request.selectors[0].data(), "/amdcpu/0/temperature/0", 23);
    EXPECT_TRUE(ValidateRequest(header, &request, sizeof(request)));
}

TEST(StartupPolicyTest, AllEightCheckboxCombinationsKeepServiceIndependent) {
    for (int flags = 0; flags < 8; ++flags) {
        ce::startup::Preferences preferences{(flags & 1) != 0, (flags & 2) != 0, (flags & 4) != 0};
        using ce::startup::Registration;
        EXPECT_EQ(ce::startup::SelectRegistration(preferences, true), !preferences.autostart ? Registration::None
                                                                      : preferences.elevated
                                                                          ? Registration::ElevatedTask
                                                                          : Registration::UserRun);
        EXPECT_EQ(ce::startup::SelectRegistration(preferences, false),
                  preferences.autostart ? Registration::UserRun : Registration::None);
        EXPECT_EQ(ce::startup::ShouldRequestElevation(true, false, preferences), preferences.elevated);
        EXPECT_FALSE(ce::startup::ShouldRequestElevation(false, false, preferences));
        EXPECT_FALSE(ce::startup::ShouldRequestElevation(true, true, preferences));
    }
}

TEST(StartupPolicyTest, WindowsArgumentsRoundTripWithoutChangingContents) {
    const std::wstring values[] = {L"",
                                   L"plain",
                                   L"two words",
                                   LR"(C:\folder name\)",
                                   LR"(quote"inside)",
                                   LR"(\\server\share\file)",
                                   LR"(--launch=game arg "x")"};
    std::wstring command = L"CaptureEngine.exe";
    for (const auto& value : values)
        command += L" " + QuoteArgument(value);
    int count = 0;
    wchar_t** decoded = CommandLineToArgvW(command.c_str(), &count);
    ASSERT_NE(decoded, nullptr);
    ASSERT_EQ(count, 1 + std::size(values));
    for (int index = 1; index < count; ++index)
        EXPECT_EQ(decoded[index], values[index - 1]);
    LocalFree(decoded);
}

TEST(StartupPolicyTest, CanceledOrFailedSetupCannotCommitPreference) {
    for (unsigned long error : {1223ul, 5ul, 1060ul}) {
        bool committed = false;
        bool rolledBack = false;
        unsigned long rollbackError = 99;
        EXPECT_EQ(ce::startup::ApplyTransaction([&] { return error; },
                                                [&] {
                                                    committed = true;
                                                    return true;
                                                },
                                                [&] {
                                                    rolledBack = true;
                                                    return 0ul;
                                                },
                                                29, rollbackError),
                  error);
        EXPECT_FALSE(committed);
        EXPECT_FALSE(rolledBack);
        EXPECT_EQ(rollbackError, 0);
    }
}

TEST(StartupPolicyTest, PersistenceFailureRestoresPrecedingRegistration) {
    int registration = 1;
    unsigned long rollbackError = 99;
    EXPECT_EQ(ce::startup::ApplyTransaction(
                  [&] {
                      registration = 2;
                      return 0ul;
                  },
                  [] { return false; },
                  [&] {
                      registration = 1;
                      return 0ul;
                  },
                  29, rollbackError),
              29);
    EXPECT_EQ(registration, 1);
    EXPECT_EQ(rollbackError, 0);
    EXPECT_EQ(
        ce::startup::ApplyTransaction([] { return 0ul; }, [] { return false; }, [] { return 5ul; }, 29, rollbackError),
        29);
    EXPECT_EQ(rollbackError, 5);
}

TEST(ElevationPipeTest, CancellationDrainsPendingReadBeforeReturning) {
    const std::wstring name = LR"(\\.\pipe\CE_ElevationRegression_)" + std::to_wstring(GetCurrentProcessId());
    Handle server(CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
                                   PIPE_TYPE_BYTE | PIPE_REJECT_REMOTE_CLIENTS, 1, 4096, 4096, 0, nullptr));
    ASSERT_TRUE(server);
    Handle client(CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                              FILE_FLAG_OVERLAPPED, nullptr));
    ASSERT_TRUE(client);
    Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    OVERLAPPED connection{};
    connection.hEvent = event.Get();
    EXPECT_TRUE(ConnectNamedPipe(server.Get(), &connection) || GetLastError() == ERROR_PIPE_CONNECTED);
    Handle stop(CreateEventW(nullptr, TRUE, TRUE, nullptr));
    uint32_t value = 0;
    EXPECT_FALSE(Transfer(server.Get(), &value, sizeof(value), false, stop.Get(), nullptr, INFINITE));
    EXPECT_EQ(GetLastError(), ERROR_OPERATION_ABORTED);
    value = 123;
    EXPECT_TRUE(Transfer(client.Get(), &value, sizeof(value), true));
    uint32_t received = 0;
    EXPECT_TRUE(Transfer(server.Get(), &received, sizeof(received), false));
    EXPECT_EQ(received, value);
}

TEST(ElevationLifetimeTest, LastReleaseStopsAndRefusesReconnectionToOldGeneration) {
    ClientLifetime lifetime;
    ASSERT_TRUE(lifetime.Admit());
    ASSERT_TRUE(lifetime.Admit());
    EXPECT_FALSE(lifetime.Release());
    EXPECT_EQ(lifetime.Count(), 1);
    EXPECT_TRUE(lifetime.Admit());
    EXPECT_FALSE(lifetime.Release());
    EXPECT_TRUE(lifetime.Release());
    EXPECT_FALSE(lifetime.Admit());
    EXPECT_FALSE(lifetime.Release());
    EXPECT_EQ(lifetime.Count(), 0);
}

TEST(ElevationLifetimeTest, ConcurrentControllerLossStopsExactlyOnce) {
    ClientLifetime lifetime;
    ASSERT_TRUE(lifetime.Admit());
    ASSERT_TRUE(lifetime.Admit());
    Handle ready1(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    Handle ready2(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    Handle release(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    std::atomic<int> stopped{0};
    const auto lose = [&](HANDLE ready) {
        SetEvent(ready);
        WaitForSingleObject(release.Get(), INFINITE);
        if (lifetime.Release())
            ++stopped;
    };
    std::thread first(lose, ready1.Get());
    std::thread second(lose, ready2.Get());
    HANDLE ready[] = {ready1.Get(), ready2.Get()};
    const DWORD arrived = WaitForMultipleObjects(2, ready, TRUE, 5000);
    SetEvent(release.Get());
    first.join();
    second.join();
    EXPECT_EQ(arrived, WAIT_OBJECT_0);
    EXPECT_EQ(stopped.load(), 1);
    EXPECT_FALSE(lifetime.Admit());
}

TEST(ElevationAuthenticationTest, VerifiesRealPipePeerControllerOwnerAndSession) {
    const std::wstring name = LR"(\\.\pipe\CE_ElevationAuthentication_)" + std::to_wstring(GetCurrentProcessId());
    Handle server(CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
                                   PIPE_TYPE_BYTE | PIPE_REJECT_REMOTE_CLIENTS, 1, 4096, 4096, 0, nullptr));
    ASSERT_TRUE(server);
    Handle client(CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                              FILE_FLAG_OVERLAPPED, nullptr));
    ASSERT_TRUE(client);
    Handle controller;
    DWORD session = 0;
    ASSERT_TRUE(ProcessIdToSessionId(GetCurrentProcessId(), &session));
    const std::wstring owner = ProcessUserSid(GetCurrentProcess());
    ASSERT_FALSE(owner.empty());
    Hello hello{GetCurrentProcessId(), session};
    EXPECT_TRUE(AuthenticateClient(server.Get(), hello, owner, controller));
    EXPECT_EQ(GetProcessId(controller.Get()), GetCurrentProcessId());
    ++hello.sessionId;
    EXPECT_FALSE(AuthenticateClient(server.Get(), hello, owner, controller));
    hello.sessionId = session;
    hello.controllerPid = 0;
    EXPECT_FALSE(AuthenticateClient(server.Get(), hello, owner, controller));
    hello.controllerPid = GetCurrentProcessId();
    if (!IsElevated())
        EXPECT_FALSE(AuthenticateClient(server.Get(), hello, L"S-1-5-18", controller));
}

TEST(ElevationProtocolTest, ServiceSamplesPreserveSequenceSelectorsAndAcquisitionTime) {
    ce::hardware_sensors::HardwareSensorSnapshot snapshot;
    snapshot.sequence = 7;
    snapshot.receivedTickMs = 1234;
    snapshot.cpuTemperature = {42.0f, true, "/amdcpu/0/temperature/0"};
    const SensorSample sample = MakeSensorSample(snapshot);
    EXPECT_EQ(sample.sampledTickMs, 1234);
    ce::hardware_sensors::BridgeMessage message;
    ASSERT_TRUE(ce::hardware_sensors::ParseBridgeMessage(sample.line.data(), message));
    EXPECT_EQ(message.snapshot.sequence, 7);
    EXPECT_FLOAT_EQ(message.snapshot.cpuTemperature.value, 42.0f);
    EXPECT_EQ(message.snapshot.cpuTemperature.identifier, snapshot.cpuTemperature.identifier);
    EXPECT_FALSE(message.snapshot.gpuTemperature.valid);
}

TEST(ElevationProtocolTest, ConsumerTraceRightsAreReadOnlyAndIncludeRealtimeAccess) {
    EXPECT_TRUE(kConsumerTraceRights & TRACELOG_ACCESS_REALTIME);
    EXPECT_TRUE(kConsumerTraceRights & WMIGUID_QUERY);
    EXPECT_EQ(kConsumerTraceRights & (WMIGUID_SET | WMIGUID_EXECUTE | TRACELOG_CREATE_REALTIME |
                                      TRACELOG_CREATE_ONDISK | TRACELOG_GUID_ENABLE), 0ul);
}
