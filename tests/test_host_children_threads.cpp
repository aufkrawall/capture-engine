#include <gtest/gtest.h>

#include "common/ipc/media_finalization.h"
#include "common/ipc/process_ipc.h"
#include "runtime/children/host_children.h"

#include <atomic>
#include <string>
#include <thread>
#include <vector>

// The thread transport of the supervisor (DR-37, DR-38) with fake role entries. No real role runs and no process is
// spawned: every role here is a thread-hosted fake, and no test asks the supervisor for a process role.

namespace {
using namespace ce::runtime;
using ce::role::RoleContext;

constexpr DWORD kWaitMs = 10000;  // a bound for a broken test only; a healthy fake never reaches it

// Test-owned state the fake entries read. Each test sets it before it spawns anything.
std::atomic<bool> g_loggerSawStop{false};
HANDLE g_silentRelease = nullptr;  // the fake inject role that never polls waits on this
HANDLE g_gateFirst = nullptr;      // the media fake for recording r0001 publishes once this is set
HANDLE g_gateSecond = nullptr;     // the media fake for recording r0002

// Logger fake: runs until its stop event is set, as a logger does on shutdown.
int LoggerEntry(RoleContext& context, const AppConfig&) {
    g_loggerSawStop = WaitForSingleObject(context.stopEvent, kWaitMs) == WAIT_OBJECT_0;
    return 0;
}

// Channel fake: answers Ping with Pong, Shutdown with Ack and ends. It stops on its stop event too.
int ServingEntry(RoleContext& context, const AppConfig&) {
    while (!context.channel->HasFatalDisconnect()) {
        if (!context.channel->WaitForCommand(50)) {
            if (WaitForSingleObject(context.stopEvent, 0) == WAIT_OBJECT_0)
                return 0;
            continue;
        }
        ProcessCommand command = ProcessCommand::None;
        while (context.channel->PollCommand(command)) {
            if (command == ProcessCommand::Shutdown) {
                context.channel->SendResponse(ProcessResponse::Ack);
                return 0;
            }
            context.channel->SendResponse(command == ProcessCommand::Ping ? ProcessResponse::Pong
                                                                          : ProcessResponse::Error);
        }
    }
    return 0;
}

// Channel fake that never polls its channel: every command it is sent goes unanswered until it is released.
int SilentEntry(RoleContext&, const AppConfig&) {
    WaitForSingleObject(g_silentRelease, kWaitMs);
    return 0;
}

// Media fake: maps its inherited result slot, publishes a saved result whose path names its recording, and returns the
// coded exit. The recording is the one the supervisor copied into the context at spawn.
int MediaEntry(RoleContext& context, const AppConfig&) {
    ce::media_result::Slot* slot = ce::media_result::MapInheritedSlot(context.resultSlotHandle);
    WaitForSingleObject(context.recordingId == "r0001" ? g_gateFirst : g_gateSecond, kWaitMs);
    ce::media_result::FinalizationInputs inputs;
    inputs.outputSaved = true;
    inputs.outputPath = "C:\\out\\" + context.recordingId + ".mp4";
    const ce::media_result::Result result = ce::media_result::BuildResult(inputs, 0);
    if (slot)
        ce::media_result::PublishResult(*slot, result);
    ce::media_result::UnmapInheritedSlot(slot);
    return static_cast<int>(ce::media_result::EncodeExitCode(result.status, result.flags));
}

ThreadRoleSetup ThreadSetup(RoleEntries entries) {
    ThreadRoleSetup setup;
    setup.entries = entries;
    setup.transports.inject = HelperTransport::Thread;
    setup.transports.media = HelperTransport::Thread;
    setup.transports.logger = HelperTransport::Thread;
    setup.currentConfig = [] { return AppConfig(); };
    return setup;
}

HANDLE NewEvent() {
    return CreateEventW(nullptr, TRUE, FALSE, nullptr);
}

// Sets g_RecordingId for one spawn and restores the previous value.
class ScopedRecordingId {
public:
    explicit ScopedRecordingId(const std::string& id) : previous_(g_RecordingId) { g_RecordingId = id; }
    ~ScopedRecordingId() { g_RecordingId = previous_; }
    ScopedRecordingId(const ScopedRecordingId&) = delete;
    ScopedRecordingId& operator=(const ScopedRecordingId&) = delete;

private:
    std::string previous_;
};

class HostChildrenThreadsTest : public testing::Test {
protected:
    void SetUp() override {
        g_loggerSawStop = false;
        g_silentRelease = NewEvent();
        g_gateFirst = NewEvent();
        g_gateSecond = NewEvent();
        ASSERT_NE(g_silentRelease, nullptr);
        ASSERT_NE(g_gateFirst, nullptr);
        ASSERT_NE(g_gateSecond, nullptr);
    }
    void TearDown() override {
        for (HANDLE* event : {&g_silentRelease, &g_gateFirst, &g_gateSecond}) {
            if (*event)
                CloseHandle(*event);
            *event = nullptr;
        }
    }
};
}  // namespace

TEST_F(HostChildrenThreadsTest, ALoggerThreadRunsUntilItsStopEventAndShutdownWaitsForIt) {
    RoleEntries entries;
    entries.logger = LoggerEntry;
    ChildSupervisor children("fixture.ini", nullptr, nullptr, nullptr, ThreadSetup(entries));
    ASSERT_TRUE(children.Ensure(HostChild::Logger));
    EXPECT_TRUE(children.Running(HostChild::Logger));
    EXPECT_TRUE(children.Ready(HostChild::Logger)) << "a logger has no channel: running is ready";
    EXPECT_TRUE(children.Shutdown());
    EXPECT_TRUE(g_loggerSawStop.load()) << "the stop event, not a process exit, ended the logger";
}

