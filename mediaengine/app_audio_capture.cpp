#include "app_audio_capture.h"
#include <combaseapi.h>
#include <propvarutil.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <algorithm>
#include <chrono>
#include <functional>
#include "../common/raii_helpers.h"
#include "audio_capture.h"  // For AudioPacket
#include "audio_time_utils.h"
#include "mediaengine.h"  // For DLL_Log

// Required for ActivateAudioInterfaceAsync
#pragma comment(lib, "mmdevapi.lib")

#define REFTIMES_PER_SEC 10000000
#define REFTIMES_PER_MILLISEC 10000

// ============================================================================
// Per-Process Audio Loopback API Definitions
// These are normally in audioclientactivationparams.h but not available in
// MinGW
// ============================================================================

// Virtual audio device string for process loopback
#ifndef VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK
#define VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK L"VAD\\Process_Loopback"
#endif

// Activation type enum
typedef enum AUDIOCLIENT_ACTIVATION_TYPE {
    AUDIOCLIENT_ACTIVATION_TYPE_DEFAULT = 0,
    AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK = 1
} AUDIOCLIENT_ACTIVATION_TYPE;

// Process loopback mode enum
typedef enum PROCESS_LOOPBACK_MODE {
    PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE = 0,
    PROCESS_LOOPBACK_MODE_EXCLUDE_TARGET_PROCESS_TREE = 1
} PROCESS_LOOPBACK_MODE;

// Process loopback parameters
typedef struct AUDIOCLIENT_PROCESS_LOOPBACK_PARAMS {
    DWORD TargetProcessId;
    PROCESS_LOOPBACK_MODE ProcessLoopbackMode;
} AUDIOCLIENT_PROCESS_LOOPBACK_PARAMS;

// Activation parameters structure
typedef struct AUDIOCLIENT_ACTIVATION_PARAMS {
    AUDIOCLIENT_ACTIVATION_TYPE ActivationType;
    union {
        AUDIOCLIENT_PROCESS_LOOPBACK_PARAMS ProcessLoopbackParams;
    };
} AUDIOCLIENT_ACTIVATION_PARAMS;

// ============================================================================

// IEEE Float subformat GUID
static bool IsIEEEFloat(const GUID& g) {
    return g.Data1 == 0x00000003 && g.Data2 == 0x0000 && g.Data3 == 0x0010 && g.Data4[0] == 0x80 &&
           g.Data4[1] == 0x00 && g.Data4[2] == 0x00 && g.Data4[3] == 0xaa && g.Data4[4] == 0x00 && g.Data4[5] == 0x38 &&
           g.Data4[6] == 0x9b && g.Data4[7] == 0x71;
}

// ============================================================================
// ActivationHandler - Implements IActivateAudioInterfaceCompletionHandler
// Must also implement IAgileObject to avoid E_ILLEGAL_METHOD_CALL
// ============================================================================

// IAgileObject GUID - declared in objidlbase.h via DEFINE_GUID
// Just reference it directly without redeclaring

class AppAudioCapture::ActivationHandler : public IActivateAudioInterfaceCompletionHandler {
public:
    ActivationHandler(HANDLE completeEvent)
        : refCount(1),
          completeEvent(completeEvent),
          resultCode(E_FAIL),
          audioClient(nullptr) {}

    virtual ~ActivationHandler() = default;

