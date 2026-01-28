#pragma once

#include <Windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <atomic>
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
    bool isFloat;
    uint64_t devicePosition;  // For debug drift analysis
    uint64_t qpcPosition;     // For debug drift analysis
};

class AudioCapture {
public:
    AudioCapture();
    ~AudioCapture();

    // Start capturing from a specific device (empty ID = Default)
    // isLoopback: true for System Audio, false for Mic
    bool Start(const std::string& deviceId, bool isLoopback);

    void Stop();

    // Read available packets
    bool GetNextPacket(AudioPacket& packet);

private:
    IMMDeviceEnumerator* pEnumerator;
    IMMDevice* pDevice;
    IAudioClient* pAudioClient;
    IAudioCaptureClient* pCaptureClient;
    WAVEFORMATEX* pwfx;

    std::atomic<bool> isCapturing;
    std::thread captureThread;

    std::mutex queueMutex;
    std::deque<AudioPacket> packetQueue;  // Use deque for O(1) front removal

    void CaptureLoop();
};
