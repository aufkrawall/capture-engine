#include "benchmark_manager.h"

#include <windows.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <iomanip>
#include <sstream>

#include "benchmark_html_report.h"
#include "hook_common.h"
#include "logging.h"
#include "perf_logger.h"

BenchmarkManager::BenchmarkManager() {
    wchar_t exePath[MAX_PATH] = {};
    if (GetModuleFileNameW(NULL, exePath, MAX_PATH) > 0) {
        std::wstring wpath(exePath);
        const size_t lastSlash = wpath.find_last_of(L"\\/");
        std::wstring wname = (lastSlash != std::wstring::npos) ? wpath.substr(lastSlash + 1) : wpath;
        const size_t dot = wname.find_last_of(L'.');
        if (dot != std::wstring::npos) {
            wname = wname.substr(0, dot);
        }
        int len = WideCharToMultiByte(CP_UTF8, 0, wname.c_str(), -1, nullptr, 0, nullptr, nullptr);
        if (len > 1) {
            m_executableName.resize(len - 1);
            WideCharToMultiByte(CP_UTF8, 0, wname.c_str(), -1, m_executableName.data(), len, nullptr, nullptr);
        }
    }
    if (m_executableName.empty()) {
        m_executableName = "Application";
    }
}

BenchmarkManager& BenchmarkManager::Get() {
    static BenchmarkManager instance;
    return instance;
}

void BenchmarkManager::Init(const BenchmarkConfig& config, const std::string& profileName) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_config = config;
    if (!profileName.empty()) {
        m_profileName = profileName;
    }
}

void BenchmarkManager::UpdateConfig(const BenchmarkConfig& config, const std::string& profileName) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_config = config;
    if (!profileName.empty()) {
        m_profileName = profileName;
    }
}

void BenchmarkManager::Toggle() {
    std::lock_guard<std::mutex> lock(m_mutex);
    switch (m_state) {
        case BenchmarkState::Idle:
            StartDelayOrRecording();
            break;
        case BenchmarkState::Delaying:
            HookLog("[Benchmark] Countdown cancelled by user hotkey");
            m_state = BenchmarkState::Idle;
            break;
        case BenchmarkState::Recording:
            HookLog("[Benchmark] Recording stopped by user hotkey");
            StopRecordingAndPublish();
            break;
        case BenchmarkState::Results:
            HookLog("[Benchmark] Results dismissed by user hotkey");
            ClearResults();
            break;
    }
}

void BenchmarkManager::StartDelayOrRecording() {
    if (m_config.startDelaySeconds > 0) {
        m_state = BenchmarkState::Delaying;
        m_delayStartQpcUs = PerfLogger::GetQpcUs();
        HookLogImportant("[Benchmark] Countdown started (%u s delay)", m_config.startDelaySeconds);
    } else {
        StartRecording();
    }
}

void BenchmarkManager::StartRecording() {
    m_state = BenchmarkState::Recording;
    m_recordingStartQpcUs = PerfLogger::GetQpcUs();
    m_lastFrameQpcUs = m_recordingStartQpcUs;
    m_records.clear();
    m_records.reserve(7200);  // Reserve ~1 min at 120 FPS
    HookLogImportant("[Benchmark] Recording started (duration=%u s)", m_config.durationSeconds);
}