    // IUnknown - must return S_OK for IAgileObject to prevent
    // E_ILLEGAL_METHOD_CALL
    STDMETHODIMP QueryInterface(REFIID riid, void** ppvObject) override {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IActivateAudioInterfaceCompletionHandler) ||
            riid == IID_IAgileObject) {
            *ppvObject = static_cast<IActivateAudioInterfaceCompletionHandler*>(this);
            AddRef();
            return S_OK;
        }
        *ppvObject = nullptr;
        return E_NOINTERFACE;
    }

    STDMETHODIMP_(ULONG) AddRef() override {
        return InterlockedIncrement(&refCount);
    }

    STDMETHODIMP_(ULONG) Release() override {
        ULONG count = InterlockedDecrement(&refCount);
        if (count == 0) {
            delete this;
        }
        return count;
    }

    // IActivateAudioInterfaceCompletionHandler
    STDMETHODIMP ActivateCompleted(IActivateAudioInterfaceAsyncOperation* activateOperation) override {
        HRESULT hrActivate = E_FAIL;
        IUnknown* pUnk = nullptr;

        HRESULT hr = activateOperation->GetActivateResult(&hrActivate, &pUnk);
        if (SUCCEEDED(hr) && SUCCEEDED(hrActivate) && pUnk) {
            hr = pUnk->QueryInterface(__uuidof(IAudioClient), reinterpret_cast<void**>(&audioClient));
            pUnk->Release();
        }

        resultCode = SUCCEEDED(hr) ? hrActivate : hr;

        // Signal completion
        SetEvent(completeEvent);
        return S_OK;
    }

    HRESULT GetResult() const {
        return resultCode;
    }
    IAudioClient* GetAudioClient() const {
        return audioClient;
    }

private:
    LONG refCount;
    HANDLE completeEvent;
    HRESULT resultCode;
    IAudioClient* audioClient;
};

// ============================================================================
// AppAudioCapture Implementation
// ============================================================================
AppAudioCapture::AppAudioCapture() {
    activationCompleteEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    captureEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
}

AppAudioCapture::~AppAudioCapture() {
    Stop();
    if (activationCompleteEvent) {
        CloseHandle(activationCompleteEvent);
        activationCompleteEvent = nullptr;
    }
    if (captureEvent_) {
        CloseHandle(captureEvent_);
        captureEvent_ = nullptr;
    }
}

void AppAudioCapture::SetRequestedFormat(int sampleRate, int channels, uint32_t channelMask) {
    requestedSampleRate = sampleRate > 0 ? sampleRate : 48000;
    requestedChannels = std::clamp(channels > 0 ? channels : 2, 1, 8);
    requestedChannelMask = channelMask;
    if (requestedChannelMask == 0) {
        if (requestedChannels == 1) {
            requestedChannelMask = SPEAKER_FRONT_CENTER;
        } else if (requestedChannels == 2) {
            requestedChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
        }
    }
}

bool AppAudioCapture::IsSupported() {
    // Check Windows build version
    // Per-process loopback requires Windows 10 build 20348+
    typedef LONG(WINAPI * RtlGetVersionPtr)(PRTL_OSVERSIONINFOW);
    HMODULE hNtdll = GetModuleHandleW(L"ntdll.dll");
    if (!hNtdll)
        return false;

    RtlGetVersionPtr RtlGetVersion = (RtlGetVersionPtr)GetProcAddress(hNtdll, "RtlGetVersion");
    if (!RtlGetVersion)
        return false;

    RTL_OSVERSIONINFOW osvi = {};
    osvi.dwOSVersionInfoSize = sizeof(osvi);
    if (RtlGetVersion(&osvi) != 0)
        return false;

    // Windows 10 = 10.0, Windows 11 = 10.0 with build >= 22000
    // Per-process loopback added in build 20348
    if (osvi.dwMajorVersion > 10)
        return true;
    if (osvi.dwMajorVersion == 10 && osvi.dwBuildNumber >= 20348)
        return true;

    return false;
}

bool AppAudioCapture::StartByPID(DWORD processId) {
    if (!IsSupported()) {
        DLL_Log(
            "[AppAudioCapture] Per-process audio not supported on this "
            "Windows version");
        return false;
    }

    if (isCapturing.load() || isMonitoring.load()) {
        DLL_Log("[AppAudioCapture] Already running, call Stop() first");
        return false;
    }

    if (!IsProcessRunning(processId)) {
        DLL_Log("[AppAudioCapture] Process %lu not found", processId);
        return false;
    }

    DLL_Log("[AppAudioCapture] Starting capture for PID %lu", processId);
    shouldStop.store(false);
    targetPID.store(processId);
    targetProcessName.clear();

    BeginAsyncStartForPID(processId);
    return true;
}

