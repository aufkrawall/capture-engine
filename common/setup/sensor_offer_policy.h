#pragma once

#include "common/config/config.h"

// The optional PawnIO driver offer (the conditions the startup prompt has always applied). Pure: the caller supplies the
// OS facts, so the decision is testable without a display or a driver.
namespace ce::setup {

// Whether CaptureEngine should offer the driver install: the settings use CPU sensors (the driver feeds them, and
// "off" on every CPU selector means none), the sensor library is installed next to the application, the driver is
// absent and the user has not chosen "don't ask again". `enabled = off` never offers.
inline bool SensorDriverOfferDue(const HardwareSensorsConfig& sensors, bool sensorLibraryPresent,
                                 bool driverInstalled, bool promptSuppressed) {
    if (sensors.enabled == "off" || !sensorLibraryPresent)
        return false;
    if (sensors.cpuTemperature == "off" && sensors.cpuPackagePower == "off" && sensors.cpuCoreClock == "off")
        return false;
    return !driverInstalled && !promptSuppressed;
}

}  // namespace ce::setup
