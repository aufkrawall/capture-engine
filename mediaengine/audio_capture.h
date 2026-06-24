#pragma once

#include <audioclient.h>
#include <mmdeviceapi.h>
#include <windows.h>
#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "audio_recovery_policy.h"

struct AudioPacket {
    std::vector<uint8_t> data;
    int64_t timestamp;  // ms
    int channels;
    int sampleRate;
    int bitsPerSample;
    int blockAlign;
    int validBitsPerSample;
    uint32_t channelMask;
    bool isFloat;
    uint64_t devicePosition;  // For debug drift analysis
    uint64_t qpcPosition;     // Latency-compensated WASAPI packet QPC position in 100-ns units
    uint64_t rawQpcPosition;  // Raw WASAPI GetBuffer QPC position in 100-ns units
    uint64_t streamLatency;   // IAudioClient stream latency in 100-ns units used for compensation
};

class AudioCapture {
public:
    AudioCapture();
    ~AudioCapture();

    // Start capturing from a specific device (empty ID = Default)
    // isLoopback: true for System Audio, false for Mic
    bool Start(const std::string& deviceId, bool isLoopback);
    static bool ProbeMixFormat(const std::string& deviceId, bool isLoopback, AudioPacket* format);

    void Stop(bool discardPendingPackets = true);

    // Read available packets
    bool GetNextPacket(AudioPacket& packet);

    // Drop any queued packets without stopping capture.
    void DiscardPendingPackets();

    size_t PendingPacketCount();

private:
    // Allow a couple of seconds of capture jitter before we have to drop the
    // oldest queued audio packet.
    static constexpr size_t kMaxQueuedPackets = 256;

    IMMDeviceEnumerator* pEnumerator;
    IMMDevice* pDevice;
    IAudioClient* pAudioClient;
    IAudioCaptureClient* pCaptureClient;
    WAVEFORMATEX* pwfx;
    HANDLE captureEvent_ = nullptr;
    DWORD activeStreamFlags = 0;
    bool isLoopback_ = false;
    std::string deviceId_;  // Stored so the stream can be re-resolved/re-activated after device invalidation.
    uint64_t streamLatency100ns_ = 0;
    // Endpoint timing reported by WASAPI at Start(), logged for A/V offset diagnosis.
    // GetStreamLatency() frequently returns 0 for loopback, so these expose the real
    // engine period/buffer depth that contributes to uncompensated capture latency.
    uint64_t defaultDevicePeriod100ns_ = 0;
    uint64_t minDevicePeriod100ns_ = 0;
    uint32_t bufferFrameCount_ = 0;

    std::atomic<bool> isCapturing;
    std::thread captureThread;
    bool coInitOwned = false;  // true if Start() successfully called CoInitializeEx

    std::mutex queueMutex;
    std::deque<AudioPacket> packetQueue;  // Use deque for O(1) front removal

    // Mid-recording stream re-activation policy (endpoint device invalidation).
    ce::audio::StreamRecoveryConfig recoveryConfig_;

    void CaptureLoop();

    // Resolve the configured/default endpoint into pDevice (AddRef'd, caller owns).
    // Shared by Start() and ReactivateClient() so both use identical resolution.
    bool ResolveCaptureDevice();
    // Activate + initialize + start the client on the already-resolved pDevice,
    // leaving pAudioClient/pCaptureClient/pwfx ready. Does NOT spawn the thread.
    bool ActivateAndStartClientOnDevice();
    // Release the dead client+device and re-resolve+re-activate in place, keeping
    // the capture thread and queue alive. Used by CaptureLoop on a fatal error.
    bool ReactivateClient();
};
