#pragma once

#include <audioclient.h>
#include <mmdeviceapi.h>
#include <windows.h>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "audio_recovery_policy.h"

enum class AudioPacketRecordType : uint8_t {
    Data,
    EpochStart,
    EndOfStream,
};

struct AudioPacket {
    std::vector<uint8_t> data;
    int64_t timestamp = 0;  // ms
    int channels = 0;
    int sampleRate = 0;
    int bitsPerSample = 0;
    int blockAlign = 0;
    int validBitsPerSample = 0;
    uint32_t channelMask = 0;
    bool isFloat = false;
    uint64_t devicePosition = 0;  // For debug drift analysis
    uint64_t qpcPosition = 0;     // Latency-compensated WASAPI packet QPC position in 100-ns units
    uint64_t rawQpcPosition = 0;  // Raw WASAPI GetBuffer QPC position in 100-ns units
    uint64_t streamLatency = 0;   // IAudioClient stream latency in 100-ns units used for compensation
    uint64_t captureEpoch = 0;    // Successful WASAPI activation generation
    AudioPacketRecordType recordType = AudioPacketRecordType::Data;
    bool endOfStream = false;  // Legacy mirror of recordType==EndOfStream during protocol migration
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

    // Spans in which this source ran without a live endpoint during the current
    // capture: none was available at start, or one was lost and not re-acquired
    // at once. Those spans hold silence, so the recording is reported degraded.
    uint32_t GetDeviceUnavailableEpisodes() const {
        return deviceUnavailableEpisodes_.load(std::memory_order_acquire);
    }

private:
    // Endpoint notifications for the worker's enumerator: a Windows default-device
    // switch (followed when no device is configured) and device arrival (retry a
    // source that is waiting for its endpoint now instead of after its backoff).
    // The callbacks only publish flags; all WASAPI work stays on the worker.
    class EndpointListener final : public IMMNotificationClient {
    public:
        explicit EndpointListener(AudioCapture& owner) : owner_(owner) {}
        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override;
        ULONG STDMETHODCALLTYPE AddRef() override;
        ULONG STDMETHODCALLTYPE Release() override;
        HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR deviceId, DWORD newState) override;
        HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR deviceId) override;
        HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR deviceId) override;
        HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR deviceId) override;
        HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR deviceId, const PROPERTYKEY key) override;

    private:
        AudioCapture& owner_;
        std::atomic<ULONG> refs_{1};
    };

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
    std::atomic<uint64_t> captureEpoch_{0};

    std::atomic<bool> isCapturing;
    std::thread captureThread;
    DWORD workerThreadId_ = 0;

    // Start() remains synchronous even though all WASAPI/COM initialization is
    // worker-owned. CaptureLoop publishes its initialization result through this
    // handshake; no COM interface crosses the worker-thread boundary.
    std::mutex startupMutex_;
    std::condition_variable startupCv_;
    bool startupComplete_ = false;
    bool startupSucceeded_ = false;

    // Serializes endpoint reactivation against Stop's transition into join.
    // Stop closes this gate after clearing isCapturing, ensuring no new COM
    // activation can begin once shutdown has reached the join phase.
    std::mutex reactivationMutex_;

    std::mutex queueMutex;
    std::deque<AudioPacket> packetQueue;  // Use deque for O(1) front removal

    // Mid-recording stream re-activation policy (endpoint device invalidation).
    ce::audio::StreamRecoveryConfig recoveryConfig_;

    EndpointListener endpointListener_{*this};
    bool endpointListenerRegistered_ = false;  // worker-only
    std::atomic<bool> defaultDeviceChanged_{false};
    std::atomic<bool> endpointArrived_{false};
    std::atomic<uint32_t> deviceUnavailableEpisodes_{0};
    void RegisterEndpointListenerOnWorker();
    void UnregisterEndpointListenerOnWorker();
    // True when the default endpoint for this source's flow is a different
    // device than the live client's (or there is no live client). Worker-only.
    bool DefaultEndpointDiffersFromActiveOnWorker();

    void CaptureLoop();
    void CompleteStartup(bool succeeded);

    // These helpers are called only by CaptureLoop. In particular,
    // IAudioCaptureClient must be released on the same thread that obtained it
    // through IAudioClient::GetService.
    void ReleaseActiveClientOnWorkerThread(bool releaseDevice);
    void ReleaseAllInterfacesOnWorkerThread();

    // Resolve the configured/default endpoint into pDevice (AddRef'd, caller owns).
    // Called only by the capture worker, for startup and reactivation.
    bool ResolveCaptureDevice();
    // Activate + initialize + start the client on the already-resolved pDevice,
    // leaving pAudioClient/pCaptureClient/pwfx ready. Worker-thread only.
    bool ActivateAndStartClientOnDevice();
    // Release the dead client+device and re-resolve+re-activate in place, keeping
    // the capture thread and queue alive. Used by CaptureLoop on a fatal error.
    bool ReactivateClient();
};
