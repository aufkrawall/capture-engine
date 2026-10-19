#include <gtest/gtest.h>

#include <new>
#include <string>
#include <vector>

#include "common/ipc/shared_defs.h"
#include "runtime/inject/inject_control.h"

// InjectControl is the controller's whole conversation with the inject helper. These drive it through
// InjectLink fakes (no processes) and, for the shared-memory transactions, through a real isolated
// discovery/shared-memory mapping so the passthrough is proven end to end.

namespace {

using namespace ce::runtime;
using ce::ipc::ControlStatus;

struct TestMapping {
    HANDLE handle = nullptr;
    void* view = nullptr;
    ~TestMapping() {
        if (view)
            UnmapViewOfFile(view);
        if (handle)
            CloseHandle(handle);
    }
    bool Create(const wchar_t* name, size_t size) {
        handle = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, static_cast<DWORD>(size), name);
        if (!handle || GetLastError() == ERROR_ALREADY_EXISTS)
            return false;
        view = MapViewOfFile(handle, FILE_MAP_ALL_ACCESS, 0, 0, size);
        return view != nullptr;
    }
};

struct SentCommand {
    ProcessCommand command;
    uint32_t timeoutMs;
};

// The link the runtime core builds over its child supervisor, as a fake.
class FakeLink final : public InjectLink {
public:
    bool present = true;
    bool ready = true;
    bool sendReturns = true;
    ProcessResponse reply = ProcessResponse::Ack;
    std::vector<SentCommand> sent;
    mutable int channelRequests = 0;
    const wchar_t* discoveryName = L"Local\\CE_Control_RuntimeInject_NoSuchMapping";

    bool Present() const override { return present; }
    bool Ready() const override { return ready; }
    ce::ipc::InjectControlChannel Channel() const override {
        ++channelRequests;
        return ce::ipc::InjectControlChannel(GetCurrentProcess(), discoveryName);
    }
    bool Send(ProcessCommand command, ProcessResponse& response, uint32_t timeoutMs) override {
        sent.push_back({command, timeoutMs});
        response = reply;
        return sendReturns;
    }
};

// A real inject-side mapping pair for the shared-memory transactions.
class InjectControlMappingTest : public testing::Test {
protected:
    std::wstring discoveryName = L"Local\\CE_Control_RuntimeInject_" + std::to_wstring(GetCurrentProcessId());
    TestMapping discovery;
    TestMapping memory;
    DiscoveryInfo* info = nullptr;
    SharedMemoryLayout* shared = nullptr;
    FakeLink link;

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
        link.discoveryName = discoveryName.c_str();
    }
    void TearDown() override {
        if (shared)
            shared->~SharedMemoryLayout();
        if (info)
            info->~DiscoveryInfo();
    }
};

}  // namespace

TEST(InjectControlToggleTest, WithoutAConnectedInjectNothingIsSent) {
    FakeLink link;
    link.ready = false;
    InjectControl control(link);
    EXPECT_EQ(control.ToggleOverlay(), ToggleOutcome::NotConnected);
    EXPECT_EQ(control.ToggleBenchmark(), ToggleOutcome::NotConnected);
    EXPECT_TRUE(link.sent.empty()) << "a toggle for a missing helper must not touch the command channel";
}

TEST(InjectControlToggleTest, PresentButNotReadyIsStillNotConnected) {
    // A spawned-but-unauthenticated or dead child: shared memory may exist, the command channel does not.
    FakeLink link;
    link.present = true;
    link.ready = false;
    InjectControl control(link);
    EXPECT_EQ(control.ToggleOverlay(), ToggleOutcome::NotConnected);
    EXPECT_TRUE(link.sent.empty());
}

TEST(InjectControlToggleTest, AnAcknowledgedToggleIsAcceptedAndSendsTheRightCommand) {
    FakeLink link;
    InjectControl control(link);
    EXPECT_EQ(control.ToggleOverlay(), ToggleOutcome::Accepted);
    EXPECT_EQ(control.ToggleBenchmark(), ToggleOutcome::Accepted);
    ASSERT_EQ(link.sent.size(), 2u);
    EXPECT_EQ(link.sent[0].command, ProcessCommand::ToggleOverlay);
    EXPECT_EQ(link.sent[1].command, ProcessCommand::ToggleBenchmark);
    EXPECT_EQ(link.sent[0].timeoutMs, InjectControl::kToggleTimeoutMs);
    EXPECT_EQ(link.sent[1].timeoutMs, 1000u) << "the bound the hotkey path always used";
}

TEST(InjectControlToggleTest, AnyNonErrorAnswerCountsAsAcceptance) {
    // The inject child answers Ack today, but the policy has always been "anything but Error".
    FakeLink link;
    InjectControl control(link);
    for (const ProcessResponse reply : {ProcessResponse::Ack, ProcessResponse::RecordingStopped, ProcessResponse::None}) {
        link.reply = reply;
        EXPECT_EQ(control.ToggleOverlay(), ToggleOutcome::Accepted) << static_cast<int>(reply);
    }
}

TEST(InjectControlToggleTest, AnErrorAnswerOrAnUndeliveredCommandIsRejected) {
    FakeLink link;
    InjectControl control(link);

    link.reply = ProcessResponse::Error;
    EXPECT_EQ(control.ToggleOverlay(), ToggleOutcome::Rejected);
    EXPECT_EQ(control.ToggleBenchmark(), ToggleOutcome::Rejected);

    link.reply = ProcessResponse::Ack;
    link.sendReturns = false;  // timed out or channel broken
    EXPECT_EQ(control.ToggleOverlay(), ToggleOutcome::Rejected);
    EXPECT_EQ(control.ToggleBenchmark(), ToggleOutcome::Rejected);
    EXPECT_EQ(link.sent.size(), 4u) << "each rejected toggle did attempt delivery exactly once";
}

