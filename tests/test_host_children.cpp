#include <gtest/gtest.h>

#include "runtime/children/host_children.h"
#include "runtime/recording/recording_session.h"
#include "common/ipc/shared_defs.h"

namespace {
using namespace ce::runtime;

// None of these spawn anything: the unit executable is not a product worker, so every case must
// stop before the native SpawnChildProcess call.

TEST(HostChildrenTest, ASupervisorWithoutHelpersCannotDispatchOrDiscoverAnUnownedInjectMapping) {
    ChildSupervisor children("fixture.ini", nullptr, nullptr);
    EXPECT_FALSE(children.Ready(HostChild::Inject));
    EXPECT_FALSE(children.Send(HostChild::Inject, ProcessCommand::ToggleOverlay));
    EXPECT_EQ(children.PublishRecordingIntent(RecordingStartIntent::Video).status, ce::ipc::ControlStatus::Unavailable);
    EXPECT_EQ(children.PublishNotification(OverlayNotificationType::None, 0).status,
              ce::ipc::ControlStatus::Unavailable);
    ce::ipc::RecordingHealthObservation health{true, true, 77, 5};
    EXPECT_EQ(children.ReadRecordingHealth(health).status, ce::ipc::ControlStatus::Unavailable);
    EXPECT_FALSE(health.live);
    EXPECT_EQ(health.failure, 0u);
    children.ClearMediaFailure(5, true);  // no inject mapping: must be a no-op, not a crash
}

TEST(HostChildrenTest, HeadlessEndpointOwnershipNeedsNoControllerMainOrTrayWindow) {
    ChildSupervisor owner("fixture.ini", nullptr, nullptr);
    EXPECT_FALSE(owner.Present(HostChild::Media));
    EXPECT_FALSE(owner.Running(HostChild::Media));
    EXPECT_FALSE(owner.Ready(HostChild::Media));
    EXPECT_FALSE(owner.Send(HostChild::Media, ProcessCommand::StartRecording));
    EXPECT_EQ(owner.PublishRecordingIntent(RecordingStartIntent::Video).status, ce::ipc::ControlStatus::Unavailable);
    EXPECT_TRUE(owner.Shutdown());
}

TEST(HostChildrenTest, ShutdownClosesAdmissionAndAnotherSupervisorOwnsItsOwnState) {
    ChildSupervisor first("fixture.ini", nullptr, nullptr);
    ChildSupervisor second("other.ini", nullptr, nullptr);
    EXPECT_TRUE(first.Shutdown());
    // Admission is closed for the supervisor that shut down, before any spawn call.
    EXPECT_FALSE(first.Ensure(HostChild::Inject));
    EXPECT_FALSE(first.Ensure(HostChild::Media));
    // The other supervisor has its own admission state: it is not closed by the first one's shutdown.
    EXPECT_FALSE(second.Present(HostChild::Inject));
    EXPECT_TRUE(second.Shutdown());
    EXPECT_FALSE(second.Ensure(HostChild::Logger));
}

TEST(HostChildrenTest, ShutdownIsRepeatableAndAClosedSupervisorStartsNothing) {
    ChildSupervisor children("fixture.ini", nullptr, nullptr);
    EXPECT_TRUE(children.Shutdown());
    EXPECT_TRUE(children.Shutdown());
    EXPECT_TRUE(children.StopSensorsForSetup());
    EXPECT_FALSE(children.Running(HostChild::Sensors));
    // Servicing and reconfiguring a closed supervisor must not start anything either.
    children.Service({true, true, false});
    children.ReconfigureServices({true, true, false});
    EXPECT_FALSE(children.Present(HostChild::Logger));
    EXPECT_FALSE(children.Present(HostChild::Sensors));
}

TEST(HostChildrenTest, TheSupervisorIsUsableThroughTheInterfaceCoreComposesAgainst) {
    ChildSupervisor supervisor("fixture.ini", nullptr, nullptr);
    HostChildren& children = supervisor;
    EXPECT_TRUE(children.Shutdown());
    EXPECT_FALSE(children.Ensure(HostChild::Inject));
    EXPECT_FALSE(children.Ready(HostChild::Media));
}

TEST(HostChildrenTest, RetiringWithoutAMediaChildFinalizesNothing) {
    ChildSupervisor children("fixture.ini", nullptr, nullptr);
    children.RetireMedia(42);  // no media child exists: no recording can be finalized by it
    EXPECT_TRUE(children.CollectFinalizedMedia().empty());
    EXPECT_EQ(children.FinalizingCount(), 0u);
    EXPECT_TRUE(children.Shutdown());
}
}  // namespace
