#include "audio_capture.h"
#include <algorithm>
#include <iostream>
// PKEY_Device_FriendlyName defined locally to avoid cross-compile link errors
// (MinGW on Linux needs INITGUID for the header definition, this is portable)
static const PROPERTYKEY PKEY_Device_FriendlyName = {
    {0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};
#include "audio_time_utils.h"
#include "mediaengine.h"  // For DLL_Log

#define REFTIMES_PER_SEC 10000000
#define REFTIMES_PER_MILLISEC 10000

const CLSID CLSID_MMDeviceEnumerator = __uuidof(MMDeviceEnumerator);
const IID IID_IMMDeviceEnumerator = __uuidof(IMMDeviceEnumerator);
const IID IID_IAudioClient = __uuidof(IAudioClient);
const IID IID_IAudioCaptureClient = __uuidof(IAudioCaptureClient);

// IEEE Float subformat GUID: {00000003-0000-0010-8000-00aa00389b71}
static bool IsIEEEFloat(const GUID& g) {
    return g.Data1 == 0x00000003 && g.Data2 == 0x0000 && g.Data3 == 0x0010 && g.Data4[0] == 0x80 &&
           g.Data4[1] == 0x00 && g.Data4[2] == 0x00 && g.Data4[3] == 0xaa && g.Data4[4] == 0x00 && g.Data4[5] == 0x38 &&
           g.Data4[6] == 0x9b && g.Data4[7] == 0x71;
}

static uint32_t ExtractChannelMask(const WAVEFORMATEX* format) {
    if (!format) {
        return 0;
    }
    if (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
        format->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
        const auto* wfex = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(format);
        return static_cast<uint32_t>(wfex->dwChannelMask);
    }
    if (format->nChannels == 1) {
        return SPEAKER_FRONT_CENTER;
    }
    if (format->nChannels == 2) {
        return SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
    }
    return 0;
}

static void FillPacketFormatFromWaveFormat(const WAVEFORMATEX* format, AudioPacket* packet) {
    if (!format || !packet) {
        return;
    }

    packet->channels = format->nChannels;
    packet->sampleRate = format->nSamplesPerSec;
    packet->bitsPerSample = format->wBitsPerSample;
    packet->blockAlign = format->nBlockAlign;
    packet->validBitsPerSample = 0;
    packet->channelMask = ExtractChannelMask(format);
    packet->isFloat = format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
    packet->devicePosition = 0;
    packet->qpcPosition = 0;
    packet->rawQpcPosition = 0;
    packet->streamLatency = 0;

    if (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        const auto* wfex = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(format);
        if (IsIEEEFloat(wfex->SubFormat)) {
            packet->isFloat = true;
        }
        packet->validBitsPerSample = wfex->Samples.wValidBitsPerSample;
    }
}

AudioCapture::AudioCapture()
    : pEnumerator(NULL),
      pDevice(NULL),
      pAudioClient(NULL),
      pCaptureClient(NULL),
      pwfx(NULL),
      isCapturing(false) {}

AudioCapture::~AudioCapture() {
    Stop();

    if (captureEvent_) {
        CloseHandle(captureEvent_);
        captureEvent_ = nullptr;
    }
}

bool AudioCapture::ProbeMixFormat(const std::string& deviceId, bool isLoopback, AudioPacket* format) {
    if (!format) {
        return false;
    }

    *format = {};
    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    const bool coInitOwned = (hr == S_OK);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        DLL_Log("[AudioCapture] ProbeMixFormat CoInitializeEx failed: 0x%x", hr);
        return false;
    }

    IMMDeviceEnumerator* enumerator = nullptr;
    IMMDevice* device = nullptr;
    IAudioClient* audioClient = nullptr;
    WAVEFORMATEX* waveFormat = nullptr;
    const EDataFlow dataFlow = isLoopback ? eRender : eCapture;

    auto cleanup = [&]() {
        if (waveFormat) {
            CoTaskMemFree(waveFormat);
        }
        if (audioClient) {
            audioClient->Release();
        }
        if (device) {
            device->Release();
        }
        if (enumerator) {
            enumerator->Release();
        }
        if (coInitOwned) {
            CoUninitialize();
        }
    };

    hr = CoCreateInstance(CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL, IID_IMMDeviceEnumerator,
                          reinterpret_cast<void**>(&enumerator));
    if (FAILED(hr) || !enumerator) {
        DLL_Log("[AudioCapture] ProbeMixFormat CoCreateInstance failed: 0x%x", hr);
        cleanup();
        return false;
    }

    if (deviceId.empty()) {
        hr = enumerator->GetDefaultAudioEndpoint(dataFlow, eConsole, &device);
    } else {
        int wideLen = MultiByteToWideChar(CP_UTF8, 0, deviceId.c_str(), -1, nullptr, 0);
        if (wideLen > 0) {
            std::wstring wideId(static_cast<size_t>(wideLen), L'\0');
            MultiByteToWideChar(CP_UTF8, 0, deviceId.c_str(), -1, wideId.data(), wideLen);
            hr = enumerator->GetDevice(wideId.c_str(), &device);
        } else {
            hr = E_FAIL;
        }

        if (FAILED(hr)) {
            IMMDeviceCollection* devices = nullptr;
            HRESULT enumHr = enumerator->EnumAudioEndpoints(dataFlow, DEVICE_STATE_ACTIVE, &devices);
            if (SUCCEEDED(enumHr) && devices) {
                UINT count = 0;
                devices->GetCount(&count);
                for (UINT i = 0; i < count && !device; ++i) {
                    IMMDevice* candidate = nullptr;
                    if (FAILED(devices->Item(i, &candidate)) || !candidate) {
                        continue;
                    }
                    IPropertyStore* props = nullptr;
                    if (SUCCEEDED(candidate->OpenPropertyStore(STGM_READ, &props)) && props) {
                        PROPVARIANT var = {};
                        if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &var)) && var.vt == VT_LPWSTR &&
                            var.pwszVal) {
                            int nameLen =
                                WideCharToMultiByte(CP_UTF8, 0, var.pwszVal, -1, nullptr, 0, nullptr, nullptr);
                            if (nameLen > 0) {
                                std::string friendlyName(static_cast<size_t>(nameLen), '\0');
                                WideCharToMultiByte(CP_UTF8, 0, var.pwszVal, -1, friendlyName.data(), nameLen, nullptr,
                                                    nullptr);
                                if (!friendlyName.empty() && friendlyName.back() == '\0') {
                                    friendlyName.pop_back();
                                }
                                if (friendlyName == deviceId) {
                                    device = candidate;
                                    device->AddRef();
                                    hr = S_OK;
                                }
                            }
                        }
                        if (var.vt == VT_LPWSTR && var.pwszVal) {
                            CoTaskMemFree(var.pwszVal);
                        }
                        props->Release();
                    }
                    candidate->Release();
                }
                devices->Release();
            }
        }

        if (!device) {
            hr = enumerator->GetDefaultAudioEndpoint(dataFlow, eConsole, &device);
        }
    }

    if (FAILED(hr) || !device) {
        DLL_Log("[AudioCapture] ProbeMixFormat endpoint lookup failed: 0x%x", hr);
        cleanup();
        return false;
    }

    hr = device->Activate(IID_IAudioClient, CLSCTX_ALL, NULL, reinterpret_cast<void**>(&audioClient));
    if (FAILED(hr) || !audioClient) {
        DLL_Log("[AudioCapture] ProbeMixFormat Activate failed: 0x%x", hr);
        cleanup();
        return false;
    }

    hr = audioClient->GetMixFormat(&waveFormat);
    if (FAILED(hr) || !waveFormat) {
        DLL_Log("[AudioCapture] ProbeMixFormat GetMixFormat failed: 0x%x", hr);
        cleanup();
        return false;
    }

    FillPacketFormatFromWaveFormat(waveFormat, format);
    DLL_Log("[AudioCapture] ProbeMixFormat: device=%s loopback=%d channels=%d rate=%d bits=%d mask=0x%x",
            deviceId.empty() ? "default" : deviceId.c_str(), isLoopback ? 1 : 0, format->channels, format->sampleRate,
            format->bitsPerSample, format->channelMask);
    cleanup();
    return true;
}