void BenchmarkManager::StopRecordingAndPublish() {
    if (m_records.empty()) {
        HookLog("[Benchmark] No frames recorded, returning to Idle");
        m_state = BenchmarkState::Idle;
        return;
    }

    m_results = BenchmarkResults{};
    m_results.profileName = m_profileName.empty() ? m_executableName : m_profileName;
    m_results.executableName = m_executableName;
    m_results.cpuName = SystemMetricsCollector::Get().GetCPUName();
    m_results.gpuName = SystemMetricsCollector::Get().GetGPUName();
    m_results.durationSeconds = m_records.back().timeSeconds;
    m_results.totalFrames = m_records.size();

    // Timestamp formatting
    const auto now = std::chrono::system_clock::now();
    const std::time_t nowTime = std::chrono::system_clock::to_time_t(now);
    std::tm tmBuf = {};
    localtime_s(&tmBuf, &nowTime);
    std::ostringstream ss;
    ss << std::put_time(&tmBuf, "%Y-%m-%d %H:%M:%S");
    m_results.timestampStr = ss.str();

    // Extract frame times for statistics
    std::vector<float> presFrameTimes;
    std::vector<float> dispFrameTimes;
    presFrameTimes.reserve(m_records.size());
    dispFrameTimes.reserve(m_records.size());
    for (const auto& r : m_records) {
        presFrameTimes.push_back(r.presentationFrameTimeMs);
        dispFrameTimes.push_back(r.displayFrameTimeMs);
    }

    m_results.presentationStats = CalculateStats(presFrameTimes);
    m_results.displayStats = CalculateStats(dispFrameTimes);

    // Compute hardware sensor summaries
    ComputeSensorSummary(m_records, m_results.cpuUsage, kBenchSensorCpuUsage, &BenchmarkFrameRecord::cpuUsage);
    ComputeSensorSummary(m_records, m_results.cpuMaxCoreUsage, kBenchSensorCpuMaxCore, &BenchmarkFrameRecord::cpuMaxCoreUsage);
    ComputeSensorSummary(m_records, m_results.ramUsedGb, kBenchSensorRam, &BenchmarkFrameRecord::ramUsedGb);
    ComputeSensorSummary(m_records, m_results.vramUsedGb, kBenchSensorVram, &BenchmarkFrameRecord::vramUsedGb);
    ComputeSensorSummary(m_records, m_results.gpuUsage, kBenchSensorGpuUsage, &BenchmarkFrameRecord::gpuUsage);
    ComputeSensorSummary(m_records, m_results.cpuTemp, kBenchSensorCpuTemp, &BenchmarkFrameRecord::cpuTempC);
    ComputeSensorSummary(m_records, m_results.gpuTemp, kBenchSensorGpuTemp, &BenchmarkFrameRecord::gpuTempC);
    ComputeSensorSummary(m_records, m_results.cpuPower, kBenchSensorCpuPower, &BenchmarkFrameRecord::cpuPowerW);
    ComputeSensorSummary(m_records, m_results.gpuPower, kBenchSensorGpuPower, &BenchmarkFrameRecord::gpuPowerW);
    ComputeSensorSummary(m_records, m_results.gpuFan, kBenchSensorGpuFan, &BenchmarkFrameRecord::gpuFanRpm);
    ComputeSensorSummary(m_records, m_results.cpuClock, kBenchSensorCpuClock, &BenchmarkFrameRecord::cpuClockMhz);
    ComputeSensorSummary(m_records, m_results.gpuClock, kBenchSensorGpuClock, &BenchmarkFrameRecord::gpuClockMhz);
    ComputeSensorSummary(m_records, m_results.gpuMemClock, kBenchSensorGpuMemClock, &BenchmarkFrameRecord::gpuMemClockMhz);
    ComputeSensorSummary(m_records, m_results.gpuVoltage, kBenchSensorGpuVoltage, &BenchmarkFrameRecord::gpuVoltageV);

    if (!m_records.empty()) {
        m_results.ramTotalGb = m_records.back().ramTotalGb;
        m_results.vramTotalGb = m_records.back().vramTotalGb;
    }

    m_results.records = std::move(m_records);

    // Save HTML report
    m_results.htmlFilePath = SaveBenchmarkHtmlReport(m_results, m_config.outputDir);

    m_state = BenchmarkState::Results;
    HookLogImportant("[Benchmark] Finished: %llu frames in %.2f s. Avg=%.1f FPS, 1%%Low=%.1f FPS. HTML: %s",
                     static_cast<unsigned long long>(m_results.totalFrames), m_results.durationSeconds,
                     m_results.presentationStats.avgFps, m_results.presentationStats.onePercentLowFps,
                     m_results.htmlFilePath.c_str());
}

void BenchmarkManager::ClearResults() {
    m_state = BenchmarkState::Idle;
    m_records.clear();
}

BenchmarkState BenchmarkManager::GetState() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_state;
}

bool BenchmarkManager::IsActiveOrShowingResults() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_state != BenchmarkState::Idle;
}

float BenchmarkManager::GetDelayRemainingSeconds() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_state != BenchmarkState::Delaying)
        return 0.0f;
    const int64_t now = PerfLogger::GetQpcUs();
    const double elapsed = static_cast<double>(now - m_delayStartQpcUs) / 1'000'000.0;
    const double remaining = static_cast<double>(m_config.startDelaySeconds) - elapsed;
    return static_cast<float>(std::max(0.0, remaining));
}

float BenchmarkManager::GetRecordingElapsedSeconds() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_state != BenchmarkState::Recording)
        return 0.0f;
    const int64_t now = PerfLogger::GetQpcUs();
    return static_cast<float>(static_cast<double>(now - m_recordingStartQpcUs) / 1'000'000.0);
}

uint64_t BenchmarkManager::GetRecordingFrameCount() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_records.size();
}

uint32_t BenchmarkManager::GetDurationSeconds() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_config.durationSeconds;
}

const BenchmarkResults& BenchmarkManager::GetResults() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_results;
}

