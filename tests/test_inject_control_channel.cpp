#include <gtest/gtest.h>

#include <new>
#include <string>

#include "common/ipc/inject_control_channel.h"
#include "common/ipc/shared_defs.h"

namespace {
using namespace ce::ipc;

struct TestMapping {
    HANDLE handle = nullptr;
    void* view = nullptr;
    ~TestMapping() {
        if (view) UnmapViewOfFile(view);
        if (handle) CloseHandle(handle);
    }
    bool Create(const wchar_t* name, size_t size) {
        handle = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, static_cast<DWORD>(size), name);
        if (!handle || GetLastError() == ERROR_ALREADY_EXISTS) return false;
        view = MapViewOfFile(handle, FILE_MAP_ALL_ACCESS, 0, 0, size);
        return view != nullptr;
    }
};

class InjectControlChannelTest : public testing::Test {
protected:
    std::wstring discoveryName = L"Local\\CE_Control_Unit_" + std::to_wstring(GetCurrentProcessId());
    TestMapping discovery;
    TestMapping memory;
    DiscoveryInfo* info = nullptr;
    SharedMemoryLayout* shared = nullptr;
    InjectControlChannel Channel() { return InjectControlChannel(GetCurrentProcess(), discoveryName.c_str()); }
    void SetUp() override {
        ASSERT_TRUE(discovery.Create(discoveryName.c_str(), sizeof(DiscoveryInfo)));
        info = new (discovery.view) DiscoveryInfo;
        info->abiSignature.store(SHARED_MEMORY_ABI_SIGNATURE, std::memory_order_relaxed);
        info->SetInjectPid(GetCurrentProcessId());
        info->SetMagic(DISCOVERY_MAGIC);
        wchar_t name[64]{};
        GenerateSharedMemName(name, 64, GetCurrentProcessId());
        ASSERT_TRUE(memory.Create(name, sizeof(SharedMemoryLayout)));
        shared = new (memory.view) SharedMemoryLayout;
        shared->structSize.store(sizeof(SharedMemoryLayout), std::memory_order_relaxed);
        shared->abiSignature.store(SHARED_MEMORY_ABI_SIGNATURE, std::memory_order_relaxed);
        shared->SetMagic(SHARED_MEMORY_MAGIC);
    }
    void TearDown() override {
        if (shared) shared->~SharedMemoryLayout();
        if (info) info->~DiscoveryInfo();
    }
};
}  // namespace

TEST_F(InjectControlChannelTest, PublishesIntentAndNotificationsThroughRealMappings) {
    EXPECT_TRUE(Channel().PublishRecordingIntent(RecordingStartIntent::AudioOnly));
    EXPECT_EQ(shared->runtimeState.GetRecordingStartIntent(), RecordingStartIntent::AudioOnly);
    EXPECT_TRUE(shared->runtimeState.audioOnly.load(std::memory_order_acquire));
    EXPECT_TRUE(Channel().PublishNotification(OverlayNotificationType::RecordingFinalizing, 1234));
    EXPECT_EQ(shared->runtimeState.notificationExpiry.load(std::memory_order_acquire), 1234u);
    EXPECT_TRUE(Channel().PublishNotification(OverlayNotificationType::None, 9999));
    EXPECT_EQ(shared->runtimeState.notificationType.load(std::memory_order_acquire), 0u);
    EXPECT_EQ(shared->runtimeState.notificationExpiry.load(std::memory_order_acquire), 0u);
}

