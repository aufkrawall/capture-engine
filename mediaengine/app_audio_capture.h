#pragma once

#include <audioclient.h>
#include <mmdeviceapi.h>
#include <windows.h>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <future>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Include for AudioPacket definition
#include "audio_capture.h"
#include "audio_recovery_policy.h"

/**
 * Per-application audio capture using Windows 10/11 process loopback API.
 *
 * Captures audio from a specific process using ActivateAudioInterfaceAsync
 * with AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK.
 *
 * Features:
 * - Capture by process ID or process name
 * - Auto-detect when target process starts (for name-based capture)
 * - Works with audio that started before capture began
 * - Graceful handling when target process exits
 *
 * Requirements: Windows 10 build 20348+ or Windows 11
 */
class AppAudioCapture {
public:
    AppAudioCapture();
    ~AppAudioCapture();

    // Non-copyable
    AppAudioCapture(const AppAudioCapture&) = delete;
    AppAudioCapture& operator=(const AppAudioCapture&) = delete;

    /**
     * Start capturing audio from a specific process by PID.
     * @param processId Target process ID
     * @return true if capture started successfully
     */
    bool StartByPID(DWORD processId);

    /**
     * Start capturing audio from a process by name.
     * Will monitor for the process and start capture when found.
     * @param processName Process name (e.g., "chrome.exe")
     * @return true if monitoring started (capture may start later)
     */
    bool StartByName(const std::string& processName);

    /**
     * Stop capturing and monitoring.
     */
    void Stop(bool discardPendingPackets = true);

    /**
     * Get next available audio packet.
     * @param packet Output packet (data copied)
     * @return true if packet was available
     */
    bool GetNextPacket(AudioPacket& packet);

    // Drop any queued packets without stopping capture.
    void DiscardPendingPackets();

    size_t PendingPacketCount();

    void SetRequestedFormat(int sampleRate, int channels, uint32_t channelMask);

    /**
     * Check if currently capturing audio.
     */
    bool IsCapturing() const {
        return isCapturing.load();
    }

    /**
     * Check if monitoring for a process (name-based mode).
     */
    bool IsMonitoring() const {
        return isMonitoring.load();
    }

    /**
     * Get the current target process ID (0 if not capturing).
     */
    DWORD GetTargetPID() const {
        return targetPID.load();
    }

    /**
     * Get the target process name (empty if PID-based).
     */
    const std::string& GetTargetProcessName() const {
        return targetProcessName;
    }

    /**
     * Check if per-process audio loopback is supported on this Windows version.
     * @return true if Windows 10 build 20348+ or Windows 11
     */
    static bool IsSupported();

private:
    // Allow a couple of seconds of capture jitter before we have to drop the
    // oldest queued audio packet.
    static constexpr size_t kMaxQueuedPackets = 256;

    // Internal activation handler (implements
    // IActivateAudioInterfaceCompletionHandler)
    class ActivationHandler;

    // Initialize capture for the given PID (called internally)
    bool InitializeCaptureForPID(DWORD pid);
    // Activate + initialize + start a process-loopback client for pid, leaving
    // pAudioClient/pCaptureClient/pwfx ready. Does NOT launch the capture thread,
    // so it is reusable for both first start and mid-stream re-activation.
    bool ActivateClientForPID(DWORD pid);
    // Abandon the current (dead) client and re-activate a fresh one in place,
    // keeping the capture thread, queue, and downstream track alive. Used by the
    // capture loop on a fatal stream error or a silent stall.
    bool ReactivateClientForPID(DWORD pid);
    bool StartCaptureThreadForCurrentClient();
    void BeginAsyncStartForPID(DWORD pid);
    void FinalizePendingAsyncStart();

    // Abandon process-loopback COM interfaces (without releasing them: releasing
    // crashes AudioSes CLoopbackMixer cleanup) plus free pwfx and reset stream
    // fields. Shared by CleanupCapture() and ReactivateClientForPID().
    void AbandonClientInterfaces();

    // Cleanup capture resources
    void CleanupCapture();

    // Audio capture loop (runs in captureThread)
    void CaptureLoop();

    // Process monitoring loop (runs in monitorThread for name-based capture)
    void ProcessMonitorLoop();

    // Find PID by process name
    static DWORD FindProcessByName(const std::string& name);

    // Check if a process is still running
    static bool IsProcessRunning(DWORD pid);

    // Audio client and capture interfaces
    IAudioClient* pAudioClient = nullptr;
    IAudioCaptureClient* pCaptureClient = nullptr;
    WAVEFORMATEX* pwfx = nullptr;
    DWORD activeStreamFlags = 0;
    uint64_t streamLatency100ns = 0;
    uint64_t defaultDevicePeriod100ns = 0;
    uint64_t minDevicePeriod100ns = 0;
    uint32_t bufferFrameCount = 0;
    int requestedSampleRate = 48000;
    int requestedChannels = 2;
    uint32_t requestedChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;

    // Mid-recording stream re-activation policy (device-invalidation + silent stall).
    ce::audio::StreamRecoveryConfig recoveryConfig_;

    // Target process info
    std::atomic<DWORD> targetPID{0};
    std::string targetProcessName;

    // State flags
    std::atomic<bool> isCapturing{false};
    std::atomic<bool> isMonitoring{false};
    std::atomic<bool> shouldStop{false};

    // Threads
    std::thread captureThread;
    std::thread monitorThread;

    // Packet queue
    std::mutex queueMutex;
    std::deque<AudioPacket> packetQueue;

    std::mutex startMutex;
    std::future<bool> pendingStartFuture;
    std::atomic<bool> asyncStartInProgress{false};
    std::atomic<bool> startPendingResult{false};
    std::atomic<bool> startPendingValid{false};

    // Event for async activation completion
    HANDLE activationCompleteEvent = nullptr;
    HANDLE captureEvent_ = nullptr;
    HRESULT activationResult = 0x80004005;  // E_FAIL
};