void BenchmarkManager::OnFrame(int64_t currentQpcUs, float presentationFrameTimeMs, float displayFrameTimeMs,
                              const SystemMetrics& sysMetrics, SharedMemoryLayout* sharedMem) {
    std::lock_guard<std::mutex> lock(m_mutex);

    // Synchronize toggle trigger from shared memory
    if (sharedMem) {
        const uint64_t seq = sharedMem->benchmark.toggleSeq.load(std::memory_order_acquire);
        if (seq != m_lastObservedToggleSeq) {
            m_lastObservedToggleSeq = seq;
            // Execute toggle state transition
            switch (m_state) {
                case BenchmarkState::Idle:
                    StartDelayOrRecording();
                    break;
                case BenchmarkState::Delaying:
                    m_state = BenchmarkState::Idle;
                    break;
                case BenchmarkState::Recording:
                    StopRecordingAndPublish();
                    break;
                case BenchmarkState::Results:
                    ClearResults();
                    break;
            }
        }

        // Update config from shared memory
        m_config.startDelaySeconds = sharedMem->benchmark.startDelaySeconds.load(std::memory_order_relaxed);
        m_config.durationSeconds = sharedMem->benchmark.durationSeconds.load(std::memory_order_relaxed);
        if (sharedMem->benchmark.outputDir[0] != '\0') {
            m_config.outputDir = sharedMem->benchmark.outputDir;
        }
        if (sharedMem->benchmark.profileName[0] != '\0') {
            m_profileName = sharedMem->benchmark.profileName;
        }
    }

    if (m_state == BenchmarkState::Delaying) {
        const int64_t elapsedUs = currentQpcUs - m_delayStartQpcUs;
        if (elapsedUs >= static_cast<int64_t>(m_config.startDelaySeconds) * 1'000'000) {
            StartRecording();
        }
        return;
    }

    if (m_state != BenchmarkState::Recording) {
        return;
    }

    // Benchmark frame recording
    BenchmarkFrameRecord record = {};
    record.timeSeconds = static_cast<float>(static_cast<double>(currentQpcUs - m_recordingStartQpcUs) / 1'000'000.0);
    record.presentationFrameTimeMs = presentationFrameTimeMs > 0.0f ? presentationFrameTimeMs : 0.001f;
    record.displayFrameTimeMs = displayFrameTimeMs > 0.0f ? displayFrameTimeMs : record.presentationFrameTimeMs;
    record.presentationFps = 1000.0f / record.presentationFrameTimeMs;
    record.displayFps = 1000.0f / record.displayFrameTimeMs;

    // Telemetry sensors
    record.cpuUsage = sysMetrics.cpuUsage;
    record.cpuMaxCoreUsage = sysMetrics.cpuMaxCoreUsage;
    record.validMask |= kBenchSensorCpuUsage | kBenchSensorCpuMaxCore;

    const float bytesToGb = 1.0f / (1024.0f * 1024.0f * 1024.0f);
    record.ramUsedGb = static_cast<float>(sysMetrics.ramUsed) * bytesToGb;
    record.ramTotalGb = static_cast<float>(sysMetrics.ramTotal) * bytesToGb;
    record.validMask |= kBenchSensorRam;

    record.vramUsedGb = static_cast<float>(sysMetrics.vramUsed) * bytesToGb;
    record.vramTotalGb = static_cast<float>(sysMetrics.vramTotal) * bytesToGb;
    if (sysMetrics.vramUsageValid) {
        record.validMask |= kBenchSensorVram;
    }

    if (sysMetrics.gpuUsageValid) {
        record.gpuUsage = sysMetrics.gpuUsage;
        record.validMask |= kBenchSensorGpuUsage;
    }

    if (sysMetrics.cpuTemperatureValid && sysMetrics.cpuTemperatureC > 0.0f) {
        record.cpuTempC = sysMetrics.cpuTemperatureC;
        record.validMask |= kBenchSensorCpuTemp;
    }
    if (sysMetrics.gpuTemperatureValid && sysMetrics.gpuTemperatureC > 0.0f) {
        record.gpuTempC = sysMetrics.gpuTemperatureC;
        record.validMask |= kBenchSensorGpuTemp;
    }
    if (sysMetrics.cpuPackagePowerValid && sysMetrics.cpuPackagePowerW > 0.0f) {
        record.cpuPowerW = sysMetrics.cpuPackagePowerW;
        record.validMask |= kBenchSensorCpuPower;
    }
    if (sysMetrics.gpuPackagePowerValid && sysMetrics.gpuPackagePowerW > 0.0f) {
        record.gpuPowerW = sysMetrics.gpuPackagePowerW;
        record.validMask |= kBenchSensorGpuPower;
    }
    if (sysMetrics.gpuFanValid && sysMetrics.gpuFanRpm > 0.0f) {
        record.gpuFanRpm = sysMetrics.gpuFanRpm;
        record.validMask |= kBenchSensorGpuFan;
    }
    if (sysMetrics.cpuCoreClockValid && sysMetrics.cpuCoreClockMhz > 0.0f) {
        record.cpuClockMhz = sysMetrics.cpuCoreClockMhz;
        record.validMask |= kBenchSensorCpuClock;
    }
    if (sysMetrics.gpuCoreClockValid && sysMetrics.gpuCoreClockMhz > 0.0f) {
        record.gpuClockMhz = sysMetrics.gpuCoreClockMhz;
        record.validMask |= kBenchSensorGpuClock;
    }
    if (sysMetrics.gpuMemoryClockValid && sysMetrics.gpuMemoryClockMhz > 0.0f) {
        record.gpuMemClockMhz = sysMetrics.gpuMemoryClockMhz;
        record.validMask |= kBenchSensorGpuMemClock;
    }
    if (sysMetrics.gpuCoreVoltageValid && sysMetrics.gpuCoreVoltageV > 0.0f) {
        record.gpuVoltageV = sysMetrics.gpuCoreVoltageV;
        record.validMask |= kBenchSensorGpuVoltage;
    }

    m_records.push_back(record);

    // Auto duration expiration check
    if (m_config.durationSeconds > 0) {
        const int64_t durationUs = static_cast<int64_t>(m_config.durationSeconds) * 1'000'000;
        if (currentQpcUs - m_recordingStartQpcUs >= durationUs) {
            StopRecordingAndPublish();
        }
    }
}