TEST_F(InjectControlChannelTest, RejectsInvalidDiscoveryAbiAndPayloadWithoutMutatingState) {
    info->SetMagic(0);
    EXPECT_EQ(Channel().PublishRecordingIntent(RecordingStartIntent::Video).status, ControlStatus::InvalidDiscovery);
    info->SetMagic(DISCOVERY_MAGIC);
    info->abiSignature.store(0, std::memory_order_release);
    EXPECT_EQ(Channel().PublishRecordingIntent(RecordingStartIntent::Video).status, ControlStatus::InvalidDiscovery);
    info->abiSignature.store(SHARED_MEMORY_ABI_SIGNATURE, std::memory_order_release);
    shared->SetVersion(SHARED_MEMORY_VERSION + 1);
    EXPECT_EQ(Channel().PublishRecordingIntent(RecordingStartIntent::Video).status, ControlStatus::InvalidSharedMemory);
    EXPECT_EQ(shared->runtimeState.GetRecordingStartIntent(), RecordingStartIntent::Idle);
    shared->SetVersion(SHARED_MEMORY_VERSION);
    EXPECT_TRUE(Channel().PublishRecordingIntent(RecordingStartIntent::Video));
}

TEST_F(InjectControlChannelTest, RejectsReplacementAndWithdrawnTargetWithoutCachingTheOldView) {
    auto channel = Channel();
    EXPECT_TRUE(channel.PublishRecordingIntent(RecordingStartIntent::Video));
    info->SetInjectPid(GetCurrentProcessId() + 1);
    EXPECT_EQ(channel.PublishRecordingIntent(RecordingStartIntent::Idle).status, ControlStatus::StaleTarget);
    EXPECT_EQ(shared->runtimeState.GetRecordingStartIntent(), RecordingStartIntent::Video);
    info->SetInjectPid(0);
    EXPECT_EQ(channel.PublishRecordingIntent(RecordingStartIntent::Idle).status, ControlStatus::Unavailable);
    info->SetInjectPid(GetCurrentProcessId());
    EXPECT_TRUE(channel.PublishRecordingIntent(RecordingStartIntent::Idle));
}

TEST_F(InjectControlChannelTest, ReadsHealthAndConsumesOnlyTheObservedFailure) {
    auto& state = shared->runtimeState;
    state.isRecording.store(true, std::memory_order_release);
    state.recordingStartTime.store(678, std::memory_order_release);
    state.recordingFailureCode.store(2, std::memory_order_release);
    RecordingHealthObservation observation;
    EXPECT_TRUE(Channel().ReadRecordingHealth(observation));
    EXPECT_TRUE(observation.live);
    EXPECT_EQ(observation.liveSince, 678);
    EXPECT_EQ(observation.failure, 2u);
    EXPECT_TRUE(Channel().ConsumeRecordingFailure(1));
    EXPECT_EQ(state.recordingFailureCode.load(std::memory_order_acquire), 2u);
    EXPECT_TRUE(Channel().ConsumeRecordingFailure(2));
    EXPECT_EQ(state.recordingFailureCode.load(std::memory_order_acquire), 0u);
    EXPECT_TRUE(Channel().ClearDeadMediaState());
    EXPECT_FALSE(state.isRecording.load(std::memory_order_acquire));
    EXPECT_EQ(state.recordingStartTime.load(std::memory_order_acquire), 0);
    info->SetMagic(0);
    EXPECT_FALSE(Channel().ReadRecordingHealth(observation));
    EXPECT_FALSE(observation.live);
    EXPECT_EQ(observation.liveSince, 0);
}

TEST(InjectControlChannelProbe, ReadsAndPublishesInAnotherProcess) {
    wchar_t name[128]{};
    if (!GetEnvironmentVariableW(L"CE_CONTROL_UNIT_DISCOVERY", name, 128)) GTEST_SKIP();
    InjectControlChannel channel(nullptr, name);
    RecordingHealthObservation observation;
    ASSERT_TRUE(channel.ReadRecordingHealth(observation));
    EXPECT_EQ(observation.liveSince, 4321);
    ASSERT_TRUE(channel.PublishNotification(OverlayNotificationType::ScreenshotSaved, 9876));
}

