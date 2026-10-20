// The optional PawnIO driver offer's decision (common/setup/sensor_offer_policy.h): every condition on its own, with
// everything else satisfied, so each test changes exactly one input.
#include "common/setup/sensor_offer_policy.h"

#include <gtest/gtest.h>

namespace {

// The settings of a user who uses CPU sensors: the offer is due when the driver is absent and not declined.
HardwareSensorsConfig UsingCpuSensors() {
    HardwareSensorsConfig sensors;
    sensors.enabled = "auto";
    sensors.cpuTemperature = "auto";
    sensors.cpuPackagePower = "off";
    sensors.cpuCoreClock = "off";
    return sensors;
}

TEST(SensorDriverOfferTest, TheOfferIsDueForCpuSensorsWithoutTheDriverOrAnAnswer) {
    EXPECT_TRUE(ce::setup::SensorDriverOfferDue(UsingCpuSensors(), true, false, false));
}

TEST(SensorDriverOfferTest, EachConditionOfTheOfferIsRequired) {
    const HardwareSensorsConfig sensors = UsingCpuSensors();
    EXPECT_FALSE(ce::setup::SensorDriverOfferDue(sensors, false, false, false)) << "the sensor library is absent";
    EXPECT_FALSE(ce::setup::SensorDriverOfferDue(sensors, true, true, false)) << "the driver is installed";
    EXPECT_FALSE(ce::setup::SensorDriverOfferDue(sensors, true, false, true)) << "the user chose don't ask again";
}

TEST(SensorDriverOfferTest, SensingDisabledNeverOffers) {
    HardwareSensorsConfig sensors = UsingCpuSensors();
    sensors.enabled = "off";
    EXPECT_FALSE(ce::setup::SensorDriverOfferDue(sensors, true, false, false));
    sensors.enabled = "on";
    EXPECT_TRUE(ce::setup::SensorDriverOfferDue(sensors, true, false, false)) << "on is still sensing";
}

TEST(SensorDriverOfferTest, AllCpuSelectorsOffNeedNoDriver) {
    HardwareSensorsConfig sensors = UsingCpuSensors();
    sensors.cpuTemperature = "off";
    sensors.cpuPackagePower = "off";
    sensors.cpuCoreClock = "off";
    EXPECT_FALSE(ce::setup::SensorDriverOfferDue(sensors, true, false, false));
    sensors.cpuCoreClock = "auto";
    EXPECT_TRUE(ce::setup::SensorDriverOfferDue(sensors, true, false, false)) << "one CPU selector still uses it";
}

}  // namespace