TEST(InjectControlToggleTest, ABlockedCallerIsReportedFromTheDocumentedThreshold) {
    EXPECT_FALSE(InjectControl::ShouldWarnBlocked(0));
    EXPECT_FALSE(InjectControl::ShouldWarnBlocked(249));
    EXPECT_TRUE(InjectControl::ShouldWarnBlocked(250));
    EXPECT_TRUE(InjectControl::ShouldWarnBlocked(60000));
}

TEST(InjectControlSharedMemoryTest, WithoutAnInjectIdentityNothingIsTouched) {
    FakeLink link;
    link.present = false;
    InjectControl control(link);

    EXPECT_EQ(control.Notify(OverlayNotificationType::ScreenshotSaved, 99).status, ControlStatus::Unavailable);
    EXPECT_EQ(control.PublishRecordingIntent(RecordingStartIntent::Video).status, ControlStatus::Unavailable);

    ce::ipc::RecordingHealthObservation observation;
    observation.live = true;
    observation.liveSince = 5;
    observation.failure = 7;
    EXPECT_EQ(control.ReadHealth(observation).status, ControlStatus::Unavailable);
    EXPECT_FALSE(observation.live) << "a stale observation must not survive a failed read";
    EXPECT_EQ(observation.liveSince, 0);
    EXPECT_EQ(observation.failure, 0u);

    control.ClearMediaFailure(3, true);
    EXPECT_EQ(link.channelRequests, 0) << "no identity: the shared-memory channel is never even asked for";
}

TEST_F(InjectControlMappingTest, NotificationsAndIntentReachTheSharedMemory) {
    InjectControl control(link);
    EXPECT_TRUE(control.Notify(OverlayNotificationType::ScreenshotSaved, 4321));
    EXPECT_EQ(shared->runtimeState.notificationType.load(std::memory_order_acquire),
              static_cast<uint32_t>(OverlayNotificationType::ScreenshotSaved));
    EXPECT_EQ(shared->runtimeState.notificationExpiry.load(std::memory_order_acquire), 4321u);

    EXPECT_TRUE(control.PublishRecordingIntent(RecordingStartIntent::AudioOnly));
    EXPECT_EQ(shared->runtimeState.GetRecordingStartIntent(), RecordingStartIntent::AudioOnly);
}

TEST_F(InjectControlMappingTest, HealthIsReadFromTheSharedMemory) {
    auto& state = shared->runtimeState;
    state.isRecording.store(true, std::memory_order_release);
    state.recordingStartTime.store(678, std::memory_order_release);
    state.recordingFailureCode.store(2, std::memory_order_release);

    InjectControl control(link);
    ce::ipc::RecordingHealthObservation observation;
    EXPECT_TRUE(control.ReadHealth(observation));
    EXPECT_TRUE(observation.live);
    EXPECT_EQ(observation.liveSince, 678);
    EXPECT_EQ(observation.failure, 2u);
}

TEST_F(InjectControlMappingTest, ClearMediaFailureConsumesOnlyWhatItIsToldTo) {
    auto& state = shared->runtimeState;
    state.isRecording.store(true, std::memory_order_release);
    state.recordingStartTime.store(900, std::memory_order_release);
    state.recordingFailureCode.store(2, std::memory_order_release);
    InjectControl control(link);

    // Neither a failure code nor a dead media process: nothing changes.
    control.ClearMediaFailure(0, false);
    EXPECT_EQ(state.recordingFailureCode.load(std::memory_order_acquire), 2u);
    EXPECT_TRUE(state.isRecording.load(std::memory_order_acquire));

    // A different failure code than the one in memory is not consumed (the channel compares).
    control.ClearMediaFailure(1, false);
    EXPECT_EQ(state.recordingFailureCode.load(std::memory_order_acquire), 2u);

    // The observed failure is consumed, the still-running media's recording state is left alone.
    control.ClearMediaFailure(2, false);
    EXPECT_EQ(state.recordingFailureCode.load(std::memory_order_acquire), 0u);
    EXPECT_TRUE(state.isRecording.load(std::memory_order_acquire));

    // A dead media process: the recording state it left behind is cleared.
    control.ClearMediaFailure(0, true);
    EXPECT_FALSE(state.isRecording.load(std::memory_order_acquire));
    EXPECT_EQ(state.recordingStartTime.load(std::memory_order_acquire), 0);
}

TEST_F(InjectControlMappingTest, AReplacedOrWithdrawnInjectIsRejectedByTheChannelNotCached) {
    InjectControl control(link);
    EXPECT_TRUE(control.PublishRecordingIntent(RecordingStartIntent::Video));
    info->SetInjectPid(GetCurrentProcessId() + 1);
    EXPECT_EQ(control.PublishRecordingIntent(RecordingStartIntent::Idle).status, ControlStatus::StaleTarget);
    EXPECT_EQ(shared->runtimeState.GetRecordingStartIntent(), RecordingStartIntent::Video);
    info->SetInjectPid(GetCurrentProcessId());
    EXPECT_TRUE(control.PublishRecordingIntent(RecordingStartIntent::Idle));
    EXPECT_GE(link.channelRequests, 3) << "the link is asked for the current identity on every transaction";
}