bool AudioCapture::Start(const std::string& deviceId, bool isLoopback) {
    DLL_Log("[AudioCapture] Start called: deviceId=%s loopback=%d", deviceId.empty() ? "default" : deviceId.c_str(),
            isLoopback);
    isLoopback_ = isLoopback;
    deviceId_ = deviceId;  // Remembered so CaptureLoop can re-resolve after device invalidation.
    streamLatency100ns_ = 0;
    HRESULT hr;

    // Clear any stale packets from previous session
    {
        std::lock_guard<std::mutex> lock(queueMutex);
        packetQueue.clear();
    }

    hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        // RPC_E_CHANGED_MODE means COM already initialized with a different model;
        // we can proceed as long as it's initialized. Any other failure is fatal.
        DLL_Log("[AudioCapture] CoInitializeEx failed: 0x%x", hr);
        return false;
    }
    coInitOwned =
        (hr == S_OK);  // Only own COM if WE initialized it (S_OK); S_FALSE = already initialized by another caller

    hr = CoCreateInstance(CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL, IID_IMMDeviceEnumerator, (void**)&pEnumerator);
    if (FAILED(hr))
        return false;

    if (!ResolveCaptureDevice()) {
        return false;
    }
    if (!ActivateAndStartClientOnDevice()) {
        return false;
    }

    isCapturing = true;
    captureThread = std::thread(&AudioCapture::CaptureLoop, this);

    return true;
}

