#pragma once

#include <windows.h>

#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>

#include "audio_capture.h"

// Host-side proxy for process-loopback WASAPI capture. The unsafe AudioSes COM
// graph lives in a disposable CaptureEngine worker process; only ordered packet
// records cross this boundary.
class ProcessLoopbackCapture {
public:
    ProcessLoopbackCapture();
    ~ProcessLoopbackCapture();

    ProcessLoopbackCapture(const ProcessLoopbackCapture&) = delete;
    ProcessLoopbackCapture& operator=(const ProcessLoopbackCapture&) = delete;

    bool StartByPID(DWORD processId);
    bool StartByName(const std::string& processName);
    void Stop(bool discardPendingPackets = true);
    bool GetNextPacket(AudioPacket& packet);
    void DiscardPendingPackets();
    size_t PendingPacketCount();

    uint64_t GetQueueOverrunPacketCount() const;
    uint64_t GetQueueOverrunFrameCount() const;
    uint64_t GetWorkerRestartCount() const;
    bool HasIntegrityFailure() const;
    uint32_t GetTransportStatus() const;

    void SetRequestedFormat(int sampleRate, int channels, uint32_t channelMask);
    bool IsCapturing() const;
    bool IsMonitoring() const;
    DWORD GetTargetPID() const;
    const std::string& GetTargetProcessName() const {
        return targetProcessName_;
    }

    static bool IsSupported();

private:
    struct WorkerInstance;

    bool StartWorkerLocked(bool restart);
    void RefreshWorkerLocked();
    void DrainWorkerDiagnosticsLocked(WorkerInstance& worker) const;
    void RetireActiveWorkerLocked(bool unexpectedExit, DWORD exitCode);
    void CleanupDrainedWorkersLocked();
    void AccumulateAndDestroyWorkerLocked(std::unique_ptr<WorkerInstance> worker);
    void HandleIntegrityFailureLocked(WorkerInstance& worker);
    static std::wstring QuoteCommandArgument(const std::wstring& value);
    static std::wstring Utf8ToWide(const std::string& value);

    mutable std::mutex mutex_;
    std::unique_ptr<WorkerInstance> activeWorker_;
    std::deque<std::unique_ptr<WorkerInstance>> retiredWorkers_;
    uint64_t nextWorkerGeneration_ = 1;
    uint64_t nextRestartTick_ = 0;
    uint32_t consecutiveRestartFailures_ = 0;
    uint64_t workerRestartCount_ = 0;
    uint64_t accumulatedOverrunPackets_ = 0;
    uint64_t accumulatedOverrunFrames_ = 0;
    uint64_t accumulatedDiagnosticOverruns_ = 0;
    bool desiredRunning_ = false;
    bool restartDesired_ = false;
    bool integrityFailure_ = false;
    uint32_t fatalTransportStatus_ = 0;
    bool targetByName_ = false;
    DWORD targetProcessId_ = 0;
    std::string targetProcessName_;
    int requestedSampleRate_ = 48000;
    int requestedChannels_ = 2;
    uint32_t requestedChannelMask_ = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
};