bool AppAudioCapture::StartByName(const std::string& processName) {
    if (!IsSupported()) {
        DLL_Log(
            "[AppAudioCapture] Per-process audio not supported on this "
            "Windows version");
        return false;
    }

    if (isCapturing.load() || isMonitoring.load()) {
        DLL_Log("[AppAudioCapture] Already running, call Stop() first");
        return false;
    }

    DLL_Log("[AppAudioCapture] Starting monitor for process '%s'", processName.c_str());
    targetProcessName = processName;
    shouldStop.store(false);
    isMonitoring.store(true);

    // Start the process monitor thread
    monitorThread = std::thread(&AppAudioCapture::ProcessMonitorLoop, this);

    return true;
}

void AppAudioCapture::Stop(bool discardPendingPackets) {
    shouldStop.store(true);
    if (captureEvent_) {
        SetEvent(captureEvent_);
    }

    {
        std::lock_guard<std::mutex> lock(startMutex);
        if (pendingStartFuture.valid()) {
            pendingStartFuture.wait();
            pendingStartFuture.get();
        }
        asyncStartInProgress.store(false, std::memory_order_release);
        startPendingValid.store(false, std::memory_order_release);
    }

    // Stop monitoring
    isMonitoring.store(false);
    if (monitorThread.joinable()) {
        monitorThread.join();
    }

    // Stop capturing
    isCapturing.store(false);
    if (captureEvent_) {
        SetEvent(captureEvent_);
    }
    if (captureThread.joinable()) {
        captureThread.join();
    }

    if (discardPendingPackets) {
        DiscardPendingPackets();
    }
    CleanupCapture();
    targetPID.store(0);
    targetProcessName.clear();
}

bool AppAudioCapture::GetNextPacket(AudioPacket& packet) {
    FinalizePendingAsyncStart();
    if (!isCapturing.load(std::memory_order_acquire) && !isMonitoring.load(std::memory_order_acquire) &&
        targetPID.load(std::memory_order_acquire) != 0 && !asyncStartInProgress.load(std::memory_order_acquire)) {
        targetPID.store(0, std::memory_order_release);
    }
    std::lock_guard<std::mutex> lock(queueMutex);
    if (packetQueue.empty())
        return false;
    packet = packetQueue.front();
    packetQueue.pop_front();
    return true;
}

void AppAudioCapture::DiscardPendingPackets() {
    std::lock_guard<std::mutex> lock(queueMutex);
    if (!packetQueue.empty()) {
        DLL_Log("[AppAudioCapture] Discarding %zu queued packets for PID %lu", packetQueue.size(), targetPID.load());
        packetQueue.clear();
    }
}

size_t AppAudioCapture::PendingPacketCount() {
    std::lock_guard<std::mutex> lock(queueMutex);
    return packetQueue.size();
}

void AppAudioCapture::BeginAsyncStartForPID(DWORD pid) {
    std::lock_guard<std::mutex> lock(startMutex);
    if (pendingStartFuture.valid()) {
        pendingStartFuture.wait();
        const bool started = pendingStartFuture.get();
        if (!started) {
            DLL_Log("[AppAudioCapture] Async start failed for PID %lu", targetPID.load());
        }
    }

    asyncStartInProgress.store(true, std::memory_order_release);
    startPendingValid.store(false, std::memory_order_release);
    pendingStartFuture = std::async(std::launch::async, [this, pid]() {
        const bool ok = InitializeCaptureForPID(pid);
        startPendingResult.store(ok, std::memory_order_release);
        startPendingValid.store(true, std::memory_order_release);
        asyncStartInProgress.store(false, std::memory_order_release);
        return ok;
    });
}

void AppAudioCapture::FinalizePendingAsyncStart() {
    if (!startPendingValid.load(std::memory_order_acquire)) {
        return;
    }

    std::lock_guard<std::mutex> lock(startMutex);
    if (pendingStartFuture.valid() &&
        pendingStartFuture.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
        const bool ok = pendingStartFuture.get();
        startPendingValid.store(false, std::memory_order_release);
        if (ok) {
            DLL_Log("[AppAudioCapture] Async start completed for PID %lu", targetPID.load());
        } else {
            DLL_Log("[AppAudioCapture] Async start failed for PID %lu", targetPID.load());
        }
        if (!ok && !isMonitoring.load(std::memory_order_acquire)) {
            targetPID.store(0, std::memory_order_release);
        }
    }
}