bool AudioCapture::ResolveCaptureDevice() {
    HRESULT hr;
    const EDataFlow dataFlow = isLoopback_ ? eRender : eCapture;

    if (deviceId_.empty()) {
        hr = pEnumerator->GetDefaultAudioEndpoint(dataFlow, eConsole, &pDevice);
        DLL_Log("[AudioCapture] Using default %s endpoint", isLoopback_ ? "render (loopback)" : "capture (microphone)");
    } else {
        // 1. Try exact device ID match (opaque WASAPI device ID string)
        int wideLen = MultiByteToWideChar(CP_UTF8, 0, deviceId_.c_str(), -1, nullptr, 0);
        if (wideLen > 0) {
            std::wstring wideId(static_cast<size_t>(wideLen), L'\0');
            MultiByteToWideChar(CP_UTF8, 0, deviceId_.c_str(), -1, wideId.data(), wideLen);
            hr = pEnumerator->GetDevice(wideId.c_str(), &pDevice);
            if (SUCCEEDED(hr)) {
                DLL_Log("[AudioCapture] Using device by ID: %s", deviceId_.c_str());
            }
        } else {
            hr = E_FAIL;
        }

        // 2. If GetDevice failed, try matching by friendly name
        if (FAILED(hr)) {
            IMMDeviceCollection* pDevices = nullptr;
            HRESULT enumHr = pEnumerator->EnumAudioEndpoints(dataFlow, DEVICE_STATE_ACTIVE, &pDevices);
            if (SUCCEEDED(enumHr)) {
                UINT count = 0;
                pDevices->GetCount(&count);
                for (UINT i = 0; i < count && !pDevice; i++) {
                    IMMDevice* pCandidate = nullptr;
                    if (SUCCEEDED(pDevices->Item(i, &pCandidate)) && pCandidate) {
                        IPropertyStore* pProps = nullptr;
                        if (SUCCEEDED(pCandidate->OpenPropertyStore(STGM_READ, &pProps)) && pProps) {
                            PROPVARIANT var = {};
                            if (SUCCEEDED(pProps->GetValue(PKEY_Device_FriendlyName, &var)) && var.vt == VT_LPWSTR &&
                                var.pwszVal) {
                                int nameLen =
                                    WideCharToMultiByte(CP_UTF8, 0, var.pwszVal, -1, nullptr, 0, nullptr, nullptr);
                                if (nameLen > 0) {
                                    std::string friendlyName(static_cast<size_t>(nameLen), L'\0');
                                    WideCharToMultiByte(CP_UTF8, 0, var.pwszVal, -1, friendlyName.data(), nameLen,
                                                        nullptr, nullptr);
                                    if (!friendlyName.empty() && friendlyName.back() == '\0')
                                        friendlyName.pop_back();
                                    if (friendlyName == deviceId_) {
                                        pDevice = pCandidate;
                                        pDevice->AddRef();
                                        hr = S_OK;
                                        DLL_Log("[AudioCapture] Using device by friendly name: %s", deviceId_.c_str());
                                    }
                                }
                            }
                            if (var.vt == VT_LPWSTR && var.pwszVal)
                                CoTaskMemFree(var.pwszVal);
                            pProps->Release();
                        }
                        pCandidate->Release();
                    }
                }
                pDevices->Release();
            }
        }

        // 3. If both failed, fall back to default endpoint
        if (!pDevice) {
            DLL_Log("[AudioCapture] Device '%s' not found by ID or name, falling back to default", deviceId_.c_str());
            hr = pEnumerator->GetDefaultAudioEndpoint(dataFlow, eConsole, &pDevice);
        }
    }
    if (FAILED(hr)) {
        DLL_Log("[AudioCapture] GetDefaultAudioEndpoint failed: 0x%x", hr);
        return false;
    }
    return true;
}

