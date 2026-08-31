#include <gtest/gtest.h>

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
    ASSERT_TRUE(ce::hardware_sensors::ParseBridgeMessage(
        "CE_LHM_SAMPLE\t17\t58.25\t/cpu/0/temperature/0\t63\t/gpu-nvidia/0/temperature/0\t"
        "0\t/cpu/0/power/0\t241.5\t/gpu-nvidia/0/power/0\t1450\t/gpu-nvidia/0/fan/0",
        message));
    ASSERT_EQ(message.kind, ce::hardware_sensors::BridgeMessageKind::Sample);
    EXPECT_EQ(message.snapshot.sequence, 17u);
    EXPECT_TRUE(message.snapshot.cpuTemperature.valid);
    EXPECT_FLOAT_EQ(message.snapshot.cpuTemperature.value, 58.25f);
    EXPECT_TRUE(message.snapshot.cpuPackagePower.valid);
    EXPECT_FLOAT_EQ(message.snapshot.cpuPackagePower.value, 0.0f);
    EXPECT_TRUE(message.snapshot.gpuFan.valid);
    EXPECT_EQ(message.snapshot.gpuFan.identifier, "/gpu-nvidia/0/fan/0");

    ASSERT_TRUE(ce::hardware_sensors::ParseBridgeMessage(
        "CE_LHM_SAMPLE\t18\t-\t-\t-\t-\t-\t-\t-\t-\t-\t-", message));
    EXPECT_FALSE(message.snapshot.cpuTemperature.valid);
    EXPECT_FALSE(message.snapshot.gpuTemperature.valid);
    EXPECT_FALSE(message.snapshot.cpuPackagePower.valid);
    EXPECT_FALSE(message.snapshot.gpuPackagePower.valid);
    EXPECT_FALSE(message.snapshot.gpuFan.valid);
}

TEST(HardwareSensorBridgeTest, RejectsMalformedOrUnboundedSamples) {
    ce::hardware_sensors::BridgeMessage message;
    for (const char* line : {
             "CE_LHM_SAMPLE\t0\t-\t-\t-\t-\t-\t-\t-\t-\t-\t-",
             "CE_LHM_SAMPLE\t1\tnan\t/cpu/0/temperature/0\t-\t-\t-\t-\t-\t-\t-\t-",
             "CE_LHM_SAMPLE\t1\t251\t/cpu/0/temperature/0\t-\t-\t-\t-\t-\t-\t-\t-",
             "CE_LHM_SAMPLE\t1\t50\t/\t-\t-\t-\t-\t-\t-\t-\t-",
             "CE_LHM_SAMPLE\t1\t50\t/cpu/\xC3\xA4\t-\t-\t-\t-\t-\t-\t-\t-",
             "CE_LHM_SAMPLE\t1\t50\t../unsafe\t-\t-\t-\t-\t-\t-\t-\t-",
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

TEST(HardwareSensorOverlayTest, FormatsOptionalSuffixesWithoutNewRows) {
    char value[96] = {};
    ce::overlay_layout::FormatCpuMetricsValue(value, sizeof(value), 12.4f, 44.4f, true, 58.2f, true, 65.1f);
    EXPECT_STREQ(value, "12% (44%)  58 C  65 W");

    ce::overlay_layout::FormatGpuMetricsValue(value, sizeof(value), true, 98.6f, true, 62.0f, true, 240.0f, true,
                                               1450.0f);
    EXPECT_STREQ(value, "99%  62 C  240 W  1450 RPM");

    ce::overlay_layout::FormatGpuMetricsValue(value, sizeof(value), false, 0.0f, false, 0.0f, false, 0.0f, false,
                                               0.0f);
    EXPECT_STREQ(value, "--");

    ce::overlay_layout::FormatGpuMetricsValue(value, sizeof(value), false, 0.0f, false, 0.0f, true, 0.0f, true,
                                               0.0f);
    EXPECT_STREQ(value, "--  0 W  0 RPM");
}

TEST(HardwareSensorOverlayTest, FormatsOnlyWhenTheCachedOverlayLayoutRefreshes) {
    const std::string implementation = ReadProjectFile("hook/common/overlay_adapter_render.cpp");
    ASSERT_FALSE(implementation.empty());
    EXPECT_EQ(CountOccurrences(implementation, "FormatCpuMetricsValue("), 1u);
    EXPECT_EQ(CountOccurrences(implementation, "FormatGpuMetricsValue("), 1u);
    EXPECT_NE(implementation.find("if (refreshLayout)"), std::string::npos);
    EXPECT_NE(implementation.find("cachedCpuMetricsText"), std::string::npos);
    EXPECT_NE(implementation.find("cachedGpuMetricsText"), std::string::npos);
}

TEST(HardwareSensorBridgeTest, ScriptUsesDirectCpuGpuLibraryVisitorAndInterruptibleWait) {
    const std::string script =
        ReadProjectFile("plugins/LibreHardwareMonitor/CaptureEngine.LibreHardwareMonitor.ps1");
    ASSERT_FALSE(script.empty());
    EXPECT_NE(script.find("IsCpuEnabled = ($CpuTemperature -ne 'off'"), std::string::npos);
    EXPECT_NE(script.find("IsGpuEnabled = ($GpuTemperature -ne 'off'"), std::string::npos);
    EXPECT_NE(script.find("PreviousIdentifier"), std::string::npos);
    EXPECT_NE(script.find("$candidates.Count -gt 1"), std::string::npos);
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
