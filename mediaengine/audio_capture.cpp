#include "audio_capture.h"
#include <algorithm>
#include <exception>
#include <iostream>
#include <utility>
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

    if (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
        format->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
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
    const bool coInitNeedsUninitialize = SUCCEEDED(hr);
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
        if (coInitNeedsUninitialize) {
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

    // Start is restart-safe: never overwrite a live/joinable worker. COM state is
    // worker-owned and a joined worker has already released every interface.
    if (isCapturing.load(std::memory_order_acquire) || captureThread.joinable()) {
        DLL_Log("[AudioCapture] Cleaning up previous capture state before restart");
        Stop(true);
    }

    isLoopback_ = isLoopback;
    deviceId_ = deviceId;  // Remembered so CaptureLoop can re-resolve after device invalidation.
    streamLatency100ns_ = 0;
    // Clear any stale packets from previous session
    {
        std::lock_guard<std::mutex> lock(queueMutex);
        packetQueue.clear();
    }

    {
        std::lock_guard<std::mutex> lock(startupMutex_);
        startupComplete_ = false;
        startupSucceeded_ = false;
    }

    // Publish the run request before launching the worker. CaptureLoop owns COM,
    // endpoint resolution, GetService, Start, reactivation and final Release.
    isCapturing.store(true, std::memory_order_release);
    try {
        captureThread = std::thread(&AudioCapture::CaptureLoop, this);
    } catch (const std::exception& error) {
        DLL_Log("[AudioCapture] Failed to create capture thread: %s", error.what());
        isCapturing.store(false, std::memory_order_release);
        CompleteStartup(false);
        return false;
    }

    bool startupSucceeded = false;
    {
        std::unique_lock<std::mutex> lock(startupMutex_);
        startupCv_.wait(lock, [this]() { return startupComplete_; });
        startupSucceeded = startupSucceeded_;
    }

    if (!startupSucceeded) {
        DLL_Log("[AudioCapture] Worker initialization failed; joining cleanly");
        Stop(true);
        return false;
    }

    return true;
}

void AudioCapture::CompleteStartup(bool succeeded) {
    {
        std::lock_guard<std::mutex> lock(startupMutex_);
        startupSucceeded_ = succeeded;
        startupComplete_ = true;
    }
    startupCv_.notify_all();
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
            if (SUCCEEDED(enumHr) && pDevices) {
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
    if (FAILED(hr) || !pDevice) {
        DLL_Log("[AudioCapture] GetDefaultAudioEndpoint failed: 0x%x", hr);
        return false;
    }
    return true;
}

void AudioCapture::ReleaseActiveClientOnWorkerThread(bool releaseDevice) {
    // Do not call IAudioClient::Stop here. Releasing the interfaces tears down
    // the session without entering the Windows AudioSes loopback Stop cleanup
    // path that has crashed in field dumps. Most importantly, GetService and
    // this Release execute on the same worker thread for every client lifetime.
    const DWORD currentThreadId = GetCurrentThreadId();
    if (workerThreadId_ == 0 || workerThreadId_ != currentThreadId) {
        DLL_Log(
            "[AudioCapture] ERROR: refusing cross-thread WASAPI release (ownerThread=%lu currentThread=%lu client=%p)",
            static_cast<unsigned long>(workerThreadId_), static_cast<unsigned long>(currentThreadId), pCaptureClient);
        return;
    }

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
    if (releaseDevice && pDevice) {
        pDevice->Release();
        pDevice = NULL;
    }
    activeStreamFlags = 0;
    streamLatency100ns_ = 0;
    defaultDevicePeriod100ns_ = 0;
    minDevicePeriod100ns_ = 0;
    bufferFrameCount_ = 0;
}

void AudioCapture::ReleaseAllInterfacesOnWorkerThread() {
    ReleaseActiveClientOnWorkerThread(true);
    if (pEnumerator) {
        pEnumerator->Release();
        pEnumerator = NULL;
    }
}

bool AudioCapture::ActivateAndStartClientOnDevice() {
    HRESULT hr;
    const bool isLoopback = isLoopback_;

    if (!pDevice) {
        DLL_Log("[AudioCapture] Cannot activate a client without a resolved endpoint");
        return false;
    }

    if (!captureEvent_) {
        captureEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!captureEvent_) {
            DLL_Log("[AudioCapture] CreateEventW failed: 0x%lx, falling back to polling mode", GetLastError());
        }
    }

    auto activateAndInitializeClient = [&](DWORD streamFlags) -> HRESULT {
        ReleaseActiveClientOnWorkerThread(false);

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
        if ((flags & AUDCLNT_STREAMFLAGS_EVENTCALLBACK) == 0) {
            DLL_Log("[AudioCapture] Initialize failed: 0x%x (flags=0x%x)", hr, flags);
            ReleaseActiveClientOnWorkerThread(false);
            return false;
        }
        DLL_Log("[AudioCapture] Initialize with event callback failed: 0x%x (flags=0x%x), retrying polling mode", hr,
                flags);
        flags = baseFlags;
        hr = activateAndInitializeClient(flags);
        if (FAILED(hr)) {
            DLL_Log("[AudioCapture] Polling Initialize failed: 0x%x (flags=0x%x)", hr, flags);
            ReleaseActiveClientOnWorkerThread(false);
            return false;
        }
    }
    activeStreamFlags = flags;

    if ((activeStreamFlags & AUDCLNT_STREAMFLAGS_EVENTCALLBACK) != 0 && captureEvent_) {
        ResetEvent(captureEvent_);
        hr = pAudioClient->SetEventHandle(captureEvent_);
        if (FAILED(hr)) {
            DLL_Log("[AudioCapture] SetEventHandle failed: 0x%x, reverting to polling", hr);
            flags = baseFlags;
            hr = activateAndInitializeClient(flags);
            if (FAILED(hr)) {
                DLL_Log("[AudioCapture] Polling reinitialize failed after SetEventHandle error: 0x%x", hr);
                ReleaseActiveClientOnWorkerThread(false);
                return false;
            }
            activeStreamFlags = flags;
        }
    }

    // Query telemetry from the final client. SetEventHandle fallback creates a
    // fresh polling client, so values queried before it would describe a discarded
    // interface and potentially a different buffer size.
    REFERENCE_TIME streamLatency = 0;
    hr = pAudioClient->GetStreamLatency(&streamLatency);
    if (SUCCEEDED(hr)) {
        streamLatency100ns_ = static_cast<uint64_t>(std::max<REFERENCE_TIME>(0, streamLatency));
    } else {
        DLL_Log("[AudioCapture] GetStreamLatency failed: 0x%x", hr);
        streamLatency100ns_ = 0;
    }
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

    hr = pAudioClient->GetService(IID_IAudioCaptureClient, (void**)&pCaptureClient);
    if (FAILED(hr) || !pCaptureClient) {
        DLL_Log("[AudioCapture] GetService(IAudioCaptureClient) failed: 0x%x", hr);
        ReleaseActiveClientOnWorkerThread(false);
        return false;
    }

    hr = pAudioClient->Start();
    if (FAILED(hr)) {
        DLL_Log("[AudioCapture] pAudioClient->Start() failed: 0x%x", hr);
        // GetService succeeded, so release its interface here on this same
        // worker before reporting startup/reactivation failure. A later retry
        // must always start from a null capture client.
        ReleaseActiveClientOnWorkerThread(false);
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
    ReleaseActiveClientOnWorkerThread(true);

    if (!pEnumerator || !isCapturing.load(std::memory_order_acquire)) {
        return false;
    }
    if (!ResolveCaptureDevice()) {
        return false;
    }
    if (!isCapturing.load(std::memory_order_acquire)) {
        DLL_Log("[AudioCapture] Re-activation cancelled after endpoint resolution");
        ReleaseActiveClientOnWorkerThread(true);
        return false;
    }
    if (!ActivateAndStartClientOnDevice()) {
        // ActivateAndStartClientOnDevice already clears partial client state.
        // Leave pCaptureClient null so the capture loop can retry reactivation.
        DLL_Log("[AudioCapture] Re-activation left no live capture client; retry remains armed");
        return false;
    }
    if (!isCapturing.load(std::memory_order_acquire)) {
        DLL_Log("[AudioCapture] Re-activation cancelled after client activation");
        ReleaseActiveClientOnWorkerThread(true);
        return false;
    }
    return true;
}

void AudioCapture::Stop(bool discardPendingPackets) {
    isCapturing.store(false, std::memory_order_release);
    if (captureEvent_) {
        SetEvent(captureEvent_);
    }
    {
        // Wait for an already-started reactivation to finish, or win the gate
        // before the worker can start one. attemptReactivate rechecks
        // isCapturing while holding this same gate, so none can overlap join.
        std::lock_guard<std::mutex> reactivationGate(reactivationMutex_);
    }
    if (captureThread.joinable())
        captureThread.join();

    if (discardPendingPackets) {
        DiscardPendingPackets();
    }
    // The joined worker has released every COM/WASAPI interface. Stop only
    // touches non-COM session metadata on this control thread.
    isLoopback_ = false;
    deviceId_.clear();
    if (captureEvent_) {
        ResetEvent(captureEvent_);
    }

    if (pEnumerator || pDevice || pAudioClient || pCaptureClient || pwfx) {
        // This should be unreachable. Do not violate COM thread affinity by
        // releasing here; make the ownership regression visible in diagnostics.
        DLL_Log("[AudioCapture] ERROR: worker exited with unreleased WASAPI interfaces");
    }
}

void AudioCapture::CaptureLoop() {
    workerThreadId_ = GetCurrentThreadId();
    const HRESULT coInitHr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(coInitHr) && coInitHr != RPC_E_CHANGED_MODE) {
        DLL_Log("[AudioCapture] CaptureLoop CoInitializeEx failed: 0x%x", coInitHr);
        isCapturing.store(false, std::memory_order_release);
        CompleteStartup(false);
        workerThreadId_ = 0;
        return;
    }
    const bool coInitNeedsUninitialize = SUCCEEDED(coInitHr);

    bool startupSucceeded = false;
    try {
        HRESULT hr = CoCreateInstance(CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL, IID_IMMDeviceEnumerator,
                                      reinterpret_cast<void**>(&pEnumerator));
        if (FAILED(hr) || !pEnumerator) {
            DLL_Log("[AudioCapture] Worker CoCreateInstance(MMDeviceEnumerator) failed: 0x%x", hr);
        } else if (!isCapturing.load(std::memory_order_acquire)) {
            DLL_Log("[AudioCapture] Worker initialization cancelled before endpoint resolution");
        } else if (!ResolveCaptureDevice()) {
            DLL_Log("[AudioCapture] Worker could not resolve the requested endpoint");
        } else if (!isCapturing.load(std::memory_order_acquire)) {
            DLL_Log("[AudioCapture] Worker initialization cancelled before client activation");
        } else if (!ActivateAndStartClientOnDevice()) {
            DLL_Log("[AudioCapture] Worker could not activate/start the requested endpoint");
        } else if (!isCapturing.load(std::memory_order_acquire)) {
            DLL_Log("[AudioCapture] Worker initialization cancelled after client activation");
        } else {
            startupSucceeded = true;
        }
    } catch (const std::exception& error) {
        DLL_Log("[AudioCapture] Worker initialization threw an exception: %s", error.what());
    } catch (...) {
        DLL_Log("[AudioCapture] Worker initialization threw an unknown exception");
    }

    if (!startupSucceeded) {
        ReleaseAllInterfacesOnWorkerThread();
        workerThreadId_ = 0;
        isCapturing.store(false, std::memory_order_release);
        CompleteStartup(false);
        if (coInitNeedsUninitialize) {
            CoUninitialize();
        }
        return;
    }

    DLL_Log("[AudioCapture] CaptureLoop started (workerThread=%lu; all WASAPI interfaces worker-owned)",
            static_cast<unsigned long>(GetCurrentThreadId()));
    CompleteStartup(true);

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
    int errCount = 0;             // Count GetNextPacketSize errors (reset each session)
    int loopCount = 0;            // Count packets seen (reset each session)
    int qpcSanitizeLogCount = 0;  // Throttle out-of-domain QPC warnings (reset each session)

    // Cache QPC frequency once for converting the live performance counter into the
    // same 100-ns domain WASAPI reports its qpcPosition in, so we can validate it.
    LARGE_INTEGER qpcFreqLI = {};
    const uint64_t qpcFreq =
        QueryPerformanceFrequency(&qpcFreqLI) && qpcFreqLI.QuadPart > 0 ? static_cast<uint64_t>(qpcFreqLI.QuadPart) : 0;
    uint64_t queueDropPackets = 0;
    uint64_t queueDropFrames = 0;
    uint64_t lastQueueDropLogTick = 0;
    uint64_t finalDrainPackets = 0;
    uint64_t finalDrainFrames = 0;
    uint64_t finalDrainFrameBudget = 0;

    // --- Mid-recording stream recovery (endpoint device invalidation) ---
    const ce::audio::StreamRecoveryConfig recoveryCfg = recoveryConfig_;
    uint64_t lastReactivateTick = 0;
    uint64_t recoveryBackoffMs = 0;
    uint64_t reactivateAttempts = 0;
    uint64_t reactivateSuccesses = 0;
    int fatalErrLogCount = 0;
    int getBufferErrLogCount = 0;
    int emptyBufferLogCount = 0;
    int releaseBufferErrLogCount = 0;
    int invalidPacketLogCount = 0;
    int allocationFailureLogCount = 0;
    int discontinuityLogCount = 0;
    constexpr int kErrLogCap = 8;
    auto attemptReactivate = [&](const char* reason, long hrCode) -> bool {
        std::lock_guard<std::mutex> reactivationGate(reactivationMutex_);
        if (!isCapturing.load(std::memory_order_acquire)) {
            return false;
        }
        const uint64_t now = GetTickCount64();
        if (!ce::audio::RecoveryBackoffElapsed(now, lastReactivateTick, recoveryBackoffMs)) {
            return false;
        }
        ++reactivateAttempts;
        lastReactivateTick = now;
        recoveryBackoffMs = ce::audio::NextRecoveryBackoffMs(recoveryBackoffMs, recoveryCfg);
        DLL_Log("[AudioCapture] Re-activating %s stream (reason=%s hr=0x%lx attempt=%llu nextBackoffMs=%llu)",
                isLoopback_ ? "loopback" : "capture", reason, static_cast<unsigned long>(hrCode),
                static_cast<unsigned long long>(reactivateAttempts),
                static_cast<unsigned long long>(recoveryBackoffMs));
        bool ok = false;
        try {
            ok = ReactivateClient();
        } catch (const std::exception& error) {
            DLL_Log("[AudioCapture] Re-activation threw an exception: %s; partial client state will be cleared",
                    error.what());
            ReleaseActiveClientOnWorkerThread(true);
        } catch (...) {
            DLL_Log("[AudioCapture] Re-activation threw an unknown exception; partial client state will be cleared");
            ReleaseActiveClientOnWorkerThread(true);
        }
        if (ok) {
            ++reactivateSuccesses;
            firstSet = false;  // drift baseline restarts on the fresh client (devicePosition resets)
            DLL_Log("[AudioCapture] Re-activation succeeded (attempt=%llu mode=%s)",
                    static_cast<unsigned long long>(reactivateAttempts),
                    (activeStreamFlags & AUDCLNT_STREAMFLAGS_EVENTCALLBACK) != 0 ? "event-driven" : "polling");
        } else if (isCapturing.load(std::memory_order_acquire)) {
            DLL_Log("[AudioCapture] Re-activation FAILED (attempt=%llu) - will retry with backoff",
                    static_cast<unsigned long long>(reactivateAttempts));
        } else {
            DLL_Log("[AudioCapture] Re-activation cancelled by stop (attempt=%llu)",
                    static_cast<unsigned long long>(reactivateAttempts));
        }
        return ok;
    };

    auto readNextPacketSize = [&](const char* context, bool allowRecovery) -> bool {
        packetLength = 0;
        const HRESULT packetHr = pCaptureClient->GetNextPacketSize(&packetLength);
        if (SUCCEEDED(packetHr)) {
            return true;
        }
        if (ce::audio::IsFatalWasapiStreamError(packetHr)) {
            if (fatalErrLogCount++ < kErrLogCap) {
                DLL_Log("[AudioCapture] FATAL stream error from GetNextPacketSize (%s): 0x%lx (loopback=%d%s)", context,
                        static_cast<unsigned long>(packetHr), isLoopback_ ? 1 : 0,
                        allowRecovery ? "; attempting re-activation" : "; final drain will stop");
            }
            if (allowRecovery) {
                attemptReactivate("GetNextPacketSize_fatal", packetHr);
            } else {
                DLL_Log("[AudioCapture] Final drain stopped by fatal GetNextPacketSize error: 0x%lx",
                        static_cast<unsigned long>(packetHr));
            }
        } else if (errCount++ < kErrLogCap) {
            DLL_Log("[AudioCapture] GetNextPacketSize failed (%s): 0x%lx", context,
                    static_cast<unsigned long>(packetHr));
        }
        return false;
    };

    bool finalDrainPassDone = false;
    while (true) {
        const bool stopRequestedBeforeWait = !isCapturing.load(std::memory_order_acquire);
        if (stopRequestedBeforeWait) {
            if (finalDrainPassDone) {
                break;
            }
            // Exactly one final pass starts without waiting. It drains every
            // packet currently announced by WASAPI and never reactivates a
            // device while Stop() is joining this worker.
            finalDrainPassDone = true;
        } else if ((activeStreamFlags & AUDCLNT_STREAMFLAGS_EVENTCALLBACK) != 0 && captureEvent_) {
            const DWORD waitResult = WaitForSingleObject(captureEvent_, 200);
            if (waitResult == WAIT_FAILED) {
                DLL_Log("[AudioCapture] WaitForSingleObject failed: 0x%lx", GetLastError());
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        } else if (captureEvent_) {
            // In polling mode the same event is still a prompt stop wake-up;
            // it simply is not registered as the WASAPI notification handle.
            WaitForSingleObject(captureEvent_, 10);
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        const bool drainingAfterStop = !isCapturing.load(std::memory_order_acquire);
        if (drainingAfterStop) {
            finalDrainPassDone = true;
            if (finalDrainFrameBudget == 0) {
                const uint32_t fallbackFrames =
                    ce::audio::MaxWasapiCapturePacketFrames(pwfx ? pwfx->nSamplesPerSec : 0);
                finalDrainFrameBudget = bufferFrameCount_ != 0 ? bufferFrameCount_ : fallbackFrames;
                if (finalDrainFrameBudget == 0) {
                    finalDrainFrameBudget = 1;
                }
            }
        }

        // A previous re-activation may have failed and left no client; recover it
        // here (with backoff) before any client call so we never deref nullptr.
        if (!pCaptureClient) {
            if (drainingAfterStop) {
                break;
            }
            attemptReactivate("client_null", 0);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }

        if (!readNextPacketSize(drainingAfterStop ? "final drain" : "outer", !drainingAfterStop)) {
            if (drainingAfterStop) {
                break;
            }
            continue;
        }

        if (loopCount++ < 3 && packetLength > 0) {
            DLL_Log("[AudioCapture] Got packetLength=%u", packetLength);
        }

        while (packetLength != 0) {
            if (drainingAfterStop && finalDrainFrames >= finalDrainFrameBudget) {
                DLL_Log(
                    "[AudioCapture] Final drain reached the endpoint-buffer bound (%llu frame(s)); leaving any "
                    "newly-arrived data for stream teardown",
                    static_cast<unsigned long long>(finalDrainFrameBudget));
                packetLength = 0;
                break;
            }
            hr = pCaptureClient->GetBuffer(&pData, &numFramesAvailable, &flags, &devicePosition, &qpcPosition);
            if (hr == AUDCLNT_S_BUFFER_EMPTY) {
                // This success status means no packet was acquired, so calling
                // ReleaseBuffer would itself be an out-of-order WASAPI error.
                if (emptyBufferLogCount++ < kErrLogCap) {
                    DLL_Log(
                        "[AudioCapture] GetBuffer returned AUDCLNT_S_BUFFER_EMPTY after announcing %u frame(s); "
                        "waiting for the next capture notification",
                        packetLength);
                }
                packetLength = 0;
                break;
            }
            if (FAILED(hr)) {
                if (ce::audio::IsFatalWasapiStreamError(hr)) {
                    if (fatalErrLogCount++ < kErrLogCap) {
                        DLL_Log("[AudioCapture] FATAL stream error from GetBuffer: 0x%lx (loopback=%d%s)",
                                static_cast<unsigned long>(hr), isLoopback_ ? 1 : 0,
                                drainingAfterStop ? "; final drain will stop" : "; attempting re-activation");
                    }
                    if (!drainingAfterStop) {
                        attemptReactivate("GetBuffer_fatal", hr);
                    }
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
                LARGE_INTEGER nowQpcLI = {};
                const uint64_t nowQpc100ns =
                    qpcFreq != 0 && QueryPerformanceCounter(&nowQpcLI) && nowQpcLI.QuadPart >= 0
                        ? ce::audio::RawQpcToHundredNanoseconds(static_cast<uint64_t>(nowQpcLI.QuadPart), qpcFreq)
                        : 0;
                const uint64_t sanitizedQpc = ((flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR) != 0 && nowQpc100ns != 0)
                                                  ? nowQpc100ns
                                                  : ce::audio::SanitizeCaptureQpcPosition(qpcPosition, nowQpc100ns);
                if (sanitizedQpc != qpcPosition) {
                    if (qpcSanitizeLogCount++ < 8) {
                        DLL_Log(
                            "[AudioCapture] WARNING: out-of-domain WASAPI qpcPosition=%llu substituted with "
                            "nowQpc=%llu (devPos=%llu rate=%u loopback=%d) - driver reported an invalid capture "
                            "timestamp%s",
                            (unsigned long long)qpcPosition, (unsigned long long)nowQpc100ns,
                            (unsigned long long)devicePosition, pwfx ? pwfx->nSamplesPerSec : 0, isLoopback_ ? 1 : 0,
                            (flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR) != 0 ? " (TIMESTAMP_ERROR)" : "");
                    }
                    qpcPosition = sanitizedQpc;
                }
            }

            size_t bytes = 0;
            const bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
            const bool frameCountValid = ce::audio::IsWasapiCapturePacketFrameCountValid(
                packetLength, numFramesAvailable, bufferFrameCount_, pwfx ? pwfx->nSamplesPerSec : 0);
            const bool byteSizeValid =
                pwfx && ce::audio::TryComputeAudioPacketByteSize(numFramesAvailable, pwfx->nBlockAlign, &bytes);
            if (!frameCountValid || !byteSizeValid || (!silent && !pData)) {
                if (invalidPacketLogCount++ < kErrLogCap) {
                    DLL_Log(
                        "[AudioCapture] WARNING: rejecting malformed WASAPI packet: announced=%u actual=%u "
                        "bufferFrames=%u sampleRate=%u maxPacketFrames=%u blockAlign=%u data=%p flags=0x%lx "
                        "loopback=%d",
                        packetLength, numFramesAvailable, bufferFrameCount_, pwfx ? pwfx->nSamplesPerSec : 0,
                        ce::audio::MaxWasapiCapturePacketFrames(pwfx ? pwfx->nSamplesPerSec : 0),
                        pwfx ? pwfx->nBlockAlign : 0, pData, flags, isLoopback_ ? 1 : 0);
                }
                const HRESULT releaseHr = pCaptureClient->ReleaseBuffer(numFramesAvailable);
                if (FAILED(releaseHr)) {
                    if (releaseBufferErrLogCount++ < kErrLogCap) {
                        DLL_Log("[AudioCapture] ReleaseBuffer failed after malformed packet: 0x%lx",
                                static_cast<unsigned long>(releaseHr));
                    }
                    if (!drainingAfterStop) {
                        attemptReactivate("ReleaseBuffer_malformed", releaseHr);
                    }
                    packetLength = 0;
                    break;
                }
                if (!readNextPacketSize("after malformed packet", !drainingAfterStop)) {
                    break;
                }
                continue;
            }
            if ((flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) != 0 && discontinuityLogCount < kErrLogCap) {
                ++discontinuityLogCount;
                DLL_Log(
                    "[AudioCapture] WASAPI data discontinuity: frames=%u devPos=%llu qpc=%llu loopback=%d "
                    "(occurrence=%d)",
                    numFramesAvailable, static_cast<unsigned long long>(devicePosition),
                    static_cast<unsigned long long>(qpcPosition), isLoopback_ ? 1 : 0, discontinuityLogCount);
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
            AudioPacket packet{};
            FillPacketFormatFromWaveFormat(pwfx, &packet);
            packet.devicePosition = devicePosition;      // Store for debugging if needed
            packet.rawQpcPosition = rawQpcPosition;      // Store unmodified WASAPI timestamp for debugging
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

            packet.timestamp = (int64_t)ce::audio::HundredNanosecondsToMilliseconds(packet.qpcPosition);

            // Copy data - or generate silence if silent flag is set (critical for A/V
            // sync!)
            try {
                packet.data.resize(bytes);
            } catch (const std::exception& error) {
                if (allocationFailureLogCount++ < kErrLogCap) {
                    DLL_Log(
                        "[AudioCapture] WARNING: packet allocation failed for %zu byte(s) / %u frame(s): %s; "
                        "dropping this packet without terminating capture",
                        bytes, numFramesAvailable, error.what());
                }
                const HRESULT releaseHr = pCaptureClient->ReleaseBuffer(numFramesAvailable);
                if (FAILED(releaseHr)) {
                    if (releaseBufferErrLogCount++ < kErrLogCap) {
                        DLL_Log("[AudioCapture] ReleaseBuffer failed after packet allocation error: 0x%lx",
                                static_cast<unsigned long>(releaseHr));
                    }
                    if (!drainingAfterStop) {
                        attemptReactivate("ReleaseBuffer_allocation_failed", releaseHr);
                    }
                    packetLength = 0;
                    break;
                }
                if (!readNextPacketSize("after packet allocation failure", !drainingAfterStop)) {
                    break;
                }
                continue;
            }

            if (silent) {
                // Generate silence - DO NOT SKIP! Dropping silent packets causes
                // timeline compression
                memset(packet.data.data(), 0, bytes);
            } else {
                memcpy(packet.data.data(), pData, bytes);
            }

            bool logQueueDrop = false;
            uint64_t droppedPacketsToLog = 0;
            uint64_t droppedFramesToLog = 0;
            try {
                std::lock_guard<std::mutex> lock(queueMutex);
                // Append first so allocation failure preserves every packet
                // already queued. Only after the new packet is safely owned do
                // we trim the oldest one to retain the live edge.
                packetQueue.emplace_back(std::move(packet));
                if (packetQueue.size() > kMaxQueuedPackets) {
                    const AudioPacket& droppedPacket = packetQueue.front();
                    if (droppedPacket.blockAlign > 0) {
                        queueDropFrames += droppedPacket.data.size() / static_cast<size_t>(droppedPacket.blockAlign);
                    }
                    queueDropPackets++;
                    packetQueue.pop_front();

                    const uint64_t nowTick = GetTickCount64();
                    if (nowTick - lastQueueDropLogTick >= 1000) {
                        logQueueDrop = true;
                        droppedPacketsToLog = queueDropPackets;
                        droppedFramesToLog = queueDropFrames;
                        queueDropPackets = 0;
                        queueDropFrames = 0;
                        lastQueueDropLogTick = nowTick;
                    }
                }
            } catch (const std::exception& error) {
                if (allocationFailureLogCount++ < kErrLogCap) {
                    DLL_Log("[AudioCapture] WARNING: capture queue insertion failed: %s; dropping packet safely",
                            error.what());
                }
            }
            if (logQueueDrop) {
                DLL_Log(
                    "[AudioCapture] WARNING: capture queue overrun - dropped %llu packet(s) / %llu frame(s) "
                    "while keeping newest audio (depth=%zu)",
                    static_cast<unsigned long long>(droppedPacketsToLog),
                    static_cast<unsigned long long>(droppedFramesToLog), kMaxQueuedPackets);
            }

            const HRESULT releaseHr = pCaptureClient->ReleaseBuffer(numFramesAvailable);
            if (FAILED(releaseHr)) {
                if (releaseBufferErrLogCount++ < kErrLogCap) {
                    DLL_Log("[AudioCapture] ReleaseBuffer failed: 0x%lx%s", static_cast<unsigned long>(releaseHr),
                            drainingAfterStop ? " during final drain" : " - re-activating stream");
                }
                if (!drainingAfterStop) {
                    attemptReactivate("ReleaseBuffer_failed", releaseHr);
                }
                packetLength = 0;
                break;
            }
            if (!readNextPacketSize(drainingAfterStop ? "final inner drain" : "inner drain", !drainingAfterStop)) {
                break;
            }
        }

        if (drainingAfterStop) {
            break;
        }
    }

    DLL_Log("[AudioCapture] CaptureLoop exited; releasing worker-owned WASAPI interfaces on thread %lu",
            static_cast<unsigned long>(GetCurrentThreadId()));
    if (reactivateAttempts > 0) {
        DLL_Log("[AudioCapture] Stream recovery summary (loopback=%d): %llu re-activation attempt(s), %llu succeeded",
                isLoopback_ ? 1 : 0, static_cast<unsigned long long>(reactivateAttempts),
                static_cast<unsigned long long>(reactivateSuccesses));
    }
    if (finalDrainPackets > 0 || finalDrainFrames > 0) {
        DLL_Log("[AudioCapture] Final stop drain inspected %llu packet(s) / %llu frame(s)",
                static_cast<unsigned long long>(finalDrainPackets), static_cast<unsigned long long>(finalDrainFrames));
    }
    if (queueDropPackets > 0 || queueDropFrames > 0) {
        DLL_Log("[AudioCapture] Final queue overrun summary: dropped %llu packet(s) / %llu frame(s)",
                static_cast<unsigned long long>(queueDropPackets), static_cast<unsigned long long>(queueDropFrames));
    }
    ReleaseAllInterfacesOnWorkerThread();
    workerThreadId_ = 0;
    // Every successful CoInitializeEx call, including S_FALSE, must be balanced
    // on this same worker after every COM interface has been released.
    if (coInitNeedsUninitialize) {
        CoUninitialize();
    }
}

bool AudioCapture::GetNextPacket(AudioPacket& packet) {
    AudioPacket queuedPacket;
    {
        std::lock_guard<std::mutex> lock(queueMutex);
        if (packetQueue.empty())
            return false;
        queuedPacket = std::move(packetQueue.front());
        packetQueue.pop_front();  // O(1) for deque vs O(n) for vector
    }
    // Destroy/replace any storage already held by the caller outside the producer
    // queue lock so capture is never delayed by an allocator free.
    packet = std::move(queuedPacket);
    return true;
}

void AudioCapture::DiscardPendingPackets() {
    std::deque<AudioPacket> discarded;
    {
        std::lock_guard<std::mutex> lock(queueMutex);
        discarded.swap(packetQueue);
    }
    if (!discarded.empty()) {
        DLL_Log("[AudioCapture] Discarding %zu queued packets", discarded.size());
    }
}

size_t AudioCapture::PendingPacketCount() {
    std::lock_guard<std::mutex> lock(queueMutex);
    return packetQueue.size();
}