bool AppAudioCapture::StartCaptureThreadForCurrentClient() {
    // Clear any stale packets
    {
        std::lock_guard<std::mutex> lock(queueMutex);
        packetQueue.clear();
    }

    if (captureThread.joinable()) {
        captureThread.join();
    }

    isCapturing.store(true, std::memory_order_release);
    captureThread = std::thread(&AppAudioCapture::CaptureLoop, this);
    return true;
}

bool AppAudioCapture::InitializeCaptureForPID(DWORD pid) {
    const HRESULT coInitHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(coInitHr) && coInitHr != RPC_E_CHANGED_MODE) {
        DLL_Log("[AppAudioCapture] CoInitializeEx failed: 0x%x", coInitHr);
        return false;
    }
    const bool coInitOwned = (coInitHr == S_OK);
    CE_SCOPE_EXIT(if (coInitOwned) { CoUninitialize(); });

    HRESULT hr;

    // Reset the completion event
    ResetEvent(activationCompleteEvent);

    // Set up activation parameters for per-process loopback
    AUDIOCLIENT_ACTIVATION_PARAMS audioParams = {};
    audioParams.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
    audioParams.ProcessLoopbackParams.TargetProcessId = pid;
    audioParams.ProcessLoopbackParams.ProcessLoopbackMode = PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;

    PROPVARIANT activateParams = {};
    activateParams.vt = VT_BLOB;
    activateParams.blob.cbSize = sizeof(audioParams);
    activateParams.blob.pBlobData = reinterpret_cast<BYTE*>(&audioParams);

    // Create completion handler
    auto* handler = new ActivationHandler(activationCompleteEvent);

    IActivateAudioInterfaceAsyncOperation* asyncOp = nullptr;
    hr = ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK, __uuidof(IAudioClient), &activateParams,
                                     handler, &asyncOp);

    if (FAILED(hr)) {
        DLL_Log("[AppAudioCapture] ActivateAudioInterfaceAsync failed: 0x%x", hr);
        handler->Release();
        return false;
    }

    // Wait for activation to complete (with timeout)
    DWORD waitResult = WaitForSingleObject(activationCompleteEvent, 5000);
    if (waitResult != WAIT_OBJECT_0) {
        DLL_Log("[AppAudioCapture] Activation timeout");
        handler->Release();
        if (asyncOp)
            asyncOp->Release();
        return false;
    }

    // Get the result
    hr = handler->GetResult();
    if (FAILED(hr)) {
        DLL_Log("[AppAudioCapture] Activation failed: 0x%x", hr);
        handler->Release();
        if (asyncOp)
            asyncOp->Release();
        return false;
    }

    pAudioClient = handler->GetAudioClient();
    handler->Release();
    if (asyncOp)
        asyncOp->Release();

    if (!pAudioClient) {
        DLL_Log("[AppAudioCapture] No audio client obtained");
        return false;
    }

    // Process loopback does not reliably expose the app's native mix format, so
    // request the resolved output layout and let AUTOCONVERTPCM do only the
    // unavoidable source conversion.

    if (pwfx) {
        CoTaskMemFree(pwfx);
        pwfx = nullptr;
    }

    // KSDATAFORMAT_SUBTYPE_IEEE_FLOAT
    const GUID kKsDataFormatSubtypeIeeeFloat = {
        0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

    pwfx = (WAVEFORMATEX*)CoTaskMemAlloc(sizeof(WAVEFORMATEXTENSIBLE));
    if (!pwfx) {
        DLL_Log("[AppAudioCapture] Failed to allocate format");
        CleanupCapture();
        return false;
    }

    WAVEFORMATEXTENSIBLE* wfex = (WAVEFORMATEXTENSIBLE*)pwfx;
    wfex->Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    wfex->Format.nChannels = static_cast<WORD>(std::clamp(requestedChannels, 1, 8));
    wfex->Format.nSamplesPerSec = requestedSampleRate > 0 ? requestedSampleRate : 48000;
    wfex->Format.wBitsPerSample = 32;
    wfex->Format.nBlockAlign = wfex->Format.nChannels * wfex->Format.wBitsPerSample / 8;
    wfex->Format.nAvgBytesPerSec = wfex->Format.nSamplesPerSec * wfex->Format.nBlockAlign;
    wfex->Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    wfex->Samples.wValidBitsPerSample = 32;
    wfex->dwChannelMask = requestedChannelMask;
    wfex->SubFormat = kKsDataFormatSubtypeIeeeFloat;

    // Initialize audio client - per Microsoft sample, use LOOPBACK +
    // AUTOCONVERTPCM AUTOCONVERTPCM tells Windows to convert the process audio to
    // our format Use 10ms buffer (100000 hns) to reduce latency and burstiness
    // CRITICAL FIX: AUTOCONVERTPCM is a FLAG, not a buffer duration parameter
    DWORD streamFlags =
        AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM;
    hr = pAudioClient->Initialize(AUDCLNT_SHAREMODE_SHARED, streamFlags,
                                  100000,  // hnsBufferDuration
                                  0,       // hnsPeriodicity (must be 0 for SHARED)
                                  pwfx, nullptr);
    if (FAILED(hr)) {
        // Try without EVENTCALLBACK
        DLL_Log(
            "[AppAudioCapture] Initialize with EVENTCALLBACK failed: 0x%x, "
            "trying without",
            hr);
        streamFlags = AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM;
        hr = pAudioClient->Initialize(AUDCLNT_SHAREMODE_SHARED, streamFlags, 2000000, 0, pwfx, nullptr);
        if (FAILED(hr)) {
            // Try without any special flags
            DLL_Log(
                "[AppAudioCapture] Initialize with LOOPBACK failed: 0x%x, trying "
                "plain",
                hr);
            streamFlags = 0;
            hr = pAudioClient->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 2000000, 0, pwfx, nullptr);
            if (FAILED(hr)) {
                DLL_Log("[AppAudioCapture] Initialize failed: 0x%x", hr);
                CleanupCapture();
                return false;
            }
        }
    }
    activeStreamFlags = streamFlags;

    REFERENCE_TIME streamLatency = 0;
    hr = pAudioClient->GetStreamLatency(&streamLatency);
    if (SUCCEEDED(hr)) {
        streamLatency100ns = static_cast<uint64_t>(std::max<REFERENCE_TIME>(0, streamLatency));
    } else {
        DLL_Log("[AppAudioCapture] GetStreamLatency failed: 0x%x", hr);
        streamLatency100ns = 0;
    }

    if ((activeStreamFlags & AUDCLNT_STREAMFLAGS_EVENTCALLBACK) != 0 && captureEvent_) {
        ResetEvent(captureEvent_);
        hr = pAudioClient->SetEventHandle(captureEvent_);
        if (FAILED(hr)) {
            DLL_Log("[AppAudioCapture] SetEventHandle failed: 0x%x, reverting to polling", hr);
            activeStreamFlags &= ~AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
        }
    }

    // Get capture client
    hr = pAudioClient->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void**>(&pCaptureClient));
    if (FAILED(hr)) {
        DLL_Log("[AppAudioCapture] GetService IAudioCaptureClient failed: 0x%x", hr);
        CleanupCapture();
        return false;
    }

    // Start the audio client
    hr = pAudioClient->Start();
    if (FAILED(hr)) {
        DLL_Log("[AppAudioCapture] Start failed: 0x%x", hr);
        CleanupCapture();
        return false;
    }

    DLL_Log("[AppAudioCapture] Started: PID=%lu channels=%d rate=%d bits=%d streamLatency=%lluus compensate=1", pid,
            pwfx->nChannels, pwfx->nSamplesPerSec, pwfx->wBitsPerSample,
            static_cast<unsigned long long>(streamLatency100ns / 10));

    DLL_Log("[AppAudioCapture] Capture mode: %s",
            (activeStreamFlags & AUDCLNT_STREAMFLAGS_EVENTCALLBACK) != 0 ? "event-driven" : "polling");

    return StartCaptureThreadForCurrentClient();
}

