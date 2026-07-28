#include "app_audio_capture.h"
#include <combaseapi.h>
#include <propvarutil.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <exception>
#include <functional>
#include <limits>
#include <new>
#include <utility>
#include "../common/raii_helpers.h"
#include "audio_capture.h"  // For AudioPacket
#include "audio_time_utils.h"
#include "mediaengine.h"  // For DLL_Log
#include "process_tree_selection.h"

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

static std::vector<ce::process_loopback::ProcessTreeEntry> SnapshotProcessTree() {
    std::vector<ce::process_loopback::ProcessTreeEntry> processes;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return processes;
    }

    PROCESSENTRY32W processEntry = {};
    processEntry.dwSize = sizeof(processEntry);
    if (Process32FirstW(snapshot, &processEntry)) {
        do {
            char executableName[MAX_PATH] = {};
            if (WideCharToMultiByte(CP_UTF8, 0, processEntry.szExeFile, -1, executableName, MAX_PATH, nullptr,
                                    nullptr) > 0) {
                processes.push_back({processEntry.th32ProcessID, processEntry.th32ParentProcessID, executableName});
            }
        } while (Process32NextW(snapshot, &processEntry));
    }
    CloseHandle(snapshot);
    return processes;
}

// ============================================================================
// ActivationHandler - Implements IActivateAudioInterfaceCompletionHandler
// Must also implement IAgileObject to avoid E_ILLEGAL_METHOD_CALL
// ============================================================================

// IAgileObject GUID - declared in objidlbase.h via DEFINE_GUID
// Just reference it directly without redeclaring

class AppAudioCapture::ActivationHandler : public IActivateAudioInterfaceCompletionHandler {
public:
    ActivationHandler()
        : refCount(1),
          completeEvent(CreateEventW(nullptr, TRUE, FALSE, nullptr)),
          resultCode(E_FAIL),
          audioClient(nullptr) {}

    virtual ~ActivationHandler() {
        if (completeEvent) {
            CloseHandle(completeEvent);
        }
        // Deliberately do not release audioClient here. Process-loopback client
        // release crosses the same AudioSes teardown crash boundary documented in
        // AbandonClientInterfaces(). A timed-out late activation is rare and the
        // short-lived media process reclaims it at exit.
    }

    // IUnknown - must return S_OK for IAgileObject to prevent
    // E_ILLEGAL_METHOD_CALL
    STDMETHODIMP QueryInterface(REFIID riid, void** ppvObject) override {
        if (!ppvObject) {
            return E_POINTER;
        }
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

        HRESULT hr = activateOperation ? activateOperation->GetActivateResult(&hrActivate, &pUnk) : E_POINTER;
        if (SUCCEEDED(hr) && SUCCEEDED(hrActivate) && pUnk) {
            hr = pUnk->QueryInterface(__uuidof(IAudioClient), reinterpret_cast<void**>(&audioClient));
        }
        if (pUnk) {
            pUnk->Release();
        }

        resultCode = SUCCEEDED(hr) ? hrActivate : hr;

        // Signal completion
        if (completeEvent) {
            SetEvent(completeEvent);
        }
        return S_OK;
    }

    HRESULT GetResult() const {
        return resultCode;
    }
    HANDLE GetEvent() const {
        return completeEvent;
    }
    IAudioClient* TakeAudioClient() {
        IAudioClient* client = audioClient;
        audioClient = nullptr;
        return client;
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
    captureEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!captureEvent_) {
        DLL_Log("[AppAudioCapture] CreateEventW for capture callback failed: 0x%lx; polling will be used",
                GetLastError());
    }
    stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!stopEvent_) {
        DLL_Log("[AppAudioCapture] CreateEventW for activation cancellation failed: 0x%lx", GetLastError());
    }
    packetReadyEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!packetReadyEvent_) {
        DLL_Log("[AppAudioCapture] CreateEventW for packet notification failed: 0x%lx", GetLastError());
    }
}

