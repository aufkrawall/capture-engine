#include <gtest/gtest.h>

#include "captureengine/app/host_children.h"
#include "captureengine/app/recording_session.h"
#include "common/ipc/shared_defs.h"

namespace {
using namespace ce::runtime;

TEST(HostChildrenTest, NoOwnerCannotDispatchOrDiscoverAnUnownedInjectMapping) {
    EXPECT_FALSE(HostChildReady(HostChild::Inject));
    EXPECT_FALSE(SendHostChildCommand(HostChild::Inject, ProcessCommand::ToggleOverlay));
    EXPECT_EQ(PublishHostRecordingIntent(RecordingStartIntent::Video).status, ce::ipc::ControlStatus::Unavailable);
    EXPECT_EQ(PublishHostNotification(OverlayNotificationType::None, 0).status, ce::ipc::ControlStatus::Unavailable);
    ce::ipc::RecordingHealthObservation health{true, true, 77, 5};
    EXPECT_EQ(ReadHostRecordingHealth(health).status, ce::ipc::ControlStatus::Unavailable);
    EXPECT_FALSE(health.live);
    EXPECT_EQ(health.failure, 0u);
}

TEST(HostChildrenTest, HeadlessEndpointOwnershipNeedsNoControllerMainOrTrayWindow) {
    HostChildrenSession owner("fixture.ini", nullptr, nullptr);
    ASSERT_TRUE(owner.IsReady());
    EXPECT_FALSE(HostChildPresent(HostChild::Media));
    EXPECT_FALSE(HostChildRunning(HostChild::Media));
    EXPECT_FALSE(HostChildReady(HostChild::Media));
    EXPECT_FALSE(SendHostChildCommand(HostChild::Media, ProcessCommand::StartRecording));
    EXPECT_EQ(PublishHostRecordingIntent(RecordingStartIntent::Video).status, ce::ipc::ControlStatus::Unavailable);
    HostChildrenSession rejected("other.ini", nullptr, nullptr);
    EXPECT_FALSE(rejected.IsReady());
    EXPECT_TRUE(owner.IsReady());
    EXPECT_TRUE(ShutdownHostChildren());
}

TEST(HostChildrenTest, ShutdownClosesAdmissionAndANewScopeCanAcquireOwnership) {
    {
        HostChildrenSession owner("fixture.ini", nullptr, nullptr);
        ASSERT_TRUE(owner.IsReady());
        EXPECT_TRUE(ShutdownHostChildren());
        // Must stop before the native SpawnChildProcess call: the unit executable
        // is not a product worker and no process is allowed to start here.
        EXPECT_FALSE(EnsureHostChild(HostChild::Inject));
        EXPECT_FALSE(EnsureHostChild(HostChild::Media));
    }
    HostChildrenSession next("fixture.ini", nullptr, nullptr);
    EXPECT_TRUE(next.IsReady());
}
}  // namespace
