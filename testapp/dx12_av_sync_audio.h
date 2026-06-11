#pragma once

#define WIN32_LEAN_AND_MEAN
#define WINVER 0x0A00
#define _WIN32_WINNT 0x0A00
#include <windows.h>
#include <audioclient.h>
#include <avrt.h>
#include <mmdeviceapi.h>
#include <mmreg.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <thread>

#include "av_sync_stimulus.h"
#include "testapp_common.h"

namespace testapp::avsync {

namespace audio_detail {

constexpr GUID kAudioSubFormatPcm = {
    WAVE_FORMAT_PCM, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
constexpr GUID kAudioSubFormatIeeeFloat = {
    WAVE_FORMAT_IEEE_FLOAT, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

}  // namespace audio_detail

class AudioRenderer {
   public:
    AudioRenderer(const LARGE_INTEGER* qpcFreq, const LARGE_INTEGER* stimulusStartQpc)
        : qpcFreq_(qpcFreq), stimulusStartQpc_(stimulusStartQpc) {}

    bool Start();
    void Stop();
    bool IsReady() const { return ready_.load(std::memory_order_acquire); }
    bool HadError() const { return error_.load(std::memory_order_acquire); }
    void SetAudioClockScheduling(bool enabled) { audioClockSchedulingEnabled_ = enabled; }
    void SetBufferDurationMs(int bufferMs) { requestedBufferMs_ = ClampAudioBufferMs(bufferMs); }
    void SetAudioLeadMs(double leadMs) { audioLeadSeconds_ = ClampAudioLeadMs(leadMs) / 1000.0; }
    int RequestedBufferDurationMs() const { return requestedBufferMs_; }
    double AudioLeadMs() const { return audioLeadSeconds_ * 1000.0; }
    uint64_t StreamLatency100ns() const { return streamLatency100ns_; }
    UINT32 BufferFrames() const { return bufferFrames_; }
    bool HasAudioClock() const { return hasAudioClock_.load(std::memory_order_acquire); }
    bool AudioClockSchedulingEnabled() const { return audioClockSchedulingEnabled_; }
    uint64_t AudioClockFrequency() const { return audioClockFrequency_; }

   private:
    double QpcToSeconds(LONGLONG deltaTicks) const;
    double QpcTicksToSeconds(LONGLONG ticks) const;
    double SecondsSinceStimulusStart() const;
    void ThreadMain();
    bool Initialize();
    void FillAudio(BYTE* data, UINT32 frames, UINT32 queuedFramesBeforeWrite);
    void WriteSample(BYTE* frame, int channel, float value);
    bool IsFloatFormat() const;
    bool IsPcmFormat() const;
    void Cleanup();

    const LARGE_INTEGER* qpcFreq_ = nullptr;
    const LARGE_INTEGER* stimulusStartQpc_ = nullptr;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> ready_{false};
    std::atomic<bool> error_{false};
    Microsoft::WRL::ComPtr<IAudioClient> client_;
    Microsoft::WRL::ComPtr<IAudioRenderClient> renderClient_;
    Microsoft::WRL::ComPtr<IAudioClock> audioClock_;
    WAVEFORMATEX* mixFormat_ = nullptr;
    HANDLE event_ = nullptr;
    UINT32 bufferFrames_ = 0;
    uint64_t streamLatency100ns_ = 0;
    uint64_t audioClockFrequency_ = 0;
    std::atomic<bool> hasAudioClock_{false};
    std::atomic<bool> usedClockScheduling_{false};
    bool audioClockSchedulingEnabled_ = false;
    double audioLeadSeconds_ = kDefaultAudioLeadMs / 1000.0;
    uint64_t sampleCursor_ = 0;
    LARGE_INTEGER audioStartQpc_ = {};
    DWORD lastSummaryTick_ = 0;
    uint64_t underruns_ = 0;
    bool coInitialized_ = false;
    int requestedBufferMs_ = kDefaultAudioBufferMs;
};

inline double AudioRenderer::QpcToSeconds(LONGLONG deltaTicks) const {
    if (!qpcFreq_ || qpcFreq_->QuadPart <= 0) {
        return 0.0;
    }
    return static_cast<double>(deltaTicks) / static_cast<double>(qpcFreq_->QuadPart);
}

inline double AudioRenderer::QpcTicksToSeconds(LONGLONG ticks) const {
    if (!qpcFreq_ || qpcFreq_->QuadPart <= 0) {
        return 0.0;
    }
    return static_cast<double>(ticks) / static_cast<double>(qpcFreq_->QuadPart);
}

inline double AudioRenderer::SecondsSinceStimulusStart() const {
    if (!stimulusStartQpc_) {
        return 0.0;
    }
    LARGE_INTEGER now = {};
    QueryPerformanceCounter(&now);
    return QpcToSeconds(now.QuadPart - stimulusStartQpc_->QuadPart);
}

inline bool AudioRenderer::IsFloatFormat() const {
    if (!mixFormat_) {
        return false;
    }
    if (mixFormat_->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
        return true;
    }
    if (mixFormat_->wFormatTag == WAVE_FORMAT_EXTENSIBLE && mixFormat_->cbSize >= 22) {
        const auto* extensible = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(mixFormat_);
        return IsEqualGUID(extensible->SubFormat, audio_detail::kAudioSubFormatIeeeFloat) != FALSE;
    }
    return false;
}

inline bool AudioRenderer::IsPcmFormat() const {
    if (!mixFormat_) {
        return false;
    }
    if (mixFormat_->wFormatTag == WAVE_FORMAT_PCM) {
        return true;
    }
    if (mixFormat_->wFormatTag == WAVE_FORMAT_EXTENSIBLE && mixFormat_->cbSize >= 22) {
        const auto* extensible = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(mixFormat_);
        return IsEqualGUID(extensible->SubFormat, audio_detail::kAudioSubFormatPcm) != FALSE;
    }
    return false;
}

inline bool AudioRenderer::Start() {
    stop_.store(false, std::memory_order_release);
    thread_ = std::thread(&AudioRenderer::ThreadMain, this);
    return true;
}

inline void AudioRenderer::Stop() {
    stop_.store(true, std::memory_order_release);
    if (event_) {
        SetEvent(event_);
    }
    if (thread_.joinable()) {
        thread_.join();
    }
}

inline bool AudioRenderer::Initialize() {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        testapp::Log("AVSYNC WARNING audio CoInitializeEx failed hr=0x%08lx\n", static_cast<unsigned long>(hr));
        return false;
    }
    coInitialized_ = SUCCEEDED(hr);

    Microsoft::WRL::ComPtr<IMMDeviceEnumerator> enumerator;
    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
    if (FAILED(hr)) {
        testapp::Log("AVSYNC WARNING audio device enumerator failed hr=0x%08lx\n", static_cast<unsigned long>(hr));
        return false;
    }

    Microsoft::WRL::ComPtr<IMMDevice> device;
    hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
    if (FAILED(hr)) {
        testapp::Log("AVSYNC WARNING default render endpoint unavailable hr=0x%08lx\n", static_cast<unsigned long>(hr));
        return false;
    }

    hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(client_.GetAddressOf()));
    if (FAILED(hr)) {
        testapp::Log("AVSYNC WARNING audio client activation failed hr=0x%08lx\n", static_cast<unsigned long>(hr));
        return false;
    }