BenchmarkStats BenchmarkManager::CalculateStats(const std::vector<float>& frameTimesMs) {
    BenchmarkStats stats = {};
    if (frameTimesMs.empty())
        return stats;

    double sumMs = 0.0;
    float minMs = frameTimesMs[0];
    float maxMs = frameTimesMs[0];

    for (float ft : frameTimesMs) {
        sumMs += ft;
        if (ft < minMs)
            minMs = ft;
        if (ft > maxMs)
            maxMs = ft;
    }

    const size_t n = frameTimesMs.size();
    const double nDbl = static_cast<double>(n);
    stats.avgFrameTimeMs = static_cast<float>(sumMs / nDbl);
    stats.avgFps = sumMs > 0.0 ? static_cast<float>((1000.0 * nDbl) / sumMs) : 0.0f;
    stats.maxFps = minMs > 0.001f ? (1000.0f / minMs) : 0.0f;
    stats.minFps = maxMs > 0.001f ? (1000.0f / maxMs) : 0.0f;

    // Percentile calculations (descending sort: worst/longest frame times first)
    std::vector<float> sortedTimes = frameTimesMs;
    std::sort(sortedTimes.begin(), sortedTimes.end(), std::greater<float>());

    // 1% Low FPS
    const size_t count1Pct = std::max<size_t>(1, static_cast<size_t>(std::round(nDbl * 0.01)));
    double sum1Pct = 0.0;
    for (size_t i = 0; i < count1Pct; ++i) {
        sum1Pct += sortedTimes[i];
    }
    const double avg1PctMs = sum1Pct / static_cast<double>(count1Pct);
    stats.onePercentLowFps = avg1PctMs > 0.001 ? static_cast<float>(1000.0 / avg1PctMs) : 0.0f;

    // 0.1% Low FPS
    const size_t count01Pct = std::max<size_t>(1, static_cast<size_t>(std::round(nDbl * 0.001)));
    double sum01Pct = 0.0;
    for (size_t i = 0; i < count01Pct; ++i) {
        sum01Pct += sortedTimes[i];
    }
    const double avg01PctMs = sum01Pct / static_cast<double>(count01Pct);
    stats.zeroPointOnePercentLowFps = avg01PctMs > 0.001 ? static_cast<float>(1000.0 / avg01PctMs) : 0.0f;

    return stats;
}

void BenchmarkManager::ComputeSensorSummary(const std::vector<BenchmarkFrameRecord>& records,
                                           BenchmarkSensorSummary& summary,
                                           uint32_t validFlag,
                                           float BenchmarkFrameRecord::*member) {
    summary = {};
    double sum = 0.0;
    size_t count = 0;
    float maxVal = 0.0f;
    float minVal = 0.0f;
    bool first = true;

    for (const auto& r : records) {
        if (r.validMask & validFlag) {
            const float val = r.*member;
            sum += val;
            count++;
            if (first) {
                maxVal = val;
                minVal = val;
                first = false;
            } else {
                if (val > maxVal)
                    maxVal = val;
                if (val < minVal)
                    minVal = val;
            }
        }
    }

    if (count > 0) {
        summary.valid = true;
        summary.avg = static_cast<float>(sum / static_cast<double>(count));
        summary.max = maxVal;
        summary.min = minVal;
    }
}