void AppAudioCapture::CleanupCapture() {
    if (captureEvent_) {
        ResetEvent(captureEvent_);
    }

    // Match AudioCapture teardown: process loopback uses LOOPBACK streams too, and
    // releasing the interfaces directly avoids the crash-sensitive Stop() path.
    if (pAudioClient && (activeStreamFlags & AUDCLNT_STREAMFLAGS_LOOPBACK) == 0) {
        pAudioClient->Stop();
    }

    if (pCaptureClient) {
        pCaptureClient->Release();
        pCaptureClient = nullptr;
    }

    if (pAudioClient) {
        pAudioClient->Release();
        pAudioClient = nullptr;
    }

    if (pwfx) {
        CoTaskMemFree(pwfx);
        pwfx = nullptr;
    }

    activeStreamFlags = 0;
    streamLatency100ns = 0;
}

void AppAudioCapture::CaptureLoop() {
    const HRESULT coInitHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    DLL_Log("[AppAudioCapture] Capture loop started for PID %lu", targetPID.load());

    UINT32 packetLength = 0;
    HRESULT hr;
    BYTE* pData;
    UINT32 numFramesAvailable;
    DWORD flags;
    UINT64 devicePosition;

    UINT64 qpcPosition;

    // Debug: Track packet-timeline drift using devicePosition when available,
    // otherwise fall back to the cumulative captured frame count.
    uint64_t firstLogicalFramePos = 0;
    uint64_t firstQpcPos = 0;
    bool firstSet = false;
    int logCounter = 0;
    uint64_t logicalFrameCursor = 0;
    uint64_t lastProcessCheckTick = 0;
    uint64_t queueDropPackets = 0;
    uint64_t queueDropFrames = 0;
    uint64_t lastQueueDropLogTick = 0;
    uint64_t finalDrainPackets = 0;
    uint64_t finalDrainFrames = 0;

    while (isCapturing.load() && !shouldStop.load()) {
        const uint64_t nowTick = GetTickCount64();
        if (nowTick - lastProcessCheckTick >= 500) {
            lastProcessCheckTick = nowTick;
            if (!IsProcessRunning(targetPID.load())) {
                DLL_Log("[AppAudioCapture] Target process %lu exited", targetPID.load());
                targetPID.store(0, std::memory_order_release);
                break;
            }
        }

        if ((activeStreamFlags & AUDCLNT_STREAMFLAGS_EVENTCALLBACK) != 0 && captureEvent_) {
            const DWORD waitResult = WaitForSingleObject(captureEvent_, 200);
            if (waitResult == WAIT_FAILED) {
                DLL_Log("[AppAudioCapture] WaitForSingleObject failed: 0x%lx", GetLastError());
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        hr = pCaptureClient->GetNextPacketSize(&packetLength);
        if (FAILED(hr)) {
            static int errCount = 0;
            if (errCount++ < 5) {
                DLL_Log("[AppAudioCapture] GetNextPacketSize failed: 0x%x", hr);
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }

        if (packetLength == 0) {
            continue;
        }

        const bool drainingAfterStop =
            !isCapturing.load(std::memory_order_acquire) || shouldStop.load(std::memory_order_acquire);
        while (packetLength != 0) {
            hr = pCaptureClient->GetBuffer(&pData, &numFramesAvailable, &flags, &devicePosition, &qpcPosition);
            if (FAILED(hr))
                break;
            if (drainingAfterStop) {
                ++finalDrainPackets;
                finalDrainFrames += numFramesAvailable;
            }

            const uint64_t logicalFramePos = devicePosition > 0 ? devicePosition : logicalFrameCursor;

            // Debug: Check drift. Process loopback often reports devicePosition=0,
            // so fall back to the cumulative frame count to keep telemetry alive.
            if (!firstSet && qpcPosition > 0) {
                firstLogicalFramePos = logicalFramePos;
                firstQpcPos = qpcPosition;
                firstSet = true;
                const uint64_t packetDuration100ns =
                    ce::audio::AudioFramesToHundredNanoseconds(numFramesAvailable, pwfx->nSamplesPerSec);
                const uint64_t packetStartQpc =
                    ce::audio::ApplyProcessLoopbackPacketTimestampCompensation(firstQpcPos, numFramesAvailable,
                                                                                pwfx->nSamplesPerSec);
                const uint64_t adjustedFirstQpc =
                    ce::audio::ApplyCaptureLatencyCompensation(packetStartQpc, streamLatency100ns, true);
                DLL_Log(
                    "[AppAudioCapture] Source Sync Start (%lu): Frames=%llu QPC=%llu AdjustedQPC=%llu "
                    "Latency=%lluus packetFrames=%u packetDuration=%lluus processLoopbackPacketBias=half_period",
                    targetPID.load(), firstLogicalFramePos, firstQpcPos, adjustedFirstQpc,
                    static_cast<unsigned long long>(streamLatency100ns / 10), numFramesAvailable,
                    static_cast<unsigned long long>(packetDuration100ns / 10));
            } else if (firstSet && qpcPosition > firstQpcPos && logCounter++ % 500 == 0) {  // ~5 seconds
                double samplesDuration = (double)(logicalFramePos - firstLogicalFramePos) / pwfx->nSamplesPerSec;
                double qpcDuration = ce::audio::HundredNanosecondsToSeconds(qpcPosition - firstQpcPos);
                double driftMs = (samplesDuration - qpcDuration) * 1000.0;

                DLL_Log(
                    "[AppAudioCapture] Source Sync (%lu): Duration Samples=%.4fs, "
                    "QPC=%.4fs, Drift=%.2f ms (%.4f%%)",
                    targetPID.load(), samplesDuration, qpcDuration, driftMs,
                    qpcDuration > 0 ? (driftMs / (qpcDuration * 1000.0) * 100.0) : 0.0);
            }

            // Build packet with format info
            AudioPacket packet;
            packet.channels = pwfx->nChannels;
            packet.sampleRate = pwfx->nSamplesPerSec;
            packet.bitsPerSample = pwfx->wBitsPerSample;
            packet.blockAlign = pwfx->nBlockAlign;
            packet.validBitsPerSample = 0;
            packet.channelMask = 0;
            packet.devicePosition = devicePosition;  // Store for debug drift analysis
            packet.rawQpcPosition = qpcPosition;     // Store raw WASAPI timestamp for debug drift analysis
            packet.streamLatency = streamLatency100ns;
            const uint64_t packetStartQpc = ce::audio::ApplyProcessLoopbackPacketTimestampCompensation(
                qpcPosition, numFramesAvailable, pwfx->nSamplesPerSec);
            packet.qpcPosition = ce::audio::ApplyCaptureLatencyCompensation(packetStartQpc, streamLatency100ns, true);

            // Check for float format
            packet.isFloat = false;
            if (pwfx->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
                packet.isFloat = true;
            } else if (pwfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
                WAVEFORMATEXTENSIBLE* wfex = reinterpret_cast<WAVEFORMATEXTENSIBLE*>(pwfx);
                packet.channelMask = static_cast<uint32_t>(wfex->dwChannelMask);
                if (IsIEEEFloat(wfex->SubFormat)) {
                    packet.isFloat = true;
                }
                packet.validBitsPerSample = wfex->Samples.wValidBitsPerSample;
            }
            if (packet.channelMask == 0) {
                if (packet.channels == 1) {
                    packet.channelMask = SPEAKER_FRONT_CENTER;
                } else if (packet.channels == 2) {
                    packet.channelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
                }
            }

            packet.timestamp = (int64_t)ce::audio::HundredNanosecondsToMilliseconds(packet.qpcPosition);

            // Copy or generate silence
            size_t bytes = numFramesAvailable * pwfx->nBlockAlign;
            packet.data.resize(bytes);

            if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
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
                            "[AppAudioCapture] WARNING: capture queue overrun - dropped %llu packet(s) / %llu frame(s) "
                            "for PID %lu while keeping newest audio (depth=%zu)",
                            static_cast<unsigned long long>(queueDropPackets),
                            static_cast<unsigned long long>(queueDropFrames), targetPID.load(), kMaxQueuedPackets);
                        queueDropPackets = 0;
                        queueDropFrames = 0;
                        lastQueueDropLogTick = nowTick;
                    }
                }
                packetQueue.push_back(packet);
            }

            if (devicePosition > 0) {
                logicalFrameCursor = devicePosition + numFramesAvailable;
            } else {
                logicalFrameCursor += numFramesAvailable;
            }

            pCaptureClient->ReleaseBuffer(numFramesAvailable);
            hr = pCaptureClient->GetNextPacketSize(&packetLength);
            if (FAILED(hr))
                break;
        }
    }

    DLL_Log("[AppAudioCapture] Capture loop exited");
    if (finalDrainPackets > 0 || finalDrainFrames > 0) {
        DLL_Log("[AppAudioCapture] Final stop drain queued %llu packet(s) / %llu frame(s) for PID %lu",
                static_cast<unsigned long long>(finalDrainPackets),
                static_cast<unsigned long long>(finalDrainFrames), targetPID.load());
    }
    if (queueDropPackets > 0 || queueDropFrames > 0) {
        DLL_Log("[AppAudioCapture] Final queue overrun summary for PID %lu: dropped %llu packet(s) / %llu frame(s)",
                targetPID.load(), static_cast<unsigned long long>(queueDropPackets),
                static_cast<unsigned long long>(queueDropFrames));
    }
    isCapturing.store(false);
    startPendingValid.store(false, std::memory_order_release);
    if (SUCCEEDED(coInitHr) && coInitHr != S_FALSE) {
        CoUninitialize();
    }
}