TEST_F(HostChildrenThreadsTest, AThreadInjectRoleAnswersItsChannelAndEndsOnShutdown) {
    RoleEntries entries;
    entries.inject = ServingEntry;
    ChildSupervisor children("fixture.ini", nullptr, nullptr, nullptr, ThreadSetup(entries));
    ASSERT_TRUE(children.Ensure(HostChild::Inject));
    ProcessResponse response = ProcessResponse::None;
    EXPECT_TRUE(children.Send(HostChild::Inject, ProcessCommand::Ping, nullptr, &response, 5000));
    EXPECT_EQ(response, ProcessResponse::Pong);
    EXPECT_TRUE(children.Send(HostChild::Inject, ProcessCommand::Shutdown, nullptr, &response, 5000));
    EXPECT_EQ(response, ProcessResponse::Ack);
    // The role ended on its Ack; its channel end went with it, so the supervisor's shutdown does not wait for a reply.
    EXPECT_TRUE(children.Shutdown());
    EXPECT_FALSE(children.Running(HostChild::Inject));
}

TEST_F(HostChildrenThreadsTest, AMissedReplyBreaksTheChannelButKeepsTheRoleIdentityUntilItEnds) {
    RoleEntries entries;
    entries.inject = SilentEntry;
    ChildSupervisor children("fixture.ini", nullptr, nullptr, nullptr, ThreadSetup(entries));
    ASSERT_TRUE(children.Ensure(HostChild::Inject));

    // Ping is tolerated: the first miss keeps the channel, a second miss breaks it.
    EXPECT_FALSE(children.Send(HostChild::Inject, ProcessCommand::Ping, nullptr, nullptr, 0));
    EXPECT_TRUE(children.Ready(HostChild::Inject));
    EXPECT_FALSE(children.Send(HostChild::Inject, ProcessCommand::Ping, nullptr, nullptr, 0));
    EXPECT_FALSE(children.Ready(HostChild::Inject));
    EXPECT_TRUE(children.Present(HostChild::Inject)) << "the broken channel does not forget the thread";
    EXPECT_TRUE(children.Running(HostChild::Inject));

    // Shutdown cannot finish while the stalled role runs: it waits for the thread, which is released here.
    HANDLE done = NewEvent();
    ASSERT_NE(done, nullptr);
    bool shutdownResult = false;
    std::thread shutdown([&] {
        shutdownResult = children.Shutdown();
        SetEvent(done);
    });
    EXPECT_EQ(WaitForSingleObject(done, 0), WAIT_TIMEOUT) << "shutdown returned while the role was still running";
    SetEvent(g_silentRelease);
    shutdown.join();
    CloseHandle(done);
    EXPECT_TRUE(shutdownResult);
    EXPECT_FALSE(children.Running(HostChild::Inject));
}

TEST_F(HostChildrenThreadsTest, ARetiredMediaThreadAndASecondMediaThreadEachFinalizeOnceForTheirOwnRecording) {
    RoleEntries entries;
    entries.media = MediaEntry;
    ChildSupervisor children("fixture.ini", nullptr, nullptr, nullptr, ThreadSetup(entries));

    {
        ScopedRecordingId first("r0001");
        ASSERT_TRUE(children.Ensure(HostChild::Media));
    }
    // Retiring the first recording keeps its thread running (blocked on its gate) while a new media thread starts.
    children.RetireMedia(1);
    EXPECT_EQ(children.FinalizingCount(), 1u);
    {
        ScopedRecordingId second("r0002");
        ASSERT_TRUE(children.Ensure(HostChild::Media));
    }
    children.RetireMedia(2);
    EXPECT_EQ(children.FinalizingCount(), 2u);

    SetEvent(g_gateSecond);
    SetEvent(g_gateFirst);
    EXPECT_TRUE(children.Shutdown());

    const std::vector<FinalizedMedia> finalized = children.CollectFinalizedMedia();
    ASSERT_EQ(finalized.size(), 2u);
    for (const FinalizedMedia& item : finalized) {
        const bool first = item.recordingRequest == 1;
        ASSERT_TRUE(first || item.recordingRequest == 2);
        EXPECT_EQ(item.resolution.result.status, ce::media_result::Status::kSaved);
        EXPECT_TRUE(item.resolution.fromSlot);
        ASSERT_EQ(item.resolution.result.paths.size(), 1u);
        EXPECT_EQ(item.resolution.result.paths[0], first ? "C:\\out\\r0001.mp4" : "C:\\out\\r0002.mp4");
    }
    EXPECT_NE(finalized[0].recordingRequest, finalized[1].recordingRequest);
    EXPECT_TRUE(children.CollectFinalizedMedia().empty()) << "each finalization is reported once";
    EXPECT_EQ(children.FinalizingCount(), 0u);
}

TEST_F(HostChildrenThreadsTest, AThreadRoleWithoutAnEntryNeverStartsAProcess) {
    // The runtime only chooses a thread for a role with an entry; if that ever breaks, the spawn fails rather than
    // falling back to a process it did not choose.
    RoleEntries entries;
    ChildSupervisor children("fixture.ini", nullptr, nullptr, nullptr, ThreadSetup(entries));
    EXPECT_FALSE(children.Ensure(HostChild::Inject));
    EXPECT_FALSE(children.Present(HostChild::Inject));
    EXPECT_TRUE(children.Shutdown());
}