bool AudioCapture::ActivateAndStartClientOnDevice() {
    HRESULT hr;
    const bool isLoopback = isLoopback_;

    if (!captureEvent_) {
        captureEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!captureEvent_) {
            DLL_Log("[AudioCapture] CreateEventW failed: 0x%lx, falling back to polling mode", GetLastError());
        }
    }

    auto activateAndInitializeClient = [&](DWORD streamFlags) -> HRESULT {
        if (pCaptureClient) {
            pCaptureClient->Release();
            pCaptureClient = NULL;
        }
        if (pAudioClient) {
            pAudioClient->Release();
            pAudioClient = NULL;
        }
        if (pwfx) {
            CoTaskMemFree(pwfx);
            pwfx = NULL;
        }

        HRESULT initHr = pDevice->Activate(IID_IAudioClient, CLSCTX_ALL, NULL, (void**)&pAudioClient);
        if (FAILED(initHr)) {
            DLL_Log("[AudioCapture] Activate IAudioClient failed: 0x%x", initHr);
            return initHr;
        }

        initHr = pAudioClient->GetMixFormat(&pwfx);
        if (FAILED(initHr)) {
            DLL_Log("[AudioCapture] GetMixFormat failed: 0x%x", initHr);
            return initHr;
        }

        return pAudioClient->Initialize(AUDCLNT_SHAREMODE_SHARED, streamFlags, 10000000, 0, pwfx, NULL);
    };

    // LOOPBACK flag only applies to render devices (system audio capture)
    // For capture devices (microphone), we don't use LOOPBACK.
    const DWORD baseFlags = isLoopback ? AUDCLNT_STREAMFLAGS_LOOPBACK : 0;
    DWORD flags = baseFlags;
    if (captureEvent_) {
        flags |= AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
    }

    hr = activateAndInitializeClient(flags);  // 1 sec buffer
    if (FAILED(hr)) {
        DLL_Log("[AudioCapture] Initialize with event callback failed: 0x%x (flags=0x%x), retrying polling mode", hr,
                flags);
        flags = baseFlags;
        hr = activateAndInitializeClient(flags);
        if (FAILED(hr)) {
            DLL_Log("[AudioCapture] Initialize failed: 0x%x (flags=0x%x)", hr, flags);
            return false;
        }
    }
    activeStreamFlags = flags;

    REFERENCE_TIME streamLatency = 0;
    hr = pAudioClient->GetStreamLatency(&streamLatency);
    if (SUCCEEDED(hr)) {
        streamLatency100ns_ = static_cast<uint64_t>(std::max<REFERENCE_TIME>(0, streamLatency));
    } else {
        DLL_Log("[AudioCapture] GetStreamLatency failed: 0x%x", hr);
        streamLatency100ns_ = 0;
    }

    // Capture the engine period and allocated buffer so A/V offset triage can see the
    // real endpoint depth even when GetStreamLatency() reports 0 (common for loopback).
    REFERENCE_TIME defaultPeriod = 0;
    REFERENCE_TIME minPeriod = 0;
    if (SUCCEEDED(pAudioClient->GetDevicePeriod(&defaultPeriod, &minPeriod))) {
        defaultDevicePeriod100ns_ = static_cast<uint64_t>(std::max<REFERENCE_TIME>(0, defaultPeriod));
        minDevicePeriod100ns_ = static_cast<uint64_t>(std::max<REFERENCE_TIME>(0, minPeriod));
    } else {
        defaultDevicePeriod100ns_ = 0;
        minDevicePeriod100ns_ = 0;
    }
    UINT32 bufferFrames = 0;
    bufferFrameCount_ = SUCCEEDED(pAudioClient->GetBufferSize(&bufferFrames)) ? bufferFrames : 0;

    if ((activeStreamFlags & AUDCLNT_STREAMFLAGS_EVENTCALLBACK) != 0 && captureEvent_) {
        ResetEvent(captureEvent_);
        hr = pAudioClient->SetEventHandle(captureEvent_);
        if (FAILED(hr)) {
            DLL_Log("[AudioCapture] SetEventHandle failed: 0x%x, reverting to polling", hr);
            flags = baseFlags;
            hr = activateAndInitializeClient(flags);
            if (FAILED(hr)) {
                DLL_Log("[AudioCapture] Polling reinitialize failed after SetEventHandle error: 0x%x", hr);
                return false;
            }
            activeStreamFlags = flags;
        }
    }

    hr = pAudioClient->GetService(IID_IAudioCaptureClient, (void**)&pCaptureClient);
    if (FAILED(hr))
        return false;

    hr = pAudioClient->Start();
    if (FAILED(hr)) {
        DLL_Log("[AudioCapture] pAudioClient->Start() failed: 0x%x", hr);
        return false;
    }

    const uint64_t bufferDurationUs =
        (pwfx->nSamplesPerSec > 0)
            ? (static_cast<uint64_t>(bufferFrameCount_) * 1000000ull) / static_cast<uint64_t>(pwfx->nSamplesPerSec)
            : 0;
    DLL_Log(
        "[AudioCapture] Started: channels=%d rate=%d bits=%d streamLatency=%lluus loopback=%d "
        "devicePeriod=%lluus minPeriod=%lluus bufferFrames=%u bufferDur=%lluus "
        "(latency routed via video content delay, not audio advance)",
        pwfx->nChannels, pwfx->nSamplesPerSec, pwfx->wBitsPerSample,
        static_cast<unsigned long long>(streamLatency100ns_ / 10), isLoopback_ ? 1 : 0,
        static_cast<unsigned long long>(defaultDevicePeriod100ns_ / 10),
        static_cast<unsigned long long>(minDevicePeriod100ns_ / 10), bufferFrameCount_,
        static_cast<unsigned long long>(bufferDurationUs));
    DLL_Log("[AudioCapture] Capture mode: %s",
            (activeStreamFlags & AUDCLNT_STREAMFLAGS_EVENTCALLBACK) != 0 ? "event-driven" : "polling");

    return true;
}