void AppAudioCapture::ProcessMonitorLoop() {
    DLL_Log("[AppAudioCapture] Monitor loop started for '%s'", targetProcessName.c_str());

    while (isMonitoring.load() && !shouldStop.load()) {
        FinalizePendingAsyncStart();

        if (asyncStartInProgress.load(std::memory_order_acquire)) {
            for (int i = 0; i < 5 && isMonitoring.load() && !shouldStop.load(); ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            continue;
        }

        // Check if we're already capturing
        if (isCapturing.load()) {
            // Check if target process is still running
            DWORD pid = targetPID.load();
            if (pid != 0 && !IsProcessRunning(pid)) {
                DLL_Log("[AppAudioCapture] Target process %lu exited, stopping capture", pid);
                isCapturing.store(false);
                if (captureThread.joinable()) {
                    captureThread.join();
                }
                CleanupCapture();
                targetPID.store(0);
            }
        } else {
            // Not capturing - try to find the target process
            DWORD pid = FindProcessByName(targetProcessName);
            if (pid != 0) {
                DLL_Log("[AppAudioCapture] Found process '%s' with PID %lu", targetProcessName.c_str(), pid);
                targetPID.store(pid);
                BeginAsyncStartForPID(pid);
            }
        }

        // Check every second, but use small intervals for responsive shutdown
        for (int i = 0; i < 10 && isMonitoring.load() && !shouldStop.load(); i++) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    DLL_Log("[AppAudioCapture] Monitor loop exited");
}

DWORD AppAudioCapture::FindProcessByName(const std::string& name) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return 0;
    }

    PROCESSENTRY32W pe32 = {};
    pe32.dwSize = sizeof(pe32);

    DWORD foundPID = 0;

    if (Process32FirstW(snapshot, &pe32)) {
        do {
            // Convert wide string to narrow for comparison
            char exeName[MAX_PATH];
            WideCharToMultiByte(CP_UTF8, 0, pe32.szExeFile, -1, exeName, MAX_PATH, nullptr, nullptr);

            // Case-insensitive comparison
            if (_stricmp(exeName, name.c_str()) == 0) {
                foundPID = pe32.th32ProcessID;
                break;
            }
        } while (Process32NextW(snapshot, &pe32));
    }

    CloseHandle(snapshot);
    return foundPID;
}

bool AppAudioCapture::IsProcessRunning(DWORD pid) {
    HANDLE hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!hProcess) {
        return false;
    }

    DWORD exitCode = 0;
    BOOL result = GetExitCodeProcess(hProcess, &exitCode);
    CloseHandle(hProcess);

    return result && exitCode == STILL_ACTIVE;
}
