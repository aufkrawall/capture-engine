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
    } else if (hr == E_NOTIMPL) {
        DLL_Log("[AudioCapture] GetStreamLatency is not implemented for this capture client; continuing without "
                "client-reported latency");
        streamLatency100ns_ = 0;
    } else {
        DLL_Log("[AudioCapture] GetStreamLatency unavailable: 0x%x; continuing without client-reported latency", hr);
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

    const uint64_t activatedEpoch = captureEpoch_.fetch_add(1, std::memory_order_acq_rel) + 1;
    {
        std::lock_guard<std::mutex> lock(queueMutex);
        AudioPacket epochStart;
        epochStart.captureEpoch = activatedEpoch;
        epochStart.recordType = AudioPacketRecordType::EpochStart;
        packetQueue.emplace_back(std::move(epochStart));
    }

    const uint64_t bufferDurationUs =
        (pwfx->nSamplesPerSec > 0)
            ? (static_cast<uint64_t>(bufferFrameCount_) * 1000000ull) / static_cast<uint64_t>(pwfx->nSamplesPerSec)
            : 0;
    DLL_Log(
        "[AudioCapture] Started: epoch=%llu channels=%d rate=%d bits=%d streamLatency=%lluus loopback=%d "
        "devicePeriod=%lluus minPeriod=%lluus bufferFrames=%u bufferDur=%lluus "
        "(latency routed via video content delay, not audio advance)",
        static_cast<unsigned long long>(activatedEpoch), pwfx->nChannels, pwfx->nSamplesPerSec, pwfx->wBitsPerSample,
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