bool AudioCapture::ReactivateClient() {
    // Endpoint capture/loopback: releasing the interfaces (without IAudioClient::Stop())
    // is safe — only Stop() trips the AudioSes CLoopbackMixer crash. Re-resolve the
    // endpoint because a device-invalidation usually means the default/target device
    // changed underneath us. The capture thread and packet queue stay alive.
    if (pCaptureClient) {
        pCaptureClient->Release();
        pCaptureClient = NULL;
    }
    if (pAudioClient) {
        pAudioClient->Release();
        pAudioClient = NULL;
    }
    if (pwfx) {
        CoTaskMemFree(pwfx);
        pwfx = NULL;
    }
    if (pDevice) {
        pDevice->Release();
        pDevice = NULL;
    }
    activeStreamFlags = 0;
    streamLatency100ns_ = 0;
    defaultDevicePeriod100ns_ = 0;
    minDevicePeriod100ns_ = 0;
    bufferFrameCount_ = 0;

    if (!pEnumerator) {
        return false;
    }
    if (!ResolveCaptureDevice()) {
        return false;
    }
    return ActivateAndStartClientOnDevice();
}

void AudioCapture::Stop(bool discardPendingPackets) {
    isCapturing = false;
    if (captureEvent_) {
        SetEvent(captureEvent_);
    }
    if (captureThread.joinable())
        captureThread.join();

    if (discardPendingPackets) {
        DiscardPendingPackets();
    }
    activeStreamFlags = 0;
    streamLatency100ns_ = 0;
    defaultDevicePeriod100ns_ = 0;
    minDevicePeriod100ns_ = 0;
    bufferFrameCount_ = 0;
    isLoopback_ = false;
    if (captureEvent_) {
        ResetEvent(captureEvent_);
    }

    // NOTE: Do NOT call pAudioClient->Stop() here. On Windows 11, calling Stop() on
    // a loopback audio stream triggers a bug in AudioSes.dll where CLoopbackMixer::Cleanup()
    // crashes with an access violation (double-free of AudioLimiterAPO COM object).
    // The crash happens on the mixer's own thread and is unhandled, killing the process.
    // Releasing the interfaces directly (without Stop()) is safe: Windows tears down the
    // audio session cleanly when the ref count drops to zero via Release().

    if (pCaptureClient) {
        pCaptureClient->Release();
        pCaptureClient = NULL;
    }
    if (pAudioClient) {
        pAudioClient->Release();
        pAudioClient = NULL;
    }
    if (pDevice) {
        pDevice->Release();
        pDevice = NULL;
    }
    if (pEnumerator) {
        pEnumerator->Release();
        pEnumerator = NULL;
    }
    if (pwfx) {
        CoTaskMemFree(pwfx);
        pwfx = NULL;
    }
    if (coInitOwned) {
        CoUninitialize();
        coInitOwned = false;
    }
}

