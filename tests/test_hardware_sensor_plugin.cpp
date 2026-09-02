#include <gtest/gtest.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include "../captureengine/sensor_plugin.h"
#include "../hook/common/overlay_layout_policy.h"

namespace {

std::string ReadProjectFile(const std::filesystem::path& relativePath) {
    const std::filesystem::path path = std::filesystem::current_path() / relativePath;
    std::ifstream input(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

size_t CountOccurrences(const std::string& text, const std::string& needle) {
    size_t count = 0;
    for (size_t offset = 0; (offset = text.find(needle, offset)) != std::string::npos; offset += needle.size())
        ++count;
    return count;
}

}  // namespace

TEST(HardwareSensorBridgeTest, ParsesCompleteAndUnavailableSamples) {
    ce::hardware_sensors::BridgeMessage message;
    ASSERT_TRUE(ce::hardware_sensors::ParseBridgeMessage("CE_LHM_SAMPLE\t17\t58.25\t/cpu/0/temperature/0\t63\t/gpu-nvidia/0/temperature/0\t95.5\t/cpu/0/power/0\t241.5\t/gpu-nvidia/0/power/0\t1450\t/gpu-nvidia/0/fan/0\t4800\t/cpu/0/clock/1\t2700\t/gpu-nvidia/0/clock/0\t810\t/gpu-nvidia/0/clock/4\t0.805\t/gpu-nvidia/0/voltage/0", message));
    ASSERT_EQ(message.kind, ce::hardware_sensors::BridgeMessageKind::Sample);
    EXPECT_EQ(message.snapshot.sequence, 17u);
    EXPECT_TRUE(message.snapshot.cpuTemperature.valid);
    EXPECT_FLOAT_EQ(message.snapshot.cpuTemperature.value, 58.25f);
    EXPECT_TRUE(message.snapshot.cpuPackagePower.valid);
    EXPECT_FLOAT_EQ(message.snapshot.cpuPackagePower.value, 95.5f);
    EXPECT_TRUE(message.snapshot.gpuFan.valid);
    EXPECT_EQ(message.snapshot.gpuFan.identifier, "/gpu-nvidia/0/fan/0");
    EXPECT_TRUE(message.snapshot.cpuCoreClock.valid);
    EXPECT_FLOAT_EQ(message.snapshot.cpuCoreClock.value, 4800.0f);
    EXPECT_TRUE(message.snapshot.gpuCoreClock.valid);
    EXPECT_EQ(message.snapshot.gpuCoreClock.identifier, "/gpu-nvidia/0/clock/0");
    EXPECT_TRUE(message.snapshot.gpuMemoryClock.valid);
    EXPECT_FLOAT_EQ(message.snapshot.gpuMemoryClock.value, 810.0f);
    EXPECT_TRUE(message.snapshot.gpuVoltage.valid);
    EXPECT_FLOAT_EQ(message.snapshot.gpuVoltage.value, 0.805f);

    ASSERT_TRUE(ce::hardware_sensors::ParseBridgeMessage("CE_LHM_SAMPLE\t18\t-\t-\t-\t-\t-\t-\t-\t-\t-\t-\t-\t-\t-\t-\t-\t-\t-\t-", message));
    EXPECT_FALSE(message.snapshot.cpuTemperature.valid);
    EXPECT_FALSE(message.snapshot.gpuTemperature.valid);
    EXPECT_FALSE(message.snapshot.cpuPackagePower.valid);
    EXPECT_FALSE(message.snapshot.gpuPackagePower.valid);
    EXPECT_FALSE(message.snapshot.gpuFan.valid);
    EXPECT_FALSE(message.snapshot.cpuCoreClock.valid);
    EXPECT_FALSE(message.snapshot.gpuCoreClock.valid);
    EXPECT_FALSE(message.snapshot.gpuMemoryClock.valid);
    EXPECT_FALSE(message.snapshot.gpuVoltage.valid);
}

// Without elevation LibreHardwareMonitor cannot open its kernel driver and every
// CPU power and clock rail reads exactly 0, which used to reach the overlay as a
// confident "0 W". The bridge now suppresses those rails to "-", and a zero that
// still arrives carrying an identifier is malformed for every metric except a
// fan, which really can be stopped.
TEST(HardwareSensorBridgeTest, RejectsUnreadableZeroRailsButKeepsAStoppedFan) {
    ce::hardware_sensors::BridgeMessage message;
    ASSERT_TRUE(ce::hardware_sensors::ParseBridgeMessage("CE_LHM_SAMPLE\t19\t-\t-\t41\t/gpu-nvidia/0/temperature/0\t-\t-\t180.25\t/gpu-nvidia/0/power/0\t0\t/gpu-nvidia/0/fan/1\t-\t-\t210\t/gpu-nvidia/0/clock/0\t810\t/gpu-nvidia/0/clock/4\t0.72\t/gpu-nvidia/0/voltage/0", message));
    ASSERT_EQ(message.kind, ce::hardware_sensors::BridgeMessageKind::Sample);
    EXPECT_TRUE(message.snapshot.gpuFan.valid);
    EXPECT_FLOAT_EQ(message.snapshot.gpuFan.value, 0.0f);
    EXPECT_EQ(message.snapshot.gpuFan.identifier, "/gpu-nvidia/0/fan/1");
    EXPECT_FALSE(message.snapshot.cpuTemperature.valid);
    EXPECT_FALSE(message.snapshot.cpuPackagePower.valid);
    EXPECT_FALSE(message.snapshot.cpuCoreClock.valid);
    EXPECT_TRUE(message.snapshot.gpuTemperature.valid);
    EXPECT_TRUE(message.snapshot.gpuCoreClock.valid);
    EXPECT_TRUE(message.snapshot.gpuVoltage.valid);

    for (const char* line : {
             "CE_LHM_SAMPLE\t20\t0\t/cpu/0/temperature/0\t-\t-\t-\t-\t-\t-\t-\t-\t-\t-\t-\t-\t-\t-\t-\t-",
             "CE_LHM_SAMPLE\t20\t-\t-\t-\t-\t0\t/cpu/0/power/0\t-\t-\t-\t-\t-\t-\t-\t-\t-\t-\t-\t-",
             "CE_LHM_SAMPLE\t20\t-\t-\t-\t-\t-\t-\t0\t/gpu-nvidia/0/power/0\t-\t-\t-\t-\t-\t-\t-\t-\t-\t-",
             "CE_LHM_SAMPLE\t20\t-\t-\t-\t-\t-\t-\t-\t-\t-\t-\t0\t/amdcpu/0/clock/1\t-\t-\t-\t-\t-\t-",
             "CE_LHM_SAMPLE\t20\t-\t-\t-\t-\t-\t-\t-\t-\t-\t-\t-\t-\t-\t-\t-\t-\t0\t/gpu-nvidia/0/voltage/0",
         }) {
        SCOPED_TRACE(line);
        EXPECT_FALSE(ce::hardware_sensors::ParseBridgeMessage(line, message));
        EXPECT_EQ(message.kind, ce::hardware_sensors::BridgeMessageKind::Invalid);
    }

    // The same zero on the fan is a real stopped-fan reading, not a rejection.
    ASSERT_TRUE(ce::hardware_sensors::ParseBridgeMessage("CE_LHM_SAMPLE\t20\t-\t-\t-\t-\t-\t-\t-\t-\t0\t/gpu-nvidia/0/fan/1\t-\t-\t-\t-\t-\t-\t-\t-", message));
    EXPECT_TRUE(message.snapshot.gpuFan.valid);
}

// A shorter line is the previous five-metric wire format. It must be rejected
// outright rather than parsed into shifted fields.
TEST(HardwareSensorBridgeTest, RejectsTheSupersededNarrowerSampleFormat) {
    ce::hardware_sensors::BridgeMessage message;
    EXPECT_FALSE(ce::hardware_sensors::ParseBridgeMessage(
        "CE_LHM_SAMPLE\t17\t58.25\t/cpu/0/temperature/0\t63\t/gpu-nvidia/0/temperature/0\t"
        "95.5\t/cpu/0/power/0\t241.5\t/gpu-nvidia/0/power/0\t1450\t/gpu-nvidia/0/fan/0",
        message));
    EXPECT_EQ(message.kind, ce::hardware_sensors::BridgeMessageKind::Invalid);
}

TEST(HardwareSensorBridgeTest, RejectsMalformedOrUnboundedSamples) {
    ce::hardware_sensors::BridgeMessage message;
    for (const char* line : {
             "CE_LHM_SAMPLE\t0\t-\t-\t-\t-\t-\t-\t-\t-\t-\t-\t-\t-\t-\t-\t-\t-\t-\t-",
             "CE_LHM_SAMPLE\t1\tnan\t/cpu/0/temperature/0\t-\t-\t-\t-\t-\t-\t-\t-\t-\t-\t-\t-\t-\t-\t-\t-",
             "CE_LHM_SAMPLE\t1\t251\t/cpu/0/temperature/0\t-\t-\t-\t-\t-\t-\t-\t-\t-\t-\t-\t-\t-\t-\t-\t-",
             "CE_LHM_SAMPLE\t1\t50\t/\t-\t-\t-\t-\t-\t-\t-\t-\t-\t-\t-\t-\t-\t-\t-\t-",
             "CE_LHM_SAMPLE\t1\t50\t/cpu/\xC3\xA4\t-\t-\t-\t-\t-\t-\t-\t-\t-\t-\t-\t-\t-\t-\t-\t-",
             "CE_LHM_SAMPLE\t1\t50\t../unsafe\t-\t-\t-\t-\t-\t-\t-\t-\t-\t-\t-\t-\t-\t-\t-\t-",
             "CE_LHM_SAMPLE\t1\t0\t/cpu/0/temperature/0\t-\t-\t-\t-\t-\t-\t-\t-\t-\t-\t-\t-\t-\t-\t-\t-",
             "CE_LHM_SAMPLE\t1\t-\t-\t-\t-\t-\t-\t-\t-\t-\t-\t20001\t/cpu/0/clock/1\t-\t-\t-\t-\t-\t-",
             "CE_LHM_SAMPLE\t1\t50\t/cpu/0/temperature/0\t-\t-\t-\t-\t-\t-",
             "arbitrary output",
         }) {
        SCOPED_TRACE(line);
        EXPECT_FALSE(ce::hardware_sensors::ParseBridgeMessage(line, message));
        EXPECT_EQ(message.kind, ce::hardware_sensors::BridgeMessageKind::Invalid);
    }
}

TEST(HardwareSensorBridgeTest, BoundsControlMessagesAndStaleSamples) {
    ce::hardware_sensors::BridgeMessage message;
    ASSERT_TRUE(ce::hardware_sensors::ParseBridgeMessage("CE_LHM_READY\t0.9.6.0", message));
    EXPECT_EQ(message.kind, ce::hardware_sensors::BridgeMessageKind::Ready);
    EXPECT_EQ(message.detail, "0.9.6.0");
    ASSERT_TRUE(ce::hardware_sensors::ParseBridgeMessage("CE_LHM_ERROR\tFileLoadException", message));
    EXPECT_EQ(message.kind, ce::hardware_sensors::BridgeMessageKind::Error);
    EXPECT_FALSE(ce::hardware_sensors::ParseBridgeMessage("CE_LHM_ERROR\tpath leaked!", message));
    EXPECT_FALSE(ce::hardware_sensors::ParseBridgeMessage(std::string(4097, 'A'), message));
    std::string embeddedNull = "CE_LHM_READY\t0.9.6.0";
    embeddedNull.push_back('\0');
    EXPECT_FALSE(ce::hardware_sensors::ParseBridgeMessage(embeddedNull, message));

    ce::hardware_sensors::HardwareSensorSnapshot snapshot;
    snapshot.receivedTickMs = 1000;
    EXPECT_TRUE(ce::hardware_sensors::IsSnapshotFresh(snapshot, 6000, 1000));
    EXPECT_FALSE(ce::hardware_sensors::IsSnapshotFresh(snapshot, 6001, 1000));
    EXPECT_TRUE(ce::hardware_sensors::IsSnapshotFresh(snapshot, 30999, 10000));
    EXPECT_FALSE(ce::hardware_sensors::IsSnapshotFresh(snapshot, 31001, 10000));
    EXPECT_FALSE(ce::hardware_sensors::IsSnapshotFresh(snapshot, 999, 1000));
}

TEST(HardwareSensorOverlayTest, FormatsOptionalSuffixesOnTheUsageRows) {
    char value[96] = {};
    ce::overlay_layout::FormatCpuMetricsValue(value, sizeof(value), 12.4f, 44.4f, true, 58.2f, true, 65.1f);
    EXPECT_STREQ(value, "12% (44%)  58 C  65 W");

    ce::overlay_layout::FormatGpuMetricsValue(value, sizeof(value), true, 98.6f, true, 62.0f, true, 240.0f, true,
                                              1450.0f);
    EXPECT_STREQ(value, "99%  62 C  240 W  1450 RPM");

    ce::overlay_layout::FormatGpuMetricsValue(value, sizeof(value), false, 0.0f, false, 0.0f, false, 0.0f, false,
                                              0.0f);
    EXPECT_STREQ(value, "--");

    ce::overlay_layout::FormatGpuMetricsValue(value, sizeof(value), false, 0.0f, false, 0.0f, false, 0.0f, true,
                                              0.0f);
    EXPECT_STREQ(value, "--  0 RPM");
}

// The load span and the appended readings are colored separately, so the split
// offset must land exactly after the percentage and must still be well defined
// when nothing is appended.
TEST(HardwareSensorOverlayTest, ReportsWhereTheLoadColoredSpanEnds) {
    char value[96] = {};
    size_t sensorOffset = 0;
    ce::overlay_layout::FormatGpuMetricsValue(value, sizeof(value), true, 98.6f, true, 62.0f, true, 240.0f, true,
                                              1450.0f, &sensorOffset);
    EXPECT_STREQ(value, "99%  62 C  240 W  1450 RPM");
    ASSERT_LT(sensorOffset, strlen(value));
    EXPECT_EQ(std::string(value, sensorOffset), "99%");
    EXPECT_EQ(std::string(value + sensorOffset), "  62 C  240 W  1450 RPM");

    sensorOffset = 0;
    ce::overlay_layout::FormatGpuMetricsValue(value, sizeof(value), true, 98.6f, false, 0.0f, false, 0.0f, false,
                                              0.0f, &sensorOffset);
    EXPECT_STREQ(value, "99%");
    EXPECT_EQ(sensorOffset, strlen(value));

    sensorOffset = 0;
    ce::overlay_layout::FormatCpuMetricsValue(value, sizeof(value), 12.4f, 44.4f, true, 58.2f, true, 65.1f,
                                              &sensorOffset);
    EXPECT_EQ(std::string(value, sensorOffset), "12% (44%)");
}

TEST(HardwareSensorOverlayTest, FormatsTheClockRowsAndOmitsUnreadableValues) {
    char value[64] = {};
    ce::overlay_layout::FormatGpuClocksValue(value, sizeof(value), true, 2700.0f, true, 810.0f, true, 0.805f);
    EXPECT_STREQ(value, "2700 MHz  810 MHz  0.805 V");

    ce::overlay_layout::FormatGpuClocksValue(value, sizeof(value), false, 0.0f, true, 810.0f, false, 0.0f);
    EXPECT_STREQ(value, "810 MHz");

    ce::overlay_layout::FormatGpuClocksValue(value, sizeof(value), false, 0.0f, false, 0.0f, false, 0.0f);
    EXPECT_STREQ(value, "");

    ce::overlay_layout::FormatCpuClocksValue(value, sizeof(value), true, 4825.0f);
    EXPECT_STREQ(value, "4825 MHz");

    ce::overlay_layout::FormatCpuClocksValue(value, sizeof(value), false, 0.0f);
    EXPECT_STREQ(value, "");
}

// An unelevated run reads no CPU clock at all; reserving the row anyway would
// leave a labelled blank line in the overlay.
TEST(HardwareSensorOverlayTest, ReservesAClockRowOnlyWhenItHasAValue) {
    ce::overlay_layout::RowInputs inputs = {};
    inputs.showGPU = true;
    inputs.showCPU = true;
    uint32_t mask = ce::overlay_layout::BuildOverlayRowMask(inputs);
    EXPECT_EQ(mask & ce::overlay_layout::kRowGPUClocks, 0u);
    EXPECT_EQ(mask & ce::overlay_layout::kRowCPUClocks, 0u);

    inputs.showGPUClocks = true;
    inputs.showCPUClocks = true;
    mask = ce::overlay_layout::BuildOverlayRowMask(inputs);
    EXPECT_NE(mask & ce::overlay_layout::kRowGPUClocks, 0u);
    EXPECT_NE(mask & ce::overlay_layout::kRowCPUClocks, 0u);

    // The clock rows follow their usage row, so hiding CPU or GPU hides both.
    inputs.showGPU = false;
    inputs.showCPU = false;
    mask = ce::overlay_layout::BuildOverlayRowMask(inputs);
    EXPECT_EQ(mask & ce::overlay_layout::kRowGPUClocks, 0u);
    EXPECT_EQ(mask & ce::overlay_layout::kRowCPUClocks, 0u);
}

TEST(HardwareSensorOverlayTest, FormatsOnlyWhenTheCachedOverlayLayoutRefreshes) {
    const std::string implementation = ReadProjectFile("hook/common/overlay_adapter_render.cpp");
    ASSERT_FALSE(implementation.empty());
    EXPECT_EQ(CountOccurrences(implementation, "FormatCpuMetricsValue("), 1u);
    EXPECT_EQ(CountOccurrences(implementation, "FormatGpuMetricsValue("), 1u);
    EXPECT_EQ(CountOccurrences(implementation, "FormatGpuClocksValue("), 1u);
    EXPECT_EQ(CountOccurrences(implementation, "FormatCpuClocksValue("), 1u);
    EXPECT_NE(implementation.find("if (refreshLayout)"), std::string::npos);
    EXPECT_NE(implementation.find("cachedCpuMetricsText"), std::string::npos);
    EXPECT_NE(implementation.find("cachedGpuMetricsText"), std::string::npos);
    EXPECT_NE(implementation.find("cachedGpuClocksText"), std::string::npos);
    EXPECT_NE(implementation.find("cachedCpuClocksText"), std::string::npos);
}

// The whole GPU row used to be drawn in one load-derived color, so at 99% load
// the temperature, power and fan readings turned red with it.
TEST(HardwareSensorOverlayTest, DrawsSensorReadingsOutsideTheLoadColoredSpan) {
    const std::string implementation = ReadProjectFile("hook/common/overlay_adapter_render.cpp");
    ASSERT_FALSE(implementation.empty());
    EXPECT_NE(implementation.find("Colors::SensorValue"), std::string::npos);
    EXPECT_NE(implementation.find("DrawMetricsRowValue(cachedGpuMetricsText, cachedGpuSensorOffset"),
              std::string::npos);
    EXPECT_NE(implementation.find("DrawMetricsRowValue(cachedCpuMetricsText, cachedCpuSensorOffset"),
              std::string::npos);
    // GetLoadColor must reach the row only through the split-span helper.
    EXPECT_EQ(CountOccurrences(implementation, "DrawTextRightAligned(valueRightEdge, cursorY, cachedGpuMetricsText"),
              0u);
}

TEST(HardwareSensorBridgeTest, ScriptUsesDirectCpuGpuLibraryVisitorAndInterruptibleWait) {
    const std::string script =
        ReadProjectFile("plugins/LibreHardwareMonitor/CaptureEngine.LibreHardwareMonitor.ps1");
    ASSERT_FALSE(script.empty());
    EXPECT_NE(script.find("$computer.IsCpuEnabled = $cpuRequested"), std::string::npos);
    EXPECT_NE(script.find("$computer.IsGpuEnabled = $gpuRequested"), std::string::npos);
    EXPECT_NE(script.find("PreviousIdentifier"), std::string::npos);
    EXPECT_NE(script.find("$candidates.Count -gt 1"), std::string::npos);
    // Indexed sensor names ("GPU Fan 1"/"GPU Fan 2") must resolve by lowest
    // index. Falling through to the highest-value comparison reselected a
    // different physical fan on nearly every poll.
    EXPECT_NE(script.find("[regex]::Escape($name)"), std::string::npos);
    EXPECT_NE(script.find("$selectedIndex = [int]::MaxValue"), std::string::npos);
    EXPECT_NE(script.find("Test-SensorUsable"), std::string::npos);
    EXPECT_NE(script.find("RejectZero"), std::string::npos);
    for (const char* parameter : {
             "$CpuCoreClock",
             "$GpuCoreClock",
             "$GpuMemoryClock",
             "$GpuVoltage",
         }) {
        SCOPED_TRACE(parameter);
        EXPECT_NE(script.find(parameter), std::string::npos);
    }
    EXPECT_NE(script.find("WaitOne($PollIntervalMs)"), std::string::npos);
    EXPECT_NE(script.find("CE_LHM_SAMPLE"), std::string::npos);
    EXPECT_EQ(script.find("IsMotherboardEnabled"), std::string::npos);
    EXPECT_EQ(script.find("ManagementObject"), std::string::npos);
    EXPECT_EQ(script.find("HttpClient"), std::string::npos);
}

TEST(HardwareSensorBridgeTest, NativeHostContainsTheChildAndRestrictsInheritedHandles) {
    const std::string implementation = ReadProjectFile("captureengine/sensor_plugin.cpp");
    const std::string service = ReadProjectFile("captureengine/sensor_service.cpp");
    ASSERT_FALSE(implementation.empty());
    ASSERT_FALSE(service.empty());
    EXPECT_NE(implementation.find("JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE"), std::string::npos);
    EXPECT_NE(implementation.find("CREATE_SUSPENDED"), std::string::npos);
    EXPECT_NE(implementation.find("AssignProcessToJobObject"), std::string::npos);
    EXPECT_NE(implementation.find("PROC_THREAD_ATTRIBUTE_HANDLE_LIST"), std::string::npos);
    for (const char* argument : {
             "-CpuCoreClock",
             "-GpuCoreClock",
             "-GpuMemoryClock",
             "-GpuVoltage",
         }) {
        SCOPED_TRACE(argument);
        EXPECT_NE(implementation.find(argument), std::string::npos);
    }
    EXPECT_NE(implementation.find("STARTF_FORCEOFFFEEDBACK"), std::string::npos);
    EXPECT_NE(service.find("OpenProcess(SYNCHRONIZE, FALSE, controllerPid)"), std::string::npos);
    EXPECT_NE(service.find("WaitForMultipleObjects"), std::string::npos);
}

TEST(HardwareSensorBridgeTest, RuntimeAndSetupAgreeOnTheFourUserSuppliedFiles) {
    const std::string implementation = ReadProjectFile("captureengine/sensor_plugin.cpp");
    const std::string setup = ReadProjectFile("plugins/LibreHardwareMonitor/README.txt");
    ASSERT_FALSE(implementation.empty());
    ASSERT_FALSE(setup.empty());
    for (const char* filename : {
             "LibreHardwareMonitorLib.dll",
             "System.Memory.dll",
             "System.Numerics.Vectors.dll",
             "System.Runtime.CompilerServices.Unsafe.dll",
         }) {
        SCOPED_TRACE(filename);
        EXPECT_NE(implementation.find(filename), std::string::npos);
        EXPECT_NE(setup.find(filename), std::string::npos);
    }
    for (const char* filename : {
             "Aga.Controls.dll",
             "BlackSharp.Core.dll",
             "DiskInfoToolkit.dll",
             "HidSharp.dll",
             "RAMSPDToolkit-NDD.dll",
             "System.Buffers.dll",
         }) {
        SCOPED_TRACE(filename);
        EXPECT_EQ(implementation.find(filename), std::string::npos);
        EXPECT_EQ(setup.find(filename), std::string::npos);
    }
}