    hr = client_->GetMixFormat(&mixFormat_);
    if (FAILED(hr) || !mixFormat_) {
        testapp::Log("AVSYNC WARNING GetMixFormat failed hr=0x%08lx\n", static_cast<unsigned long>(hr));
        return false;
    }

    if (!IsFloatFormat() && !IsPcmFormat()) {
        testapp::Log("AVSYNC WARNING unsupported audio mix format tag=0x%x bits=%u\n", mixFormat_->wFormatTag,
                     mixFormat_->wBitsPerSample);
        return false;
    }

    event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event_) {
        testapp::Log("AVSYNC WARNING CreateEventW for audio failed gle=%lu\n", GetLastError());
        return false;
    }

    const REFERENCE_TIME requestedBufferDuration100ns =
        static_cast<REFERENCE_TIME>(ClampAudioBufferMs(requestedBufferMs_)) * 10000;
    hr = client_->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                             requestedBufferDuration100ns, 0, mixFormat_, nullptr);
    if (FAILED(hr)) {
        testapp::Log("AVSYNC WARNING audio Initialize failed hr=0x%08lx\n", static_cast<unsigned long>(hr));
        return false;
    }

    hr = client_->SetEventHandle(event_);
    if (FAILED(hr)) {
        testapp::Log("AVSYNC WARNING SetEventHandle failed hr=0x%08lx\n", static_cast<unsigned long>(hr));
        return false;
    }

    hr = client_->GetBufferSize(&bufferFrames_);
    if (FAILED(hr) || bufferFrames_ == 0) {
        testapp::Log("AVSYNC WARNING GetBufferSize failed hr=0x%08lx frames=%u\n", static_cast<unsigned long>(hr),
                     bufferFrames_);
        return false;
    }

    hr = client_->GetService(IID_PPV_ARGS(&renderClient_));
    if (FAILED(hr)) {
        testapp::Log("AVSYNC WARNING GetService(IAudioRenderClient) failed hr=0x%08lx\n",
                     static_cast<unsigned long>(hr));
        return false;
    }

    hr = client_->GetService(IID_PPV_ARGS(&audioClock_));
    if (SUCCEEDED(hr) && audioClock_) {
        UINT64 clockFrequency = 0;
        if (SUCCEEDED(audioClock_->GetFrequency(&clockFrequency)) && clockFrequency > 0) {
            audioClockFrequency_ = clockFrequency;
            hasAudioClock_.store(true, std::memory_order_release);
        }
    }
    if (!hasAudioClock_.load(std::memory_order_acquire)) {
        audioClock_.Reset();
        testapp::Log("AVSYNC WARNING audio clock unavailable; falling back to sample-cursor scheduling\n");
    }

    REFERENCE_TIME streamLatency = 0;
    hr = client_->GetStreamLatency(&streamLatency);
    if (SUCCEEDED(hr)) {
        streamLatency100ns_ = static_cast<uint64_t>(std::max<REFERENCE_TIME>(0, streamLatency));
    } else {
        streamLatency100ns_ = 0;
        testapp::Log("AVSYNC WARNING audio GetStreamLatency failed hr=0x%08lx\n", static_cast<unsigned long>(hr));
    }

    BYTE* data = nullptr;
    QueryPerformanceCounter(&audioStartQpc_);
    hr = renderClient_->GetBuffer(bufferFrames_, &data);
    if (SUCCEEDED(hr)) {
        FillAudio(data, bufferFrames_, 0);
        renderClient_->ReleaseBuffer(bufferFrames_, 0);
    }

    hr = client_->Start();
    if (FAILED(hr)) {
        testapp::Log("AVSYNC WARNING audio Start failed hr=0x%08lx\n", static_cast<unsigned long>(hr));
        return false;
    }

    const LONGLONG stimulusQpc = stimulusStartQpc_ ? stimulusStartQpc_->QuadPart : 0;
    testapp::Log(
        "AVSYNC START audio channels=%u sampleRate=%u bits=%u blockAlign=%u float=%d pcm=%d bufferFrames=%u "
        "requestedBufferMs=%d audioLeadMs=%.3f renderLatencyUs=%llu audioClock=%d audioClockFrequency=%llu "
        "audioStartQpc=%lld stimulusStartQpc=%lld\n",
        mixFormat_->nChannels, mixFormat_->nSamplesPerSec, mixFormat_->wBitsPerSample, mixFormat_->nBlockAlign,
        IsFloatFormat() ? 1 : 0, IsPcmFormat() ? 1 : 0, bufferFrames_, requestedBufferMs_, AudioLeadMs(),
        static_cast<unsigned long long>(streamLatency100ns_ / 10),
        hasAudioClock_.load(std::memory_order_acquire) ? 1 : 0,
        static_cast<unsigned long long>(audioClockFrequency_),
        static_cast<long long>(audioStartQpc_.QuadPart), static_cast<long long>(stimulusQpc));
    return true;
}

