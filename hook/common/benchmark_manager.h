#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "benchmark_config.h"
#include "shared_defs.h"
#include "system_metrics.h"

enum class BenchmarkState {
    Idle = 0,
    Delaying = 1,
    Recording = 2,
    Results = 3,
};

enum BenchmarkSensorFlags : uint32_t {
    kBenchSensorCpuUsage      = 1u << 0,
    kBenchSensorCpuMaxCore    = 1u << 1,
    kBenchSensorRam           = 1u << 2,
    kBenchSensorVram          = 1u << 3,
    kBenchSensorGpuUsage      = 1u << 4,
    kBenchSensorCpuTemp       = 1u << 5,
    kBenchSensorGpuTemp       = 1u << 6,
    kBenchSensorCpuPower      = 1u << 7,
    kBenchSensorGpuPower      = 1u << 8,
    kBenchSensorGpuFan        = 1u << 9,
    kBenchSensorCpuClock      = 1u << 10,
    kBenchSensorGpuClock      = 1u << 11,
    kBenchSensorGpuMemClock   = 1u << 12,
    kBenchSensorGpuVoltage    = 1u << 13,
};

struct BenchmarkFrameRecord {
    float timeSeconds = 0.0f;
    float presentationFrameTimeMs = 0.0f;
    float displayFrameTimeMs = 0.0f;
    float presentationFps = 0.0f;
    float displayFps = 0.0f;
    float cpuUsage = 0.0f;
    float cpuMaxCoreUsage = 0.0f;
    float ramUsedGb = 0.0f;
    float ramTotalGb = 0.0f;
    float vramUsedGb = 0.0f;
    float vramTotalGb = 0.0f;
    float gpuUsage = 0.0f;
    float cpuTempC = 0.0f;
    float gpuTempC = 0.0f;
    float cpuPowerW = 0.0f;
    float gpuPowerW = 0.0f;
    float gpuFanRpm = 0.0f;
    float cpuClockMhz = 0.0f;
    float gpuClockMhz = 0.0f;
    float gpuMemClockMhz = 0.0f;
    float gpuVoltageV = 0.0f;
    uint32_t validMask = 0;
};

struct BenchmarkStats {
    float avgFps = 0.0f;
    float maxFps = 0.0f;
    float minFps = 0.0f;
    float onePercentLowFps = 0.0f;
    float zeroPointOnePercentLowFps = 0.0f;
    float avgFrameTimeMs = 0.0f;
};

struct BenchmarkSensorSummary {
    float avg = 0.0f;
    float max = 0.0f;
    float min = 0.0f;
    bool valid = false;
};

struct BenchmarkResults {
    std::string profileName;
    std::string executableName;
    std::string cpuName;
    std::string gpuName;
    std::string timestampStr;
    std::string htmlFilePath;
    float durationSeconds = 0.0f;
    uint64_t totalFrames = 0;

    BenchmarkStats presentationStats;
    BenchmarkStats displayStats;

    BenchmarkSensorSummary cpuUsage;
    BenchmarkSensorSummary cpuMaxCoreUsage;
    BenchmarkSensorSummary ramUsedGb;
    float ramTotalGb = 0.0f;
    BenchmarkSensorSummary vramUsedGb;
    float vramTotalGb = 0.0f;
    BenchmarkSensorSummary gpuUsage;
    BenchmarkSensorSummary cpuTemp;
    BenchmarkSensorSummary gpuTemp;
    BenchmarkSensorSummary cpuPower;
    BenchmarkSensorSummary gpuPower;
    BenchmarkSensorSummary gpuFan;
    BenchmarkSensorSummary cpuClock;
    BenchmarkSensorSummary gpuClock;
    BenchmarkSensorSummary gpuMemClock;
    BenchmarkSensorSummary gpuVoltage;

    std::vector<BenchmarkFrameRecord> records;
};

class BenchmarkManager {
public:
    static BenchmarkManager& Get();

    void Init(const BenchmarkConfig& config, const std::string& profileName);
    void UpdateConfig(const BenchmarkConfig& config, const std::string& profileName);

    void OnFrame(int64_t currentQpcUs, float presentationFrameTimeMs, float displayFrameTimeMs,
                 const SystemMetrics& sysMetrics, SharedMemoryLayout* sharedMem);

    void Toggle();

    BenchmarkState GetState() const;
    bool IsActiveOrShowingResults() const;

    float GetDelayRemainingSeconds() const;
    float GetRecordingElapsedSeconds() const;
    uint64_t GetRecordingFrameCount() const;
    uint32_t GetDurationSeconds() const;

    const BenchmarkResults& GetResults() const;

    static BenchmarkStats CalculateStats(const std::vector<float>& frameTimesMs);
    static void ComputeSensorSummary(const std::vector<BenchmarkFrameRecord>& records,
                                     BenchmarkSensorSummary& summary,
                                     uint32_t validFlag,
                                     float BenchmarkFrameRecord::*member);

private:
    BenchmarkManager();

    void StartDelayOrRecording();
    void StartRecording();
    void StopRecordingAndPublish();
    void ClearResults();

    mutable std::mutex m_mutex;
    BenchmarkState m_state = BenchmarkState::Idle;
    BenchmarkConfig m_config;
    std::string m_profileName;
    std::string m_executableName;
    uint64_t m_lastObservedToggleSeq = 0;

    int64_t m_delayStartQpcUs = 0;
    int64_t m_recordingStartQpcUs = 0;
    int64_t m_lastFrameQpcUs = 0;

    std::vector<BenchmarkFrameRecord> m_records;
    BenchmarkResults m_results;
};