void AudioCapture::CaptureLoop() {
    const HRESULT coInitHr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    DLL_Log("[AudioCapture] CaptureLoop started");

    UINT32 packetLength = 0;
    HRESULT hr;
    BYTE* pData;
    UINT32 numFramesAvailable;
    DWORD flags;
    UINT64 devicePosition;

    UINT64 qpcPosition;

    // Debug: Drift tracking variables (non-static for per-session tracking)
    uint64_t firstDevicePos = 0;
    uint64_t firstQpcPos = 0;
    bool firstSet = false;
    int logCounter = 0;
    int errCount = 0;   // Count GetNextPacketSize errors (reset each session)
    int loopCount = 0;  // Count packets seen (reset each session)
    int qpcSanitizeLogCount = 0;  // Throttle out-of-domain QPC warnings (reset each session)

    // Cache QPC frequency once for converting the live performance counter into the
    // same 100-ns domain WASAPI reports its qpcPosition in, so we can validate it.
    LARGE_INTEGER qpcFreqLI;
    QueryPerformanceFrequency(&qpcFreqLI);
    const uint64_t qpcFreq = static_cast<uint64_t>(qpcFreqLI.QuadPart);
    uint64_t queueDropPackets = 0;
    uint64_t queueDropFrames = 0;
    uint64_t lastQueueDropLogTick = 0;
    uint64_t finalDrainPackets = 0;
    uint64_t finalDrainFrames = 0;

    // --- Mid-recording stream recovery (endpoint device invalidation) ---
    const ce::audio::StreamRecoveryConfig recoveryCfg = recoveryConfig_;
    uint64_t lastReactivateTick = 0;
    uint64_t recoveryBackoffMs = 0;
    uint64_t reactivateAttempts = 0;
    uint64_t reactivateSuccesses = 0;
    int fatalErrLogCount = 0;
    int getBufferErrLogCount = 0;
    constexpr int kErrLogCap = 8;
    auto attemptReactivate = [&](const char* reason, long hrCode) -> bool {
        const uint64_t now = GetTickCount64();
        if (!ce::audio::RecoveryBackoffElapsed(now, lastReactivateTick, recoveryBackoffMs)) {
            return false;
        }
        ++reactivateAttempts;
        lastReactivateTick = now;
        recoveryBackoffMs = ce::audio::NextRecoveryBackoffMs(recoveryBackoffMs, recoveryCfg);
        DLL_Log(
            "[AudioCapture] Re-activating %s stream (reason=%s hr=0x%lx attempt=%llu nextBackoffMs=%llu)",
            isLoopback_ ? "loopback" : "capture", reason, static_cast<unsigned long>(hrCode),
            static_cast<unsigned long long>(reactivateAttempts), static_cast<unsigned long long>(recoveryBackoffMs));
        const bool ok = ReactivateClient();
        if (ok) {
            ++reactivateSuccesses;
            firstSet = false;  // drift baseline restarts on the fresh client (devicePosition resets)
            DLL_Log("[AudioCapture] Re-activation succeeded (attempt=%llu mode=%s)",
                    static_cast<unsigned long long>(reactivateAttempts),
                    (activeStreamFlags & AUDCLNT_STREAMFLAGS_EVENTCALLBACK) != 0 ? "event-driven" : "polling");
        } else {
            DLL_Log("[AudioCapture] Re-activation FAILED (attempt=%llu) - will retry with backoff",
                    static_cast<unsigned long long>(reactivateAttempts));
        }
        return ok;
    };

    while (isCapturing) {
        if ((activeStreamFlags & AUDCLNT_STREAMFLAGS_EVENTCALLBACK) != 0 && captureEvent_) {
            const DWORD waitResult = WaitForSingleObject(captureEvent_, 200);
            if (waitResult == WAIT_FAILED) {
                DLL_Log("[AudioCapture] WaitForSingleObject failed: 0x%lx", GetLastError());
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        // A previous re-activation may have failed and left no client; recover it
        // here (with backoff) before any client call so we never deref nullptr.
        if (!pCaptureClient) {
            attemptReactivate("client_null", 0);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }

        hr = pCaptureClient->GetNextPacketSize(&packetLength);
        if (FAILED(hr)) {
            if (ce::audio::IsFatalWasapiStreamError(hr)) {
                if (fatalErrLogCount++ < kErrLogCap) {
                    DLL_Log(
                        "[AudioCapture] FATAL stream error from GetNextPacketSize: 0x%lx (loopback=%d) - "
                        "attempting re-activation",
                        static_cast<unsigned long>(hr), isLoopback_ ? 1 : 0);
                }
                attemptReactivate("GetNextPacketSize_fatal", hr);
                continue;
            }
            if (errCount++ < 5) {
                DLL_Log("[AudioCapture] GetNextPacketSize failed (transient): 0x%lx", static_cast<unsigned long>(hr));
            }
            continue;
        }

        if (loopCount++ < 3 && packetLength > 0) {
            DLL_Log("[AudioCapture] Got packetLength=%u", packetLength);
        }

        const bool drainingAfterStop = !isCapturing.load(std::memory_order_acquire);
        while (packetLength != 0) {
            hr = pCaptureClient->GetBuffer(&pData, &numFramesAvailable, &flags, &devicePosition, &qpcPosition);
            if (FAILED(hr)) {
                if (ce::audio::IsFatalWasapiStreamError(hr)) {
                    if (fatalErrLogCount++ < kErrLogCap) {
                        DLL_Log(
                            "[AudioCapture] FATAL stream error from GetBuffer: 0x%lx (loopback=%d) - attempting "
                            "re-activation",
                            static_cast<unsigned long>(hr), isLoopback_ ? 1 : 0);
                    }
                    attemptReactivate("GetBuffer_fatal", hr);
                } else if (getBufferErrLogCount++ < kErrLogCap) {
                    DLL_Log("[AudioCapture] GetBuffer failed: 0x%lx", static_cast<unsigned long>(hr));
                }
                break;
            }
            recoveryBackoffMs = 0;  // healthy delivery resets backoff for responsive future recovery
            if (drainingAfterStop) {
                ++finalDrainPackets;
                finalDrainFrames += numFramesAvailable;
            }

            // WASAPI driver timestamp sanity guard. The qpcPosition returned by
            // GetBuffer is device/driver-provided and not always trustworthy: some
            // endpoints (observed on a 192 kHz loopback device) report a garbage
            // first-packet position hundreds of days in the future. Used verbatim,
            // that bogus value makes the media-engine timeline math request an
            // unbounded leading-silence allocation (multi-TB std::vector ->
            // std::bad_alloc -> std::terminate). Validate against the live QPC and
            // substitute it for out-of-domain values so the timeline stays in-domain
            // on every device; healthy positions pass through bit-identical. The raw
            // value is still preserved on the packet for diagnostics below.
            const uint64_t rawQpcPosition = qpcPosition;
            {
                LARGE_INTEGER nowQpcLI;
                QueryPerformanceCounter(&nowQpcLI);
                const uint64_t nowQpc100ns =
                    ce::audio::RawQpcToHundredNanoseconds(static_cast<uint64_t>(nowQpcLI.QuadPart), qpcFreq);
                const uint64_t sanitizedQpc = ce::audio::SanitizeCaptureQpcPosition(qpcPosition, nowQpc100ns);
                if (sanitizedQpc != qpcPosition) {
                    if (qpcSanitizeLogCount++ < 8) {
                        DLL_Log(
                            "[AudioCapture] WARNING: out-of-domain WASAPI qpcPosition=%llu substituted with "
                            "nowQpc=%llu (devPos=%llu rate=%u loopback=%d) - driver reported an invalid capture "
                            "timestamp",
                            (unsigned long long)qpcPosition, (unsigned long long)nowQpc100ns,
                            (unsigned long long)devicePosition, pwfx->nSamplesPerSec, isLoopback_ ? 1 : 0);
                    }
                    qpcPosition = sanitizedQpc;
                }
            }

            // Debug: Check drift between Device Position (samples) and QPC time.
            // WASAPI already converts qpcPosition to 100-ns units.
            // devicePosition is cumulative frame count
            // qpcPosition is the timestamp of that position in 100-ns units.

            if (!firstSet && devicePosition > 0) {
                firstDevicePos = devicePosition;
                firstQpcPos = qpcPosition;
                firstSet = true;
                // streamLatency is telemetry only; placement uses the raw QPC (the A/V offset is
                // corrected by delaying video content, not by advancing audio). wouldAdvanceQpc
                // shows what the retired GetStreamLatency audio-advance would have produced.
                const uint64_t wouldAdvanceQpc =
                    ce::audio::ApplyCaptureLatencyCompensation(firstQpcPos, streamLatency100ns_, isLoopback_);
                DLL_Log(
                    "[AudioCapture] Source Sync Start: DevPos=%llu QPC=%llu (placed raw; "
                    "wouldAdvanceQpc=%llu streamLatency=%lluus, not applied)",
                    firstDevicePos, firstQpcPos, wouldAdvanceQpc,
                    static_cast<unsigned long long>(streamLatency100ns_ / 10));
            } else if (firstSet && logCounter++ % 500 == 0) {  // Log every ~5 seconds
                double samplesDuration = (double)(devicePosition - firstDevicePos) / pwfx->nSamplesPerSec;
                double qpcDuration = ce::audio::HundredNanosecondsToSeconds(qpcPosition - firstQpcPos);
                double driftMs = (samplesDuration - qpcDuration) * 1000.0;

                DLL_Log(
                    "[AudioCapture] Source Sync: Duration Samples=%.4fs, "
                    "QPC=%.4fs, Drift=%.2f ms (%.4f%%)",
                    samplesDuration, qpcDuration, driftMs, (driftMs / (qpcDuration * 1000.0) * 100.0));
            }

            // Build packet with format info
            AudioPacket packet;
            packet.channels = pwfx->nChannels;
            packet.sampleRate = pwfx->nSamplesPerSec;
            packet.bitsPerSample = pwfx->wBitsPerSample;
            packet.blockAlign = pwfx->nBlockAlign;
            packet.validBitsPerSample = 0;  // Default: same as bitsPerSample
            packet.channelMask = ExtractChannelMask(pwfx);
            packet.devicePosition = devicePosition;      // Store for debugging if needed
            packet.rawQpcPosition = rawQpcPosition;       // Store unmodified WASAPI timestamp for debugging
            packet.streamLatency = streamLatency100ns_;  // telemetry only (see below)
            // A/V capture latency is corrected by DELAYING video content (and equalizing faster
            // audio sources up to it), never by advancing live audio: the earlier samples do not
            // exist, so advancing only manufactures a live-edge deficit the CFR pipeline pads and
            // the encoded cursor re-pins (the shift is absorbed, not corrected). The render->loopback
            // offset is auto-measured/configured (audio_capture_latency_ms) and routed entirely
            // through the video content delay. So place the packet at the raw WASAPI QPC; do NOT
            // subtract streamLatency here (it double-counts with the video delay on devices that
            // report a nonzero GetStreamLatency, and on HDMI/AVR it is 0 anyway).
            packet.qpcPosition = qpcPosition;

            // Check if float format and extract validBitsPerSample from
            // WAVEFORMATEXTENSIBLE
            packet.isFloat = false;
            if (pwfx->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
                packet.isFloat = true;
            } else if (pwfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
                WAVEFORMATEXTENSIBLE* wfex = (WAVEFORMATEXTENSIBLE*)pwfx;
                if (IsIEEEFloat(wfex->SubFormat)) {
                    packet.isFloat = true;
                }
                // Extract valid bits - important for 24-bit audio in 32-bit container
                packet.validBitsPerSample = wfex->Samples.wValidBitsPerSample;
            }

            packet.timestamp = (int64_t)ce::audio::HundredNanosecondsToMilliseconds(packet.qpcPosition);

            // Copy data - or generate silence if silent flag is set (critical for A/V
            // sync!)
            size_t bytes = numFramesAvailable * pwfx->nBlockAlign;
            packet.data.resize(bytes);

            if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                // Generate silence - DO NOT SKIP! Dropping silent packets causes
                // timeline compression
                memset(packet.data.data(), 0, bytes);
            } else {
                memcpy(packet.data.data(), pData, bytes);
            }

            {
                std::lock_guard<std::mutex> lock(queueMutex);
                if (packetQueue.size() >= kMaxQueuedPackets) {
                    const AudioPacket& droppedPacket = packetQueue.front();
                    if (droppedPacket.blockAlign > 0) {
                        queueDropFrames += droppedPacket.data.size() / static_cast<size_t>(droppedPacket.blockAlign);
                    }
                    queueDropPackets++;
                    packetQueue.pop_front();

                    const uint64_t nowTick = GetTickCount64();
                    if (nowTick - lastQueueDropLogTick >= 1000) {
                        DLL_Log(
                            "[AudioCapture] WARNING: capture queue overrun - dropped %llu packet(s) / %llu frame(s) "
                            "while keeping newest audio (depth=%zu)",
                            static_cast<unsigned long long>(queueDropPackets),
                            static_cast<unsigned long long>(queueDropFrames), kMaxQueuedPackets);
                        queueDropPackets = 0;
                        queueDropFrames = 0;
                        lastQueueDropLogTick = nowTick;
                    }
                }
                packetQueue.push_back(packet);
            }

            pCaptureClient->ReleaseBuffer(numFramesAvailable);
            hr = pCaptureClient->GetNextPacketSize(&packetLength);
        }
    }

    DLL_Log("[AudioCapture] CaptureLoop exited");
    if (reactivateAttempts > 0) {
        DLL_Log("[AudioCapture] Stream recovery summary (loopback=%d): %llu re-activation attempt(s), %llu succeeded",
                isLoopback_ ? 1 : 0, static_cast<unsigned long long>(reactivateAttempts),
                static_cast<unsigned long long>(reactivateSuccesses));
    }
    if (finalDrainPackets > 0 || finalDrainFrames > 0) {
        DLL_Log("[AudioCapture] Final stop drain queued %llu packet(s) / %llu frame(s)",
                static_cast<unsigned long long>(finalDrainPackets), static_cast<unsigned long long>(finalDrainFrames));
    }
    if (queueDropPackets > 0 || queueDropFrames > 0) {
        DLL_Log("[AudioCapture] Final queue overrun summary: dropped %llu packet(s) / %llu frame(s)",
                static_cast<unsigned long long>(queueDropPackets), static_cast<unsigned long long>(queueDropFrames));
    }
    // Only uninitialize COM if we successfully initialized it on this thread.
    // S_FALSE means COM was already initialized (we don't own the reference).
    if (SUCCEEDED(coInitHr) && coInitHr != S_FALSE) {
        CoUninitialize();
    }
}

bool AudioCapture::GetNextPacket(AudioPacket& packet) {
    std::lock_guard<std::mutex> lock(queueMutex);
    if (packetQueue.empty())
        return false;
    packet = packetQueue.front();
    packetQueue.pop_front();  // O(1) for deque vs O(n) for vector
    return true;
}

void AudioCapture::DiscardPendingPackets() {
    std::lock_guard<std::mutex> lock(queueMutex);
    if (!packetQueue.empty()) {
        DLL_Log("[AudioCapture] Discarding %zu queued packets", packetQueue.size());
        packetQueue.clear();
    }
}

size_t AudioCapture::PendingPacketCount() {
    std::lock_guard<std::mutex> lock(queueMutex);
    return packetQueue.size();
}