inline void AudioRenderer::ThreadMain() {
    DWORD taskIndex = 0;
    HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
    if (mmcss) {
        AvSetMmThreadPriority(mmcss, AVRT_PRIORITY_HIGH);
    }

    if (!Initialize()) {
        error_.store(true, std::memory_order_release);
        ready_.store(true, std::memory_order_release);
        Cleanup();
        return;
    }

    ready_.store(true, std::memory_order_release);
    while (!stop_.load(std::memory_order_acquire)) {
        DWORD wait = WaitForSingleObject(event_, 250);
        if (wait == WAIT_TIMEOUT) {
            ++underruns_;
            testapp::Log("AVSYNC WARNING audio event timeout count=%llu\n",
                         static_cast<unsigned long long>(underruns_));
        }

        UINT32 padding = 0;
        HRESULT hr = client_->GetCurrentPadding(&padding);
        if (FAILED(hr)) {
            testapp::Log("AVSYNC WARNING GetCurrentPadding failed hr=0x%08lx\n", static_cast<unsigned long>(hr));
            break;
        }
        if (padding > bufferFrames_) {
            padding = bufferFrames_;
        }
        const UINT32 available = bufferFrames_ - padding;
        if (available == 0) {
            continue;
        }

        BYTE* data = nullptr;
        hr = renderClient_->GetBuffer(available, &data);
        if (FAILED(hr)) {
            testapp::Log("AVSYNC WARNING GetBuffer failed hr=0x%08lx available=%u\n", static_cast<unsigned long>(hr),
                         available);
            break;
        }
        FillAudio(data, available, padding);
        renderClient_->ReleaseBuffer(available, 0);

        const DWORD nowTick = GetTickCount();
        if (nowTick - lastSummaryTick_ >= 1000) {
            lastSummaryTick_ = nowTick;
            testapp::Log(
                "AVSYNC AUDIO_BUFFER sampleCursor=%llu padding=%u available=%u underruns=%llu "
                "renderLatencyUs=%llu audioClockScheduling=%d stimulusSeconds=%.6f\n",
                static_cast<unsigned long long>(sampleCursor_), padding, available,
                static_cast<unsigned long long>(underruns_),
                static_cast<unsigned long long>(streamLatency100ns_ / 10),
                usedClockScheduling_.load(std::memory_order_acquire) ? 1 : 0, SecondsSinceStimulusStart());
        }
    }

    if (client_) {
        client_->Stop();
    }
    testapp::Log("AVSYNC SUMMARY audio sampleCursor=%llu underruns=%llu\n",
                 static_cast<unsigned long long>(sampleCursor_), static_cast<unsigned long long>(underruns_));
    Cleanup();
}