TEST_F(InjectControlChannelTest, ReleaseAcquirePublicationIsVisibleAcrossProcesses) {
    shared->runtimeState.recordingStartTime.store(4321, std::memory_order_release);
    wchar_t executable[32768]{};
    ASSERT_GT(GetModuleFileNameW(nullptr, executable, 32768), 0u);
    std::wstring command = L"\"" + std::wstring(executable) + L"\" --gtest_filter=InjectControlChannelProbe.*";
    ASSERT_TRUE(SetEnvironmentVariableW(L"CE_CONTROL_UNIT_DISCOVERY", discoveryName.c_str()));
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION process{};
    const bool created = CreateProcessW(executable, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                                        nullptr, nullptr, &startup, &process) != FALSE;
    SetEnvironmentVariableW(L"CE_CONTROL_UNIT_DISCOVERY", nullptr);
    ASSERT_TRUE(created);
    CloseHandle(process.hThread);
    const DWORD wait = WaitForSingleObject(process.hProcess, 10000);
    if (wait != WAIT_OBJECT_0) {
        TerminateProcess(process.hProcess, 1);
        WaitForSingleObject(process.hProcess, 10000);
    }
    DWORD exitCode = 1;
    GetExitCodeProcess(process.hProcess, &exitCode);
    CloseHandle(process.hProcess);
    ASSERT_EQ(wait, WAIT_OBJECT_0);
    EXPECT_EQ(exitCode, 0u);
    EXPECT_EQ(shared->runtimeState.notificationExpiry.load(std::memory_order_acquire), 9876u);
    EXPECT_EQ(shared->runtimeState.notificationType.load(std::memory_order_acquire),
              static_cast<uint32_t>(OverlayNotificationType::ScreenshotSaved));
}

TEST(InjectControlChannelTestStandalone, MissingMappingReturnsUnavailableAndClearsObservation) {
    InjectControlChannel channel(nullptr, L"Local\\CE_Control_Unit_Missing");
    RecordingHealthObservation observation;
    observation.live = true;
    EXPECT_EQ(channel.ReadRecordingHealth(observation).status, ControlStatus::Unavailable);
    EXPECT_FALSE(observation.live);
}

TEST_F(InjectControlChannelTest, FailedTransactionsDoNotLeakHandles) {
    // Warm the diagnostic gate before measuring; every operation opens its own views.
    shared->SetMagic(0);
    EXPECT_EQ(Channel().PublishRecordingIntent(RecordingStartIntent::Video).status,
              ControlStatus::InvalidSharedMemory);
    DWORD before = 0, after = 0;
    ASSERT_TRUE(GetProcessHandleCount(GetCurrentProcess(), &before));
    for (int i = 0; i < 100; ++i)
        EXPECT_EQ(Channel().PublishRecordingIntent(RecordingStartIntent::Video).status,
                  ControlStatus::InvalidSharedMemory);
    ASSERT_TRUE(GetProcessHandleCount(GetCurrentProcess(), &after));
    EXPECT_EQ(before, after);
}

TEST(InjectControlChannelTestStandalone, MissingPayloadAndFailedMapRemainRetryable) {
    const std::wstring discoveryName = L"Local\\CE_Control_Short_" + std::to_wstring(GetCurrentProcessId());
    TestMapping discovery, payload;
    ASSERT_TRUE(discovery.Create(discoveryName.c_str(), sizeof(DiscoveryInfo)));
    auto* info = new (discovery.view) DiscoveryInfo;
    info->abiSignature.store(SHARED_MEMORY_ABI_SIGNATURE, std::memory_order_relaxed);
    info->SetInjectPid(GetCurrentProcessId());
    info->SetMagic(DISCOVERY_MAGIC);
    const InjectControlChannel channel(GetCurrentProcess(), discoveryName.c_str());
    EXPECT_EQ(channel.PublishRecordingIntent(RecordingStartIntent::Video).status, ControlStatus::Unavailable);
    wchar_t name[64]{};
    GenerateSharedMemName(name, 64, GetCurrentProcessId());
    ASSERT_TRUE(payload.Create(name, 4096));
    EXPECT_EQ(channel.PublishRecordingIntent(RecordingStartIntent::Video).status, ControlStatus::MappingFailed);
    EXPECT_EQ(channel.PublishNotification(OverlayNotificationType::ScreenshotSaved, 1).status,
              ControlStatus::MappingFailed);
    info->~DiscoveryInfo();
}
