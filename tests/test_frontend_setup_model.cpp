// The frontend's setup model (frontend/app/setup_model.h): the tray's checkmarks, labels and the parsing of the setup
// failure codes, from fixed runtime status snapshots.
#include "frontend/app/setup_model.h"

#include <gtest/gtest.h>

#include <cstring>

namespace {
using namespace ce::frontend;

ce_setup_status_t Status(int32_t service, bool driver, bool offer, bool elevated, bool preferred, bool autostart) {
    ce_setup_status_t status{};
    status.struct_size = sizeof(status);
    status.elevation_service_state = service;
    status.sensor_driver_installed = driver ? 1u : 0u;
    status.sensor_driver_offer = offer ? 1u : 0u;
    status.launch_elevated = elevated ? 1u : 0u;
    status.elevation_service_preferred = preferred ? 1u : 0u;
    status.client_autostart = autostart ? 1u : 0u;
    return status;
}

TEST(FrontendSetupModelTest, TheServiceLabelNamesTheRegistrationState) {
    EXPECT_EQ(ElevationServiceLabel(2), L"Use elevation service (running)");
    EXPECT_EQ(ElevationServiceLabel(1), L"Use elevation service (installed, stopped)");
    EXPECT_EQ(ElevationServiceLabel(0), L"Use elevation service (not installed)");
    EXPECT_EQ(ElevationServiceLabel(-1), L"Use elevation service (not installed)");
    EXPECT_TRUE(ElevationServiceRegistered(1));
    EXPECT_TRUE(ElevationServiceRegistered(2));
    EXPECT_FALSE(ElevationServiceRegistered(0));
    EXPECT_FALSE(ElevationServiceRegistered(-1));
}

TEST(FrontendSetupModelTest, TheServiceItemIsCheckedOnlyWhenPreferredAndRegistered) {
    EXPECT_TRUE(BuildTraySetupState(Status(2, true, false, false, true, false), false).serviceChecked);
    EXPECT_FALSE(BuildTraySetupState(Status(0, true, false, false, true, false), false).serviceChecked)
        << "preferred but not registered";
    EXPECT_FALSE(BuildTraySetupState(Status(2, true, false, false, false, false), false).serviceChecked)
        << "registered but not preferred";
}

TEST(FrontendSetupModelTest, TheTrayItemsFollowTheStatusSnapshot) {
    const TraySetupState state = BuildTraySetupState(Status(1, false, true, true, false, true), true);
    EXPECT_FALSE(state.pawnIoInstalled);
    EXPECT_TRUE(state.elevatedChecked);
    EXPECT_TRUE(state.autostartChecked);
    EXPECT_TRUE(state.busy);
    EXPECT_EQ(state.serviceText, L"Use elevation service (installed, stopped)");

    const TraySetupState idle = BuildTraySetupState(Status(2, true, false, false, true, false), false);
    EXPECT_TRUE(idle.pawnIoInstalled);
    EXPECT_FALSE(idle.elevatedChecked);
    EXPECT_FALSE(idle.autostartChecked);
    EXPECT_FALSE(idle.busy);
}

TEST(FrontendSetupModelTest, AWindowsErrorIsReadFromTheRuntimesFailureMessage) {
    unsigned long code = 0;
    EXPECT_TRUE(WindowsErrorOf("Windows error 1223", code));
    EXPECT_EQ(code, 1223u);
    EXPECT_TRUE(WindowsErrorOf("Windows error 5", code));
    EXPECT_EQ(code, 5u);
    EXPECT_FALSE(WindowsErrorOf("sensor helper did not exit; the driver was not removed", code));
    EXPECT_FALSE(WindowsErrorOf("Windows error x", code));
    EXPECT_FALSE(WindowsErrorOf("", code));
}

}  // namespace