inline void AudioRenderer::WriteSample(BYTE* frame, int channel, float value) {
    const int bytesPerSample = mixFormat_->wBitsPerSample / 8;
    BYTE* dst = frame + channel * bytesPerSample;
    value = std::max(-1.0f, std::min(value, 1.0f));
    if (IsFloatFormat() && mixFormat_->wBitsPerSample == 32) {
        *reinterpret_cast<float*>(dst) = value;
        return;
    }

    const int32_t scaled = static_cast<int32_t>(std::lrint(value * 2147483647.0f));
    if (bytesPerSample == 2) {
        const int16_t s16 = static_cast<int16_t>(scaled >> 16);
        memcpy(dst, &s16, sizeof(s16));
    } else if (bytesPerSample == 3) {
        dst[0] = static_cast<BYTE>((scaled >> 8) & 0xff);
        dst[1] = static_cast<BYTE>((scaled >> 16) & 0xff);
        dst[2] = static_cast<BYTE>((scaled >> 24) & 0xff);
    } else if (bytesPerSample == 4) {
        memcpy(dst, &scaled, sizeof(scaled));
    }
}

inline void AudioRenderer::FillAudio(BYTE* data, UINT32 frames, UINT32 queuedFramesBeforeWrite) {
    if (!data || !mixFormat_) {
        return;
    }
    const int channels = std::max<int>(1, mixFormat_->nChannels);
    const int sampleRate = std::max<int>(1, mixFormat_->nSamplesPerSec);
    const int blockAlign = std::max<int>(1, mixFormat_->nBlockAlign);
    const LONGLONG stimulusStart = stimulusStartQpc_ ? stimulusStartQpc_->QuadPart : 0;
    const double renderLatencySeconds = static_cast<double>(streamLatency100ns_) / 10000000.0;
    double baseOffsetSeconds = QpcToSeconds(audioStartQpc_.QuadPart - stimulusStart) + renderLatencySeconds +
                               static_cast<double>(sampleCursor_) / static_cast<double>(sampleRate);
    bool usedClock = false;
    if (audioClockSchedulingEnabled_ && audioClock_) {
        UINT64 devicePosition = 0;
        UINT64 qpcPosition100ns = 0;
        if (SUCCEEDED(audioClock_->GetPosition(&devicePosition, &qpcPosition100ns)) && qpcPosition100ns > 0) {
            const double clockSeconds = static_cast<double>(qpcPosition100ns) / 10000000.0;
            const double stimulusStartSeconds = QpcTicksToSeconds(stimulusStart);
            const double queuedSeconds =
                static_cast<double>(queuedFramesBeforeWrite) / static_cast<double>(sampleRate);
            baseOffsetSeconds = clockSeconds - stimulusStartSeconds + queuedSeconds + renderLatencySeconds;
            usedClock = true;
        }
    }
    usedClockScheduling_.store(usedClock, std::memory_order_release);

    for (UINT32 frame = 0; frame < frames; ++frame) {
        BYTE* frameData = data + frame * blockAlign;
        const double stimulusSeconds = baseOffsetSeconds + audioLeadSeconds_ +
                                       static_cast<double>(frame) / static_cast<double>(sampleRate);
        for (int channel = 0; channel < channels; ++channel) {
            WriteSample(frameData, channel, AudioSampleAt(stimulusSeconds, channel, channels));
        }
    }
    sampleCursor_ += frames;
}

inline void AudioRenderer::Cleanup() {
    if (mixFormat_) {
        CoTaskMemFree(mixFormat_);
        mixFormat_ = nullptr;
    }
    if (event_) {
        CloseHandle(event_);
        event_ = nullptr;
    }
    renderClient_.Reset();
    audioClock_.Reset();
    client_.Reset();
    if (coInitialized_) {
        CoUninitialize();
        coInitialized_ = false;
    }
}

}  // namespace testapp::avsync
