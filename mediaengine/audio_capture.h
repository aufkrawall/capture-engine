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

    void Stop();

    // Read available packets
    bool GetNextPacket(AudioPacket& packet);

    // Drop any queued packets without stopping capture.
    void DiscardPendingPackets();

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
    uint64_t streamLatency100ns_ = 0;

    std::atomic<bool> isCapturing;
    std::thread captureThread;
    bool coInitOwned = false;  // true if Start() successfully called CoInitializeEx

    std::mutex queueMutex;
    std::deque<AudioPacket> packetQueue;  // Use deque for O(1) front removal

    void CaptureLoop();
};
