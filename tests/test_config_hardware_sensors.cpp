#include "test_config_shared.h"

#include <filesystem>

// Every selector defaults to auto: the bridge now reports a metric it cannot
// actually read as unavailable rather than as a zero, so an unreadable sensor
// costs nothing but a hidden value.
TEST_F(ConfigTest, HardwareSensorsDefaultEverySelectorToAuto) {
    WriteConfig("[Overlay]\nshow_cpu=true\nshow_gpu=true\n");
    AppConfig config;
    LoadConfig(tempConfigFile, config);

    EXPECT_EQ(config.hardwareSensors.enabled, "auto");
    EXPECT_EQ(config.hardwareSensors.pollIntervalMs, 1000u);
    EXPECT_EQ(config.hardwareSensors.cpuTemperature, "auto");
    EXPECT_EQ(config.hardwareSensors.gpuTemperature, "auto");
    EXPECT_EQ(config.hardwareSensors.cpuPackagePower, "auto");
    EXPECT_EQ(config.hardwareSensors.gpuPackagePower, "auto");
    EXPECT_EQ(config.hardwareSensors.gpuFan, "auto");
    EXPECT_EQ(config.hardwareSensors.cpuCoreClock, "auto");
    EXPECT_EQ(config.hardwareSensors.gpuCoreClock, "auto");
    EXPECT_EQ(config.hardwareSensors.gpuMemoryClock, "auto");
    EXPECT_EQ(config.hardwareSensors.gpuVoltage, "auto");
}

TEST_F(ConfigTest, HardwareSensorsAcceptModesIntervalsAndExactIdentifiers) {
    WriteConfig(
        "[HardwareSensors]\n"
        "enabled=ON\n"
        "poll_interval_ms=250\n"
        "cpu_temperature=/cpu/0/temperature/3\n"
        "gpu_temperature=off\n"
        "cpu_package_power=/cpu-amd/0/power/0\n"
        "gpu_package_power=auto\n"
        "gpu_fan=/gpu-nvidia/0/fan/0\n"
        "cpu_core_clock=/amdcpu/0/clock/1\n"
        "gpu_core_clock=off\n"
        "gpu_memory_clock=/gpu-nvidia/0/clock/4\n"
        "gpu_voltage=auto\n");
    AppConfig config;
    LoadConfig(tempConfigFile, config);

    EXPECT_EQ(config.hardwareSensors.enabled, "on");
    EXPECT_EQ(config.hardwareSensors.pollIntervalMs, 250u);
    EXPECT_EQ(config.hardwareSensors.cpuTemperature, "/cpu/0/temperature/3");
    EXPECT_EQ(config.hardwareSensors.gpuTemperature, "off");
    EXPECT_EQ(config.hardwareSensors.cpuPackagePower, "/cpu-amd/0/power/0");
    EXPECT_EQ(config.hardwareSensors.gpuPackagePower, "auto");
    EXPECT_EQ(config.hardwareSensors.gpuFan, "/gpu-nvidia/0/fan/0");
    EXPECT_EQ(config.hardwareSensors.cpuCoreClock, "/amdcpu/0/clock/1");
    EXPECT_EQ(config.hardwareSensors.gpuCoreClock, "off");
    EXPECT_EQ(config.hardwareSensors.gpuMemoryClock, "/gpu-nvidia/0/clock/4");
    EXPECT_EQ(config.hardwareSensors.gpuVoltage, "auto");
}

TEST_F(ConfigTest, HardwareSensorsRejectUnsafeOrOutOfRangeValues) {
    WriteConfig(
        "[HardwareSensors]\n"
        "enabled=maybe\n"
        "poll_interval_ms=249\n"
        "cpu_temperature=/\n"
        "gpu_temperature=/gpu/0/temperature/0;Write-Host\n"
        "cpu_package_power=/cpu/0/power/0 with-space\n"
        "gpu_package_power=/gpu/\xC3\xA4\n"
        "gpu_fan=auto\n"
        "cpu_core_clock=/amdcpu/0/clock/1;rm\n"
        "gpu_voltage=\n");
    AppConfig config;
    LoadConfig(tempConfigFile, config);

    EXPECT_EQ(config.hardwareSensors.enabled, "auto");
    EXPECT_EQ(config.hardwareSensors.pollIntervalMs, 1000u);
    EXPECT_EQ(config.hardwareSensors.cpuTemperature, "auto");
    EXPECT_EQ(config.hardwareSensors.gpuTemperature, "auto");
    // Each rejected selector falls back to its own default independently.
    EXPECT_EQ(config.hardwareSensors.cpuPackagePower, "auto");
    EXPECT_EQ(config.hardwareSensors.gpuPackagePower, "auto");
    EXPECT_EQ(config.hardwareSensors.gpuFan, "auto");
    EXPECT_EQ(config.hardwareSensors.cpuCoreClock, "auto");
    EXPECT_EQ(config.hardwareSensors.gpuVoltage, "auto");
}

TEST_F(ConfigTest, DefaultTemplateDocumentsEveryHardwareSensorControl) {
    const std::string configTemplate = ReadTextFile(DefaultTemplatePath());
    ASSERT_FALSE(configTemplate.empty());
    for (const char* text : {
             "[HardwareSensors]",
             "enabled=auto",
             "poll_interval_ms=1000",
             "cpu_temperature=auto",
             "gpu_temperature=auto",
             "cpu_package_power=auto",
             "gpu_package_power=auto",
             "gpu_fan=auto",
             "cpu_core_clock=auto",
             "gpu_core_clock=auto",
             "gpu_memory_clock=auto",
             "gpu_voltage=auto",
             "dedicated sensor service",
             "administrator",
         }) {
        SCOPED_TRACE(text);
        EXPECT_NE(configTemplate.find(text), std::string::npos);
    }
}

TEST_F(ConfigTest, HardwareSensorChangesRestartTheLongLivedSensorService) {
    const std::string mainInternal =
        ReadTextFile((std::filesystem::current_path() / "captureengine" / "main_internal.h").string());
    const std::string mainEntry =
        ReadTextFile((std::filesystem::current_path() / "captureengine" / "main_entry.cpp").string());
    ASSERT_FALSE(mainInternal.empty());
    ASSERT_FALSE(mainEntry.empty());
    EXPECT_NE(mainInternal.find("HardwareSensorServiceConfigEquals"), std::string::npos);
    EXPECT_NE(mainInternal.find("sensorConfigChanged"), std::string::npos);
    // A selector left out of the comparison would stay latched until the next
    // application launch.
    for (const char* member : {
             "cpuCoreClock",
             "gpuCoreClock",
             "gpuMemoryClock",
             "gpuVoltage",
         }) {
        SCOPED_TRACE(member);
        EXPECT_NE(mainInternal.find(member), std::string::npos);
    }
    EXPECT_NE(mainEntry.find("SyncLoggerAndSensorProcesses(main_g_Config, &oldConfig)"), std::string::npos);

    const std::string sensorService =
        ReadTextFile((std::filesystem::current_path() / "captureengine" / "sensor_service.cpp").string());
    ASSERT_FALSE(sensorService.empty());
    EXPECT_NE(sensorService.find("hardwareSensorPluginActive && effectiveHardwareSensors.pollIntervalMs < 1000"),
              std::string::npos);
}