AppAudioCapture::~AppAudioCapture() {
    Stop();
    if (captureEvent_) {
        CloseHandle(captureEvent_);
        captureEvent_ = nullptr;
    }
    if (stopEvent_) {
        CloseHandle(stopEvent_);
        stopEvent_ = nullptr;
    }
    if (packetReadyEvent_) {
        CloseHandle(packetReadyEvent_);
        packetReadyEvent_ = nullptr;
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

    if (isCapturing.load() || isMonitoring.load() || asyncStartInProgress.load(std::memory_order_acquire)) {
        DLL_Log("[AppAudioCapture] Already running, call Stop() first");
        return false;
    }

    if (captureThread.joinable() || pAudioClient || pCaptureClient || pwfx) {
        DLL_Log("[AppAudioCapture] Cleaning up completed/partial capture state before PID start");
        Stop(true);
    }

    if (!IsProcessRunning(processId)) {
        DLL_Log("[AppAudioCapture] Process %lu not found", processId);
        return false;
    }

    DLL_Log("[AppAudioCapture] Starting capture for PID %lu", processId);
    if (stopEvent_ && !ResetEvent(stopEvent_)) {
        DLL_Log("[AppAudioCapture] ResetEvent for activation cancellation failed: 0x%lx", GetLastError());
        return false;
    }
    shouldStop.store(false);
    workerRecycleRequested.store(false, std::memory_order_release);
    targetPID.store(processId);
    targetProcessName.clear();

    if (!BeginAsyncStartForPID(processId)) {
        targetPID.store(0, std::memory_order_release);
        return false;
    }
    return true;
}

bool AppAudioCapture::StartByName(const std::string& processName) {
    if (!IsSupported()) {
        DLL_Log(
            "[AppAudioCapture] Per-process audio not supported on this "
            "Windows version");
        return false;
    }

    if (isCapturing.load() || isMonitoring.load() || asyncStartInProgress.load(std::memory_order_acquire)) {
        DLL_Log("[AppAudioCapture] Already running, call Stop() first");
        return false;
    }

    if (captureThread.joinable() || pAudioClient || pCaptureClient || pwfx) {
        DLL_Log("[AppAudioCapture] Cleaning up completed/partial capture state before process monitor start");
        Stop(true);
    }

    DLL_Log("[AppAudioCapture] Starting monitor for process '%s'", processName.c_str());
    targetProcessName = processName;
    if (stopEvent_ && !ResetEvent(stopEvent_)) {
        DLL_Log("[AppAudioCapture] ResetEvent for activation cancellation failed: 0x%lx", GetLastError());
        targetProcessName.clear();
        return false;
    }
    shouldStop.store(false);
    workerRecycleRequested.store(false, std::memory_order_release);
    isMonitoring.store(true);

    // Start the process monitor thread
    try {
        monitorThread = std::thread(&AppAudioCapture::ProcessMonitorLoop, this);
    } catch (const std::exception& error) {
        DLL_Log("[AppAudioCapture] Failed to create process monitor thread: %s", error.what());
        isMonitoring.store(false, std::memory_order_release);
        shouldStop.store(true, std::memory_order_release);
        targetProcessName.clear();
        return false;
    }

    return true;
}

void AppAudioCapture::Stop(bool discardPendingPackets) {
    shouldStop.store(true);
    if (stopEvent_) {
        SetEvent(stopEvent_);
    }
    if (captureEvent_) {
        SetEvent(captureEvent_);
    }

    {
        std::lock_guard<std::mutex> lock(startMutex);
        if (pendingStartFuture.valid()) {
            pendingStartFuture.wait();
            try {
                (void)pendingStartFuture.get();
            } catch (const std::exception& error) {
                DLL_Log("[AppAudioCapture] Async start raised during Stop: %s", error.what());
            }
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

    const bool sessionMonitorWasRunning = audioSessionMonitor_.IsRunning();
    audioSessionMonitor_.Stop();
    const size_t monitoredEndpoints = audioSessionMonitor_.RegisteredEndpointCount();
    const uint64_t sessionNotifications = audioSessionMonitor_.Generation();
    const uint64_t droppedSessionNotifications = audioSessionMonitor_.DroppedNotificationCount();
    if (sessionMonitorWasRunning) {
        DLL_Log("[AppAudioCapture] Audio-session monitor stopped: endpoints=%zu notifications=%llu dropped=%llu",
                monitoredEndpoints, static_cast<unsigned long long>(sessionNotifications),
                static_cast<unsigned long long>(droppedSessionNotifications));
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
    AudioPacket queuedPacket;
    {
        std::lock_guard<std::mutex> lock(queueMutex);
        if (packetQueue.empty())
            return false;
        queuedPacket = std::move(packetQueue.front());
        packetQueue.pop_front();
        if (!packetQueue.empty() && packetReadyEvent_) {
            SetEvent(packetReadyEvent_);
        }
    }
    packet = std::move(queuedPacket);
    return true;
}

void AppAudioCapture::DiscardPendingPackets() {
    std::deque<AudioPacket> discarded;
    {
        std::lock_guard<std::mutex> lock(queueMutex);
        discarded.swap(packetQueue);
        if (packetReadyEvent_) {
            ResetEvent(packetReadyEvent_);
        }
    }
    if (!discarded.empty()) {
        DLL_Log("[AppAudioCapture] Discarding %zu queued packets for PID %lu", discarded.size(), targetPID.load());
    }
}

size_t AppAudioCapture::PendingPacketCount() {
    std::lock_guard<std::mutex> lock(queueMutex);
    return packetQueue.size();
}

bool AppAudioCapture::BeginAsyncStartForPID(DWORD pid) {
    std::lock_guard<std::mutex> lock(startMutex);
    if (pendingStartFuture.valid()) {
        pendingStartFuture.wait();
        bool started = false;
        try {
            started = pendingStartFuture.get();
        } catch (const std::exception& error) {
            DLL_Log("[AppAudioCapture] Prior async start raised for PID %lu: %s", targetPID.load(), error.what());
        }
        if (!started) {
            DLL_Log("[AppAudioCapture] Async start failed for PID %lu", targetPID.load());
        }
    }

    asyncStartInProgress.store(true, std::memory_order_release);
    startPendingValid.store(false, std::memory_order_release);
    try {
        pendingStartFuture = std::async(std::launch::async, [this, pid]() {
            bool ok = false;
            try {
                ok = InitializeCaptureForPID(pid);
            } catch (const std::exception& error) {
                DLL_Log("[AppAudioCapture] Async initialization raised for PID %lu: %s", pid, error.what());
                CleanupCapture();
            }
            startPendingResult.store(ok, std::memory_order_release);
            startPendingValid.store(true, std::memory_order_release);
            asyncStartInProgress.store(false, std::memory_order_release);
            // Wake the process-loopback worker even when activation failed and
            // therefore produced no epoch/data record. This keeps worker state
            // transitions event-driven instead of requiring an activation poll.
            if (packetReadyEvent_) {
                SetEvent(packetReadyEvent_);
            }
            return ok;
        });
    } catch (const std::exception& error) {
        DLL_Log("[AppAudioCapture] Failed to launch async start for PID %lu: %s", pid, error.what());
        asyncStartInProgress.store(false, std::memory_order_release);
        startPendingValid.store(false, std::memory_order_release);
        return false;
    }
    return true;
}

void AppAudioCapture::FinalizePendingAsyncStart() {
    if (!startPendingValid.load(std::memory_order_acquire)) {
        return;
    }

    std::lock_guard<std::mutex> lock(startMutex);
    if (pendingStartFuture.valid() &&
        pendingStartFuture.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
        bool ok = false;
        try {
            ok = pendingStartFuture.get();
        } catch (const std::exception& error) {
            DLL_Log("[AppAudioCapture] Async start completion raised for PID %lu: %s", targetPID.load(), error.what());
        }
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
    // ActivateClientForPID has already committed the epoch-start marker. Never
    // clear the queue here: the worker must observe that lifecycle record before
    // the first data packet from the thread launched below.
    queueOverrunPackets.store(0, std::memory_order_relaxed);
    queueOverrunFrames.store(0, std::memory_order_relaxed);

    if (captureThread.joinable()) {
        captureThread.join();
    }

    isCapturing.store(true, std::memory_order_release);
    try {
        captureThread = std::thread(&AppAudioCapture::CaptureLoop, this);
    } catch (const std::exception& error) {
        DLL_Log("[AppAudioCapture] Failed to create capture thread for PID %lu: %s", targetPID.load(), error.what());
        isCapturing.store(false, std::memory_order_release);
        (void)QueueCaptureEpochMarker(AudioPacketRecordType::EndOfStream,
                                      captureEpoch.load(std::memory_order_acquire), "capture thread creation failure");
        CleanupCapture();
        return false;
    }
    return true;
}

bool AppAudioCapture::QueueCaptureEpochMarker(AudioPacketRecordType recordType, uint64_t epoch, const char* reason) {
    if ((recordType != AudioPacketRecordType::EpochStart && recordType != AudioPacketRecordType::EndOfStream) ||
        epoch == 0) {
        DLL_Log("[AppAudioCapture] ERROR: Refusing invalid capture epoch marker: record=%u epoch=%llu reason=%s",
                static_cast<unsigned>(recordType), static_cast<unsigned long long>(epoch), reason ? reason : "unknown");
        return false;
    }

    try {
        AudioPacket marker;
        marker.captureEpoch = epoch;
        marker.recordType = recordType;
        marker.endOfStream = recordType == AudioPacketRecordType::EndOfStream;
        size_t markerQueueDepth = 0;
        DWORD signalError = ERROR_SUCCESS;
        {
            std::lock_guard<std::mutex> lock(queueMutex);
            // Lifecycle markers are ordered with data and are never subject to
            // the bounded data-packet retention policy.
            packetQueue.emplace_back(std::move(marker));
            if (packetReadyEvent_ && !SetEvent(packetReadyEvent_)) {
                signalError = GetLastError();
            }
            markerQueueDepth = packetQueue.size();
        }
        DLL_Log("[AppAudioCapture] Queued ordered capture-%s marker: epoch=%llu queueDepth=%zu reason=%s",
                recordType == AudioPacketRecordType::EpochStart ? "start" : "end",
                static_cast<unsigned long long>(epoch), markerQueueDepth, reason ? reason : "unknown");
        if (signalError != ERROR_SUCCESS) {
            DLL_Log("[AppAudioCapture] ERROR: Failed to signal queued capture epoch marker: error=0x%lx",
                    signalError);
        }
        return true;
    } catch (const std::exception& error) {
        DLL_Log("[AppAudioCapture] ERROR: Failed to queue capture epoch marker: record=%u epoch=%llu reason=%s: %s",
                static_cast<unsigned>(recordType), static_cast<unsigned long long>(epoch), reason ? reason : "unknown",
                error.what());
        return false;
    }
}

bool AppAudioCapture::InitializeCaptureForPID(DWORD pid) {
    if (!audioSessionMonitor_.IsRunning()) {
        if (audioSessionMonitor_.Start(stopEvent_)) {
            DLL_Log(
                "[AppAudioCapture] Audio-session creation monitor armed before process-loopback activation: "
                "registeredEndpoints=%zu activeEndpoints=%zu existingSessions=%zu registrationFailures=%zu "
                "firstFailure=0x%lx",
                audioSessionMonitor_.RegisteredEndpointCount(), audioSessionMonitor_.ActiveEndpointCount(),
                audioSessionMonitor_.ExistingSessionCount(), audioSessionMonitor_.RegistrationFailureCount(),
                static_cast<unsigned long>(audioSessionMonitor_.FirstRegistrationFailure()));
        } else {
            DLL_Log(
                "[AppAudioCapture] WARNING: audio-session creation monitor unavailable (hr=0x%lx); "
                "initial process-loopback activation cannot observe a render session created after startup",
                static_cast<unsigned long>(audioSessionMonitor_.StartupResult()));
        }
    }
    if (!ActivateClientForPID(pid, true)) {
        return false;
    }
    return StartCaptureThreadForCurrentClient();
}

bool AppAudioCapture::ReactivateClientForPID(DWORD pid, bool allowEventDriven) {
    // Drop the dead client without releasing the process-loopback COM interfaces
    // (releasing them crashes AudioSes CLoopbackMixer cleanup). The capture
    // thread, packet queue, and downstream track stay alive; we just swap in a
    // freshly activated client so packets resume on the same source.
    AbandonClientInterfaces();
    return ActivateClientForPID(pid, allowEventDriven);
}

bool AppAudioCapture::ActivateAudioInterfaceForPID(DWORD pid, IAudioClient** audioClient) {
    if (!audioClient) {
        return false;
    }
    *audioClient = nullptr;

    // Set up activation parameters for per-process loopback
    AUDIOCLIENT_ACTIVATION_PARAMS audioParams = {};
    audioParams.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
    audioParams.ProcessLoopbackParams.TargetProcessId = pid;
    audioParams.ProcessLoopbackParams.ProcessLoopbackMode = PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;

    PROPVARIANT activateParams = {};
    activateParams.vt = VT_BLOB;
    activateParams.blob.cbSize = sizeof(audioParams);
    activateParams.blob.pBlobData = reinterpret_cast<BYTE*>(&audioParams);

    // Each activation owns its completion event through its handler. Reusing a
    // shared event lets a timed-out old callback wake a later activation and makes
    // the later attempt consume the wrong result.
    auto* handler = new (std::nothrow) ActivationHandler();
    if (!handler || !handler->GetEvent()) {
        DLL_Log("[AppAudioCapture] Failed to create process-loopback activation handler/event: 0x%lx", GetLastError());
        if (handler) {
            handler->Release();
        }
        return false;
    }

    IActivateAudioInterfaceAsyncOperation* asyncOp = nullptr;
    HRESULT hr = ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK, __uuidof(IAudioClient),
                                             &activateParams, handler, &asyncOp);

    if (FAILED(hr)) {
        DLL_Log("[AppAudioCapture] ActivateAudioInterfaceAsync failed: 0x%x", hr);
        handler->Release();
