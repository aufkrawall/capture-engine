#include "audio_capture.h"
#include <iostream>
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

bool AudioCapture::Start(const std::string& deviceId, bool isLoopback) {
    DLL_Log("[AudioCapture] Start called: deviceId=%s loopback=%d", deviceId.empty() ? "default" : deviceId.c_str(),
            isLoopback);
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

    if (deviceId.empty()) {
        // For loopback (system audio), we capture from render device
        // For microphone, we capture from capture device
        EDataFlow dataFlow = isLoopback ? eRender : eCapture;
        hr = pEnumerator->GetDefaultAudioEndpoint(dataFlow, eConsole, &pDevice);
        DLL_Log("[AudioCapture] Using default %s endpoint", isLoopback ? "render (loopback)" : "capture (microphone)");
    } else {
        // Find device by ID or name
        // For now, try to get default based on data flow
        EDataFlow dataFlow = isLoopback ? eRender : eCapture;
        hr = pEnumerator->GetDefaultAudioEndpoint(dataFlow, eConsole, &pDevice);
        DLL_Log("[AudioCapture] Using default %s endpoint (deviceId ignored for now)",
                isLoopback ? "render" : "capture");
    }
    if (FAILED(hr)) {
        DLL_Log("[AudioCapture] GetDefaultAudioEndpoint failed: 0x%x", hr);
        return false;
    }

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

    DLL_Log("[AudioCapture] Started: channels=%d rate=%d bits=%d", pwfx->nChannels, pwfx->nSamplesPerSec,
            pwfx->wBitsPerSample);
    DLL_Log("[AudioCapture] Capture mode: %s",
            (activeStreamFlags & AUDCLNT_STREAMFLAGS_EVENTCALLBACK) != 0 ? "event-driven" : "polling");

    isCapturing = true;
    captureThread = std::thread(&AudioCapture::CaptureLoop, this);

    return true;
}

void AudioCapture::Stop() {
    isCapturing = false;
    if (captureEvent_) {
        SetEvent(captureEvent_);
    }
    if (captureThread.joinable())
        captureThread.join();

    DiscardPendingPackets();
    activeStreamFlags = 0;
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
    uint64_t queueDropPackets = 0;
    uint64_t queueDropFrames = 0;
    uint64_t lastQueueDropLogTick = 0;

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

        hr = pCaptureClient->GetNextPacketSize(&packetLength);
        if (FAILED(hr)) {
            if (errCount++ < 5) {
                DLL_Log("[AudioCapture] GetNextPacketSize failed: 0x%x", hr);
            }
            continue;
        }

        if (loopCount++ < 3 && packetLength > 0) {
            DLL_Log("[AudioCapture] Got packetLength=%u", packetLength);
        }

        while (packetLength != 0) {
            hr = pCaptureClient->GetBuffer(&pData, &numFramesAvailable, &flags, &devicePosition, &qpcPosition);
            if (FAILED(hr))
                break;

            // Debug: Check drift between Device Position (samples) and QPC time.
            // WASAPI already converts qpcPosition to 100-ns units.
            // devicePosition is cumulative frame count
            // qpcPosition is the timestamp of that position in 100-ns units.

            if (!firstSet && devicePosition > 0) {
                firstDevicePos = devicePosition;
                firstQpcPos = qpcPosition;
                firstSet = true;
                DLL_Log("[AudioCapture] Source Sync Start: DevPos=%llu QPC=%llu", firstDevicePos, firstQpcPos);
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
            packet.validBitsPerSample = 0;           // Default: same as bitsPerSample
            packet.devicePosition = devicePosition;  // Store for debugging if needed
            packet.qpcPosition = qpcPosition;        // Store for debugging if needed

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

            packet.timestamp = (int64_t)ce::audio::HundredNanosecondsToMilliseconds(qpcPosition);

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
    if (queueDropPackets > 0 || queueDropFrames > 0) {
        DLL_Log("[AudioCapture] Final queue overrun summary: dropped %llu packet(s) / %llu frame(s)",
                static_cast<unsigned long long>(queueDropPackets),
                static_cast<unsigned long long>(queueDropFrames));
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
