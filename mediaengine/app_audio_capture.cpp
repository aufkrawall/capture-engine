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
            // Wake the process-loopback helper even when activation failed and
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
    // Clear any stale packets
    {
        std::lock_guard<std::mutex> lock(queueMutex);
        packetQueue.clear();
    }
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
        CleanupCapture();
        return false;
    }
    return true;
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
        if (asyncOp) {
            asyncOp->Release();
        }
        return false;
    }

    // Wait for activation to complete, but let Stop wake the asynchronous start
    // immediately through a dedicated manual-reset cancellation event. Do not use
    // captureEvent_ here: abandoned process-loopback clients can continue signaling
    // their old capture callback event during a recovery activation.
    HANDLE waitHandles[] = {handler->GetEvent(), stopEvent_};
    const DWORD waitHandleCount = stopEvent_ ? 2 : 1;
    const DWORD waitResult = WaitForMultipleObjects(waitHandleCount, waitHandles, FALSE, 5000);
    if (waitHandleCount == 2 && waitResult == WAIT_OBJECT_0 + 1) {
        DLL_Log("[AppAudioCapture] Activation cancelled for PID %lu during shutdown", pid);
        handler->Release();
        if (asyncOp) {
            asyncOp->Release();
        }
        return false;
    }
    if (waitResult != WAIT_OBJECT_0) {
        DLL_Log("[AppAudioCapture] Activation wait failed for PID %lu: result=0x%lx error=0x%lx", pid, waitResult,
                waitResult == WAIT_FAILED ? GetLastError() : ERROR_TIMEOUT);
        handler->Release();
        if (asyncOp) {
            asyncOp->Release();
        }
        return false;
    }

    // Get the result
    hr = handler->GetResult();
    if (FAILED(hr)) {
        DLL_Log("[AppAudioCapture] Activation failed: 0x%x", hr);
        handler->Release();
        if (asyncOp) {
            asyncOp->Release();
        }
        return false;
    }

    *audioClient = handler->TakeAudioClient();
    handler->Release();
    if (asyncOp) {
        asyncOp->Release();
    }

    if (!*audioClient) {
        DLL_Log("[AppAudioCapture] No audio client obtained");
        return false;
    }
    return true;
}

bool AppAudioCapture::ActivateClientForPID(DWORD pid, bool allowEventDriven) {
    const HRESULT coInitHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(coInitHr) && coInitHr != RPC_E_CHANGED_MODE) {
        DLL_Log("[AppAudioCapture] CoInitializeEx failed: 0x%x", coInitHr);
        return false;
    }
    // S_FALSE is a successful call and increments COM's per-thread init count.
    const bool coInitNeedsUninitialize = SUCCEEDED(coInitHr);
    CE_SCOPE_EXIT(if (coInitNeedsUninitialize) { CoUninitialize(); });

    // Process loopback does not reliably expose the app's native mix format, so
    // request the resolved output layout and let AUTOCONVERTPCM do only the
    // unavoidable source conversion.

    // KSDATAFORMAT_SUBTYPE_IEEE_FLOAT
    const GUID kKsDataFormatSubtypeIeeeFloat = {
        0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

    struct ClientAttempt {
        DWORD streamFlags;
        REFERENCE_TIME bufferDuration;
        bool requiresEvent;
        const char* description;
    };
    const ClientAttempt attempts[] = {
        {AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM, 2000000, false,
         "polling loopback/autoconvert"},
        {AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM, 100000,
         true, "event-driven loopback/autoconvert"},
        {0, 2000000, false, "polling plain"},
    };

    HRESULT hr = E_FAIL;
    bool initialized = false;
    for (const ClientAttempt& attempt : attempts) {
        if (shouldStop.load(std::memory_order_acquire)) {
            DLL_Log("[AppAudioCapture] Activation cancelled for PID %lu during shutdown", pid);
            return false;
        }
        if (attempt.requiresEvent && (!allowEventDriven || !captureEvent_)) {
            continue;
        }

        IAudioClient* activatedClient = nullptr;
        // Atomically record the notification boundary and the render-session
        // processes already known at that boundary. Windows can emit silent
        // placeholder packets even when process loopback was activated before
        // the target's first session existed, so packet arrival alone cannot
        // prove that this client attached to the target session.
        const auto processTree = SnapshotProcessTree();
        std::array<DWORD, 1024> observedSessionProcessIds{};
        size_t observedSessionProcessCount = 0;
        const uint64_t activationGeneration = audioSessionMonitor_.SnapshotGenerationAndObservedProcessIds(
            observedSessionProcessIds.data(), observedSessionProcessIds.size(), &observedSessionProcessCount);
        const bool hadObservedTargetSession =
            std::any_of(observedSessionProcessIds.begin(),
                        observedSessionProcessIds.begin() + observedSessionProcessCount, [&](DWORD sessionProcessId) {
                            return ce::process_loopback::ProcessBelongsToTree(processTree, sessionProcessId, pid);
                        });
        activationHadObservedTargetSession_.store(hadObservedTargetSession, std::memory_order_release);
        activationAudioSessionGeneration_.store(activationGeneration, std::memory_order_release);
        DLL_Log(
            "[AppAudioCapture] Process-loopback activation boundary: PID=%lu generation=%llu "
            "observedSessionProcesses=%zu targetSessionObserved=%d mode=%s",
            pid, static_cast<unsigned long long>(activationGeneration), observedSessionProcessCount,
            hadObservedTargetSession ? 1 : 0, attempt.description);
        if (!ActivateAudioInterfaceForPID(pid, &activatedClient)) {
            // Activation is independent of the event/polling flags used later by
            // IAudioClient::Initialize. Repeating a five-second activation timeout
            // once per mode only stalls capture and shutdown for up to 15 seconds.
            DLL_Log(
                "[AppAudioCapture] Audio-interface activation failed for %s; capture-mode fallbacks were not "
                "reached",
                attempt.description);
            return false;
        }
        pAudioClient = activatedClient;
        if (shouldStop.load(std::memory_order_acquire)) {
            DLL_Log("[AppAudioCapture] Activation completed for PID %lu after shutdown began; abandoning client", pid);
            AbandonClientInterfaces();
            return false;
        }

        pwfx = static_cast<WAVEFORMATEX*>(CoTaskMemAlloc(sizeof(WAVEFORMATEXTENSIBLE)));
        if (!pwfx) {
            DLL_Log("[AppAudioCapture] Failed to allocate capture format");
            AbandonClientInterfaces();
            return false;
        }
        std::memset(pwfx, 0, sizeof(WAVEFORMATEXTENSIBLE));
        auto* wfex = reinterpret_cast<WAVEFORMATEXTENSIBLE*>(pwfx);
        wfex->Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
        wfex->Format.nChannels = static_cast<WORD>(std::clamp(requestedChannels, 1, 8));
        wfex->Format.nSamplesPerSec = static_cast<DWORD>(requestedSampleRate > 0 ? requestedSampleRate : 48000);
        wfex->Format.wBitsPerSample = 32;
        wfex->Format.nBlockAlign = wfex->Format.nChannels * wfex->Format.wBitsPerSample / 8;
        const uint64_t avgBytesPerSec = static_cast<uint64_t>(wfex->Format.nSamplesPerSec) * wfex->Format.nBlockAlign;
        if (avgBytesPerSec > std::numeric_limits<DWORD>::max()) {
            DLL_Log("[AppAudioCapture] Requested format byte rate is not representable: rate=%lu blockAlign=%u",
                    wfex->Format.nSamplesPerSec, wfex->Format.nBlockAlign);
            AbandonClientInterfaces();
            return false;
        }
        wfex->Format.nAvgBytesPerSec = static_cast<DWORD>(avgBytesPerSec);
        wfex->Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
        wfex->Samples.wValidBitsPerSample = 32;
        wfex->dwChannelMask = requestedChannelMask;
        wfex->SubFormat = kKsDataFormatSubtypeIeeeFloat;

        hr = pAudioClient->Initialize(AUDCLNT_SHAREMODE_SHARED, attempt.streamFlags, attempt.bufferDuration, 0, pwfx,
                                      nullptr);
        if (FAILED(hr)) {
            DLL_Log("[AppAudioCapture] Initialize failed for %s: 0x%x; trying next mode", attempt.description, hr);
            // IAudioClient cannot be safely initialized again. Obtain a fresh
            // process-loopback interface for every fallback attempt.
            AbandonClientInterfaces();
            continue;
        }
        activeStreamFlags = attempt.streamFlags;

        if (attempt.requiresEvent) {
            const BOOL resetOk = ResetEvent(captureEvent_);
            hr = resetOk ? pAudioClient->SetEventHandle(captureEvent_) : HRESULT_FROM_WIN32(GetLastError());
            if (FAILED(hr)) {
                DLL_Log(
                    "[AppAudioCapture] SetEventHandle failed for initialized event client: 0x%x; "
                    "re-activating in polling mode",
                    hr);
                // Clearing activeStreamFlags alone is incorrect: the initialized
                // client still requires an event and Start would fail or never wake.
                AbandonClientInterfaces();
                continue;
            }
        }

        initialized = true;
        break;
    }

    if (!initialized || !pAudioClient || !pwfx) {
        DLL_Log("[AppAudioCapture] All process-loopback initialization modes failed for PID %lu", pid);
        AbandonClientInterfaces();
        return false;
    }

    REFERENCE_TIME streamLatency = 0;
    hr = pAudioClient->GetStreamLatency(&streamLatency);
    if (SUCCEEDED(hr)) {
        streamLatency100ns = static_cast<uint64_t>(std::max<REFERENCE_TIME>(0, streamLatency));
    } else {
        DLL_Log("[AppAudioCapture] GetStreamLatency failed: 0x%x", hr);
        streamLatency100ns = 0;
    }
    REFERENCE_TIME defaultPeriod = 0;
    REFERENCE_TIME minPeriod = 0;
    if (SUCCEEDED(pAudioClient->GetDevicePeriod(&defaultPeriod, &minPeriod))) {
        defaultDevicePeriod100ns = static_cast<uint64_t>(std::max<REFERENCE_TIME>(0, defaultPeriod));
        minDevicePeriod100ns = static_cast<uint64_t>(std::max<REFERENCE_TIME>(0, minPeriod));
    } else {
        defaultDevicePeriod100ns = 0;
        minDevicePeriod100ns = 0;
    }
    UINT32 bufferFrames = 0;
    bufferFrameCount = SUCCEEDED(pAudioClient->GetBufferSize(&bufferFrames)) ? bufferFrames : 0;

    // Get capture client
    hr = pAudioClient->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void**>(&pCaptureClient));
    if (FAILED(hr) || !pCaptureClient) {
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
    const uint64_t activatedEpoch = captureEpoch.fetch_add(1, std::memory_order_acq_rel) + 1;
    {
        std::lock_guard<std::mutex> lock(queueMutex);
        AudioPacket epochStart;
        epochStart.captureEpoch = activatedEpoch;
        epochStart.recordType = AudioPacketRecordType::EpochStart;
        packetQueue.emplace_back(std::move(epochStart));
        if (packetReadyEvent_) {
            SetEvent(packetReadyEvent_);
        }
    }

    const uint64_t bufferDurationUs =
        (pwfx->nSamplesPerSec > 0)
            ? (static_cast<uint64_t>(bufferFrameCount) * 1000000ull) / static_cast<uint64_t>(pwfx->nSamplesPerSec)
            : 0;
    DLL_Log(
        "[AppAudioCapture] Started: PID=%lu epoch=%llu channels=%d rate=%d bits=%d streamLatency=%lluus "
        "devicePeriod=%lluus minPeriod=%lluus bufferFrames=%u bufferDur=%lluus "
        "(latency routed via video content delay, not audio advance)",
        pid, static_cast<unsigned long long>(activatedEpoch), pwfx->nChannels, pwfx->nSamplesPerSec,
        pwfx->wBitsPerSample, static_cast<unsigned long long>(streamLatency100ns / 10),
        static_cast<unsigned long long>(defaultDevicePeriod100ns / 10),
        static_cast<unsigned long long>(minDevicePeriod100ns / 10), bufferFrameCount,
        static_cast<unsigned long long>(bufferDurationUs));

    DLL_Log("[AppAudioCapture] Capture mode contract: selected=%s preference=polling-first eventFallbackAllowed=%d",
            (activeStreamFlags & AUDCLNT_STREAMFLAGS_EVENTCALLBACK) != 0 ? "event-driven" : "polling",
            allowEventDriven ? 1 : 0);

    return true;
}

void AppAudioCapture::AbandonClientInterfaces() {
    // Process loopback is backed by AudioSes' CLoopbackMixer. On current Windows 11
    // builds the mixer can crash in AudioLimiterAPO cleanup when the process-loopback
    // COM interfaces are released, especially with duplicate process-loopback
    // captures. This object runs in a disposable process-loopback helper, so leave
    // these OS-owned interfaces for helper teardown instead of touching the
    // crash-prone AudioSes cleanup path. A re-activation emits a new epoch; the
    // worker then recycles the helper immediately, bounding abandoned wrappers to
    // one helper generation even during a long recording.
    if (pCaptureClient || pAudioClient) {
        DLL_Log(
            "[AppAudioCapture] Abandoning process-loopback COM interfaces "
            "(audioClient=%p captureClient=%p flags=0x%lx) to avoid AudioSes CLoopbackMixer teardown crash",
            pAudioClient, pCaptureClient, activeStreamFlags);
        pCaptureClient = nullptr;
        pAudioClient = nullptr;
    }

    if (pwfx) {
        CoTaskMemFree(pwfx);
        pwfx = nullptr;
    }

    activeStreamFlags = 0;
    streamLatency100ns = 0;
    defaultDevicePeriod100ns = 0;
    minDevicePeriod100ns = 0;
    bufferFrameCount = 0;
}

void AppAudioCapture::CleanupCapture() {
    if (captureEvent_) {
        ResetEvent(captureEvent_);
    }
    AbandonClientInterfaces();
}

void AppAudioCapture::CaptureLoop() {
    const HRESULT coInitHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(coInitHr) && coInitHr != RPC_E_CHANGED_MODE) {
        DLL_Log("[AppAudioCapture] CaptureLoop CoInitializeEx failed for PID %lu: 0x%x", targetPID.load(), coInitHr);
        isCapturing.store(false, std::memory_order_release);
        startPendingValid.store(false, std::memory_order_release);
        return;
    }
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
    // Content telemetry (diagnostics): per-window silent-flag / amplitude tracking so we can
    // tell whether process loopback keeps delivering REAL audio or goes (and stays) silent
    // while the stream stays alive. Reset each Source Sync log window (~5 s).
    uint64_t windowTotalFrames = 0;
    uint64_t windowSilentFlagFrames = 0;
    uint64_t windowZeroContentFrames = 0;
    float windowPeakAbs = 0.0f;
    uint64_t lastProcessCheckTick = 0;
    uint64_t queueDropPackets = 0;
    uint64_t queueDropFrames = 0;
    uint64_t lastQueueDropLogTick = 0;
    uint64_t finalDrainPackets = 0;
    uint64_t finalDrainFrames = 0;
    uint64_t finalDrainFrameBudget = 0;
    LARGE_INTEGER qpcFreqLI = {};
    const uint64_t qpcFreq =
        QueryPerformanceFrequency(&qpcFreqLI) && qpcFreqLI.QuadPart > 0 ? static_cast<uint64_t>(qpcFreqLI.QuadPart) : 0;
    int qpcSanitizeLogCount = 0;

    // --- Mid-recording stream recovery state (device-invalidation + silent stall) ---
    const ce::audio::StreamRecoveryConfig recoveryCfg = recoveryConfig_;
    uint64_t lastPacketTick = GetTickCount64();  // arms the silent-stall window from stream start
    uint64_t lastNoPacketDiagnosticTick = 0;
    uint64_t lastReactivateTick = 0;
    uint64_t recoveryBackoffMs = 0;
    bool sawAnyPacket = false;
    bool currentActivationQualified = false;
    uint64_t currentActivationStartTick = lastPacketTick;
    uint64_t qualifiedActivationCount = 0;
    uint64_t eventFallbackAttempts = 0;
    uint64_t eventFallbackSuccesses = 0;
    uint64_t reactivateAttempts = 0;
    uint64_t reactivateSuccesses = 0;
    // Per-session throttled error logging (NOT static: must reset every session, or
    // a long-lived process silently swallows every error after the first session).
    int fatalErrLogCount = 0;
    int transientErrLogCount = 0;
    int getBufferErrLogCount = 0;
    int emptyBufferLogCount = 0;
    int releaseBufferErrLogCount = 0;
    int invalidPacketLogCount = 0;
    int allocationFailureLogCount = 0;
    int discontinuityLogCount = 0;
    constexpr int kErrLogCap = 8;
    // Require a few consecutive misses before declaring the process gone, so a
    // transient OpenProcess hiccup cannot permanently kill an otherwise-live capture.
    int processMissingStreak = 0;
    constexpr int kProcessMissingStreakToExit = 3;

    // Tear down the dead client and re-activate in place. Honors backoff so a stream
    // that cannot be recovered (or an app that is legitimately paused) is not hammered.
    auto attemptReactivate = [&](const char* reason, long hrCode, bool allowEventDriven = true) -> bool {
        const uint64_t now = GetTickCount64();
        if (!ce::audio::RecoveryBackoffElapsed(now, lastReactivateTick, recoveryBackoffMs)) {
            return false;
        }
        const DWORD pid = targetPID.load();
        ++reactivateAttempts;
        lastReactivateTick = now;
        recoveryBackoffMs = ce::audio::NextRecoveryBackoffMs(recoveryBackoffMs, recoveryCfg);
        DLL_Log(
            "[AppAudioCapture] Re-activating process-loopback stream for PID %lu (reason=%s hr=0x%lx "
            "attempt=%llu nextBackoffMs=%llu)",
            pid, reason, static_cast<unsigned long>(hrCode), static_cast<unsigned long long>(reactivateAttempts),
            static_cast<unsigned long long>(recoveryBackoffMs));
        const bool ok = ReactivateClientForPID(pid, allowEventDriven);
        // A successful replacement must qualify with its own first packet before
        // another silent-stall recovery can arm. A failed/null client still retries
        // through the explicit backoff path below.
        lastPacketTick = GetTickCount64();
        currentActivationStartTick = lastPacketTick;
        currentActivationQualified = false;
        if (ok) {
            ++reactivateSuccesses;
            firstSet = false;
            DLL_Log("[AppAudioCapture] Re-activation succeeded for PID %lu (attempt=%llu mode=%s)", pid,
                    static_cast<unsigned long long>(reactivateAttempts),
                    (activeStreamFlags & AUDCLNT_STREAMFLAGS_EVENTCALLBACK) != 0 ? "event-driven" : "polling");
        } else {
            DLL_Log("[AppAudioCapture] Re-activation FAILED for PID %lu (attempt=%llu) - will retry with backoff", pid,
                    static_cast<unsigned long long>(reactivateAttempts));
        }
        return ok;
    };

    auto readNextPacketSize = [&](const char* context) -> bool {
        packetLength = 0;
        const HRESULT packetHr = pCaptureClient->GetNextPacketSize(&packetLength);
        if (SUCCEEDED(packetHr)) {
            return true;
        }
        if (ce::audio::IsFatalWasapiStreamError(packetHr)) {
            if (fatalErrLogCount++ < kErrLogCap) {
                DLL_Log(
                    "[AppAudioCapture] FATAL stream error from GetNextPacketSize (%s): 0x%lx (PID %lu) - "
                    "attempting re-activation",
                    context, static_cast<unsigned long>(packetHr), targetPID.load());
            }
            if (isCapturing.load(std::memory_order_acquire) && !shouldStop.load(std::memory_order_acquire)) {
                attemptReactivate("GetNextPacketSize_fatal", packetHr);
            }
        } else if (transientErrLogCount++ < kErrLogCap) {
            DLL_Log("[AppAudioCapture] GetNextPacketSize failed (%s): 0x%lx", context,
                    static_cast<unsigned long>(packetHr));
        }
        return false;
    };

    while (true) {
        const bool drainingAfterStop =
            !isCapturing.load(std::memory_order_acquire) || shouldStop.load(std::memory_order_acquire);
        if (drainingAfterStop) {
            // Stop wakes the worker, then the worker owns one non-blocking drain
            // of packets already committed by WASAPI. This preserves the audio
            // tail without waiting for new data or reactivating a dying stream.
            if (finalDrainFrameBudget == 0) {
                const uint32_t fallbackFrames =
                    ce::audio::MaxWasapiCapturePacketFrames(pwfx ? pwfx->nSamplesPerSec : 0);
                finalDrainFrameBudget = bufferFrameCount != 0 ? bufferFrameCount : fallbackFrames;
                if (finalDrainFrameBudget == 0) {
                    finalDrainFrameBudget = 1;
                }
            }
            if (!pCaptureClient || !readNextPacketSize("final stop drain") || packetLength == 0) {
                break;
            }
        }

        const uint64_t nowTick = GetTickCount64();
        if (!drainingAfterStop && nowTick - lastProcessCheckTick >= 500) {
            lastProcessCheckTick = nowTick;
            const DWORD activePid = targetPID.load(std::memory_order_acquire);
            if (!IsProcessRunning(activePid)) {
                if (++processMissingStreak >= kProcessMissingStreakToExit) {
                    DLL_Log("[AppAudioCapture] Target process %lu exited (missed %d consecutive checks)", activePid,
                            processMissingStreak);
                    targetPID.store(0, std::memory_order_release);
                    break;
                }
                DLL_Log("[AppAudioCapture] Target process %lu not found on check %d/%d - deferring exit", activePid,
                        processMissingStreak, kProcessMissingStreakToExit);
            } else {
                processMissingStreak = 0;
            }
        }

        const HANDLE sessionActivityEvent = audioSessionMonitor_.GetActivityEvent();
        if (!drainingAfterStop && (activeStreamFlags & AUDCLNT_STREAMFLAGS_EVENTCALLBACK) != 0 && captureEvent_) {
            HANDLE waitHandles[] = {captureEvent_, sessionActivityEvent};
            const DWORD waitHandleCount = sessionActivityEvent ? 2 : 1;
            const DWORD waitResult = WaitForMultipleObjects(waitHandleCount, waitHandles, FALSE, 200);
            if (waitResult == WAIT_FAILED) {
                DLL_Log("[AppAudioCapture] Capture/session notification wait failed: 0x%lx", GetLastError());
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        } else if (!drainingAfterStop) {
            if (sessionActivityEvent) {
                WaitForSingleObject(sessionActivityEvent, 10);
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }

        if (!drainingAfterStop) {
            std::array<ce::process_loopback::AudioSessionCreation, 64> sessionCreations{};
            const size_t sessionCreationCount =
                audioSessionMonitor_.TakeSessionCreations(sessionCreations.data(), sessionCreations.size());
            if (sessionCreationCount != 0) {
                const uint64_t activationGeneration = activationAudioSessionGeneration_.load(std::memory_order_acquire);
                const bool activationHadObservedTargetSession =
                    activationHadObservedTargetSession_.load(std::memory_order_acquire);
                const DWORD activePid = targetPID.load(std::memory_order_acquire);
                const auto processTree = SnapshotProcessTree();
                for (size_t creationIndex = 0; creationIndex < sessionCreationCount; ++creationIndex) {
                    const auto& creation = sessionCreations[creationIndex];
                    if (!ce::process_loopback::ShouldRecycleCaptureForSessionCreation(
                            processTree, currentActivationQualified, activationHadObservedTargetSession, activePid,
                            creation.processId, activationGeneration, creation.generation)) {
                        continue;
                    }
                    workerRecycleRequested.store(true, std::memory_order_release);
                    if (packetReadyEvent_) {
                        SetEvent(packetReadyEvent_);
                    }
                    DLL_Log(
                        "[AppAudioCapture] Target render session requires a fresh process-loopback binding: "
                        "targetPID=%lu sessionPID=%lu notificationGeneration=%llu activationGeneration=%llu "
                        "activationQualified=%d targetSessionObservedAtActivation=%d epoch=%llu; recycling the "
                        "disposable helper",
                        activePid, creation.processId, static_cast<unsigned long long>(creation.generation),
                        static_cast<unsigned long long>(activationGeneration), currentActivationQualified ? 1 : 0,
                        activationHadObservedTargetSession ? 1 : 0,
                        static_cast<unsigned long long>(captureEpoch.load(std::memory_order_relaxed)));
                    break;
                }
                if (workerRecycleRequested.load(std::memory_order_acquire)) {
                    break;
                }
            }
        }

        // A previous re-activation may have failed and abandoned the client; recover
        // it here (with backoff) before any client call, so we never deref nullptr.
        if (!pCaptureClient) {
            if (!drainingAfterStop) {
                attemptReactivate("client_null", 0);
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            continue;
        }

        if (!drainingAfterStop && !readNextPacketSize("outer")) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }

        if (packetLength == 0) {
            if (drainingAfterStop) {
                break;
            }
            const uint64_t noPacketNow = GetTickCount64();
            const uint64_t activationElapsedMs =
                noPacketNow >= currentActivationStartTick ? noPacketNow - currentActivationStartTick : 0;
            const bool eventDriven = (activeStreamFlags & AUDCLNT_STREAMFLAGS_EVENTCALLBACK) != 0;
            if (ce::audio::ShouldFallbackUnqualifiedEventCapture(
                    eventDriven, currentActivationQualified, activationElapsedMs, recoveryCfg) &&
                ce::audio::RecoveryBackoffElapsed(noPacketNow, lastReactivateTick, recoveryBackoffMs)) {
                ++eventFallbackAttempts;
                DLL_Log(
                    "[AppAudioCapture] WARNING: event-driven activation failed first-packet qualification for "
                    "PID %lu after %llu ms (epoch=%llu); re-activating polling-only and recycling the helper "
                    "generation to bound abandoned AudioSes state",
                    targetPID.load(), static_cast<unsigned long long>(activationElapsedMs),
                    static_cast<unsigned long long>(captureEpoch.load(std::memory_order_relaxed)));
                if (attemptReactivate("event_first_packet_timeout", 0, false)) {
                    ++eventFallbackSuccesses;
                }
                continue;
            }
            if (!currentActivationQualified && activationElapsedMs >= 3000 &&
                (lastNoPacketDiagnosticTick == 0 || noPacketNow - lastNoPacketDiagnosticTick >= 30000)) {
                DLL_Log(
                    "[AppAudioCapture] WARNING: process-loopback stream is active but has delivered no data "
                    "packets for %llu ms: PID=%lu process=%s epoch=%llu mode=%s processAlive=%d everPacket=%d. "
                    "sessionMonitor=%d activationSessionGeneration=%llu observedSessionGeneration=%llu "
                    "targetSessionObservedAtActivation=%d. The "
                    "polling route remains expected timeline silence; a matching post-activation render-session "
                    "notification will trigger exact recovery. Verify process-tree root selection and target audio "
                    "activity",
                    static_cast<unsigned long long>(activationElapsedMs), targetPID.load(),
                    targetProcessName.empty() ? "<pid-mode>" : targetProcessName.c_str(),
                    static_cast<unsigned long long>(captureEpoch.load(std::memory_order_relaxed)),
                    (activeStreamFlags & AUDCLNT_STREAMFLAGS_EVENTCALLBACK) != 0 ? "event-driven" : "polling",
                    IsProcessRunning(targetPID.load()) ? 1 : 0, sawAnyPacket ? 1 : 0,
                    audioSessionMonitor_.IsRunning() ? 1 : 0,
                    static_cast<unsigned long long>(activationAudioSessionGeneration_.load(std::memory_order_acquire)),
                    static_cast<unsigned long long>(audioSessionMonitor_.Generation()),
                    activationHadObservedTargetSession_.load(std::memory_order_acquire) ? 1 : 0);
                lastNoPacketDiagnosticTick = noPacketNow;
            }
            // Silent-stall watchdog: a process-loopback stream that delivered audio
            // before but has gone fully silent (no packets, no error) while its
            // process is still running usually means the app tore down and recreated
            // its audio session (e.g. a game auto-muting on alt-tab) and the mixer
            // did not reattach. Re-activate to reattach to the live session.
            if (ce::audio::ShouldReactivateForSilentStall(currentActivationQualified, GetTickCount64(), lastPacketTick,
                                                          lastReactivateTick, recoveryBackoffMs, recoveryCfg)) {
                DLL_Log(
                    "[AppAudioCapture] Process-loopback silent stall for PID %lu (%llu ms without packets, process "
                    "alive) - re-activating",
                    targetPID.load(), static_cast<unsigned long long>(GetTickCount64() - lastPacketTick));
                attemptReactivate("silent_stall", 0);
            }
            continue;
        }

        while (packetLength != 0) {
            if (drainingAfterStop && finalDrainFrames >= finalDrainFrameBudget) {
                DLL_Log(
                    "[AppAudioCapture] Final drain reached the endpoint-buffer bound (%llu frame(s)); leaving "
                    "newly-arrived data for stream teardown (PID %lu)",
                    static_cast<unsigned long long>(finalDrainFrameBudget), targetPID.load());
                packetLength = 0;
                break;
            }
            hr = pCaptureClient->GetBuffer(&pData, &numFramesAvailable, &flags, &devicePosition, &qpcPosition);
            if (hr == AUDCLNT_S_BUFFER_EMPTY) {
                // No packet was acquired for this success status. In particular,
                // do not call ReleaseBuffer: that would be AUDCLNT_E_OUT_OF_ORDER.
                if (emptyBufferLogCount++ < kErrLogCap) {
                    DLL_Log(
                        "[AppAudioCapture] GetBuffer returned AUDCLNT_S_BUFFER_EMPTY for PID %lu after announcing "
                        "%u frame(s); waiting for the next capture notification",
                        targetPID.load(), packetLength);
                }
                packetLength = 0;
                break;
            }
            if (FAILED(hr)) {
                if (ce::audio::IsFatalWasapiStreamError(hr)) {
                    if (fatalErrLogCount++ < kErrLogCap) {
                        DLL_Log(
                            "[AppAudioCapture] FATAL stream error from GetBuffer: 0x%lx (PID %lu) - attempting "
                            "re-activation",
                            static_cast<unsigned long>(hr), targetPID.load());
                    }
                    if (!drainingAfterStop) {
                        attemptReactivate("GetBuffer_fatal", hr);
                    }
                } else if (getBufferErrLogCount++ < kErrLogCap) {
                    DLL_Log("[AppAudioCapture] GetBuffer failed: 0x%lx", static_cast<unsigned long>(hr));
                }
                break;
            }
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
                    if (qpcSanitizeLogCount++ < kErrLogCap) {
                        DLL_Log(
                            "[AppAudioCapture] WARNING: out-of-domain WASAPI qpcPosition=%llu substituted with "
                            "nowQpc=%llu (PID=%lu frames=%u flags=0x%lx%s)",
                            static_cast<unsigned long long>(qpcPosition), static_cast<unsigned long long>(nowQpc100ns),
                            targetPID.load(), numFramesAvailable, flags,
                            (flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR) != 0 ? " TIMESTAMP_ERROR" : "");
                    }
                    qpcPosition = sanitizedQpc;
                }
            }

            size_t bytes = 0;
            const bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
            const bool frameCountValid = ce::audio::IsWasapiCapturePacketFrameCountValid(
                packetLength, numFramesAvailable, bufferFrameCount, pwfx ? pwfx->nSamplesPerSec : 0);
            const bool byteSizeValid =
                pwfx && ce::audio::TryComputeAudioPacketByteSize(numFramesAvailable, pwfx->nBlockAlign, &bytes);
            if (!frameCountValid || !byteSizeValid || (!silent && !pData)) {
                if (invalidPacketLogCount++ < kErrLogCap) {
                    DLL_Log(
                        "[AppAudioCapture] WARNING: rejecting malformed WASAPI packet for PID %lu: announced=%u "
                        "actual=%u bufferFrames=%u sampleRate=%u maxPacketFrames=%u blockAlign=%u data=%p "
                        "flags=0x%lx",
                        targetPID.load(), packetLength, numFramesAvailable, bufferFrameCount,
                        pwfx ? pwfx->nSamplesPerSec : 0,
                        ce::audio::MaxWasapiCapturePacketFrames(pwfx ? pwfx->nSamplesPerSec : 0),
                        pwfx ? pwfx->nBlockAlign : 0, pData, flags);
                }
                const HRESULT releaseHr = pCaptureClient->ReleaseBuffer(numFramesAvailable);
                if (FAILED(releaseHr)) {
                    if (releaseBufferErrLogCount++ < kErrLogCap) {
                        DLL_Log("[AppAudioCapture] ReleaseBuffer failed after malformed packet: 0x%lx",
                                static_cast<unsigned long>(releaseHr));
                    }
                    if (!drainingAfterStop) {
                        attemptReactivate("ReleaseBuffer_malformed", releaseHr);
                    }
                    packetLength = 0;
                    break;
                }
                if (!readNextPacketSize("after malformed packet")) {
                    break;
                }
                continue;
            }
            if ((flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) != 0 && discontinuityLogCount < kErrLogCap) {
                ++discontinuityLogCount;
                DLL_Log(
                    "[AppAudioCapture] WASAPI data discontinuity for PID %lu: frames=%u devPos=%llu qpc=%llu "
                    "(occurrence=%d)",
                    targetPID.load(), numFramesAvailable, static_cast<unsigned long long>(devicePosition),
                    static_cast<unsigned long long>(qpcPosition), discontinuityLogCount);
            }

            // Healthy, validated delivery: reset the silent-stall window and
            // clear backoff so future recovery is responsive.
            lastPacketTick = GetTickCount64();
            if (!currentActivationQualified) {
                currentActivationQualified = true;
                ++qualifiedActivationCount;
                DLL_Log(
                    "[AppAudioCapture] First-packet qualification succeeded: PID=%lu epoch=%llu mode=%s "
                    "elapsed=%llums frames=%u flags=0x%lx targetSessionObservedAtActivation=%d",
                    targetPID.load(), static_cast<unsigned long long>(captureEpoch.load(std::memory_order_relaxed)),
                    (activeStreamFlags & AUDCLNT_STREAMFLAGS_EVENTCALLBACK) != 0 ? "event-driven" : "polling",
                    static_cast<unsigned long long>(
                        lastPacketTick >= currentActivationStartTick ? lastPacketTick - currentActivationStartTick : 0),
                    numFramesAvailable, flags,
                    activationHadObservedTargetSession_.load(std::memory_order_acquire) ? 1 : 0);
            }
            sawAnyPacket = true;
            recoveryBackoffMs = 0;
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
                const uint64_t packetStartQpc = ce::audio::ApplyProcessLoopbackPacketTimestampCompensation(
                    firstQpcPos, numFramesAvailable, pwfx->nSamplesPerSec);
                // streamLatency telemetry only; placement uses packetStartQpc (period-center). The
                // wouldAdvanceQpc shows the retired GetStreamLatency audio-advance, not applied.
                const uint64_t wouldAdvanceQpc =
                    ce::audio::ApplyCaptureLatencyCompensation(packetStartQpc, streamLatency100ns, true);
                DLL_Log(
                    "[AppAudioCapture] Source Sync Start (%lu): Frames=%llu QPC=%llu placedQPC=%llu "
                    "wouldAdvanceQpc=%llu streamLatency=%lluus packetFrames=%u packetDuration=%lluus "
                    "processLoopbackPacketBias=half_period (latency via video delay, not applied)",
                    targetPID.load(), firstLogicalFramePos, firstQpcPos, packetStartQpc, wouldAdvanceQpc,
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
                // Diagnostics: is the captured CONTENT real or silent this window? peakAbs≈0 with
                // high silent/zero frame ratios while the stream stays alive points to a stuck-silent
                // process loopback; peakAbs>0 means real audio is captured and any track silence is
                // a downstream placement/consumption problem, not capture.
                const double silentPct =
                    windowTotalFrames > 0
                        ? (100.0 * static_cast<double>(windowSilentFlagFrames + windowZeroContentFrames) /
                           static_cast<double>(windowTotalFrames))
                        : 0.0;
                DLL_Log(
                    "[AppAudioCapture] Content (%lu): peakAbs=%.5f silentFlagFrames=%llu zeroFrames=%llu "
                    "totalFrames=%llu silent=%.1f%%",
                    targetPID.load(), windowPeakAbs, static_cast<unsigned long long>(windowSilentFlagFrames),
                    static_cast<unsigned long long>(windowZeroContentFrames),
                    static_cast<unsigned long long>(windowTotalFrames), silentPct);
                windowTotalFrames = 0;
                windowSilentFlagFrames = 0;
                windowZeroContentFrames = 0;
                windowPeakAbs = 0.0f;
            }

            // Build packet with format info
            AudioPacket packet{};
            packet.channels = pwfx->nChannels;
            packet.sampleRate = pwfx->nSamplesPerSec;
            packet.bitsPerSample = pwfx->wBitsPerSample;
            packet.blockAlign = pwfx->nBlockAlign;
            packet.validBitsPerSample = 0;
            packet.channelMask = 0;
            packet.devicePosition = devicePosition;     // Store for debug drift analysis
            packet.rawQpcPosition = rawQpcPosition;     // Store raw WASAPI timestamp for debug drift analysis
            packet.streamLatency = streamLatency100ns;  // telemetry only (see below)
            packet.captureEpoch = captureEpoch.load(std::memory_order_acquire);
            // Period-center bias only (process loopback reports end-of-period QPCs). Do NOT advance
            // by streamLatency: the render->loopback A/V offset is corrected by delaying video
            // content (audio/PTS untouched), never by advancing live audio (the earlier samples do
            // not exist; the CFR pipeline absorbs the shift). Process loopback shares the render
            // endpoint, so it inherits audio_capture_latency_ms and is handled by the video delay.
            packet.qpcPosition = ce::audio::ApplyProcessLoopbackPacketTimestampCompensation(
                qpcPosition, numFramesAvailable, pwfx->nSamplesPerSec);

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
            try {
                packet.data.resize(bytes);
            } catch (const std::exception& error) {
                if (allocationFailureLogCount++ < kErrLogCap) {
                    DLL_Log(
                        "[AppAudioCapture] WARNING: packet allocation failed for PID %lu: %zu byte(s) / %u "
                        "frame(s): %s; dropping this packet without terminating capture",
                        targetPID.load(), bytes, numFramesAvailable, error.what());
                }
                const HRESULT releaseHr = pCaptureClient->ReleaseBuffer(numFramesAvailable);
                if (FAILED(releaseHr)) {
                    if (releaseBufferErrLogCount++ < kErrLogCap) {
                        DLL_Log("[AppAudioCapture] ReleaseBuffer failed after packet allocation error: 0x%lx",
                                static_cast<unsigned long>(releaseHr));
                    }
                    if (!drainingAfterStop) {
                        attemptReactivate("ReleaseBuffer_allocation_failed", releaseHr);
                    }
                    packetLength = 0;
                    break;
                }
                if (!readNextPacketSize("after packet allocation failure")) {
                    break;
                }
                continue;
            }

            windowTotalFrames += numFramesAvailable;
            if (silent) {
                memset(packet.data.data(), 0, bytes);
                windowSilentFlagFrames += numFramesAvailable;
            } else {
                memcpy(packet.data.data(), pData, bytes);
                // Content amplitude probe (diagnostics): our requested capture format is
                // 32-bit IEEE float, so scan the packet for the peak |sample| and whether it
                // is all-zero despite no SILENT flag. This distinguishes "loopback delivers
                // real audio" from "loopback stuck delivering silence" downstream confusion.
                if (packet.isFloat && pwfx->wBitsPerSample == 32) {
                    const float* samples = reinterpret_cast<const float*>(pData);
                    const size_t sampleCount = bytes / sizeof(float);
                    float packetPeak = 0.0f;
                    for (size_t i = 0; i < sampleCount; ++i) {
                        const float a = samples[i] < 0.0f ? -samples[i] : samples[i];
                        if (a > packetPeak) {
                            packetPeak = a;
                        }
                    }
                    if (packetPeak > windowPeakAbs) {
                        windowPeakAbs = packetPeak;
                    }
                    if (packetPeak == 0.0f) {
                        windowZeroContentFrames += numFramesAvailable;
                    }
                }
            }

            bool logQueueDrop = false;
            uint64_t droppedPacketsToLog = 0;
            uint64_t droppedFramesToLog = 0;
            try {
                std::lock_guard<std::mutex> lock(queueMutex);
                // Transactional live-edge retention: if deque growth fails,
                // every already-queued packet remains intact.
                packetQueue.emplace_back(std::move(packet));
                if (packetReadyEvent_) {
                    SetEvent(packetReadyEvent_);
                }
                if (packetQueue.size() > kMaxQueuedPackets) {
                    auto droppedIt = std::find_if(packetQueue.begin(), packetQueue.end(), [](const auto& queued) {
                        return queued.recordType == AudioPacketRecordType::Data;
                    });
                    if (droppedIt == packetQueue.end()) {
                        --droppedIt;  // The packet just appended above is always a data record.
                    }
                    const AudioPacket& droppedPacket = *droppedIt;
                    uint64_t droppedFrames = 0;
                    if (droppedPacket.blockAlign > 0) {
                        droppedFrames = droppedPacket.data.size() / static_cast<size_t>(droppedPacket.blockAlign);
                        queueDropFrames += droppedFrames;
                    }
                    queueDropPackets++;
                    queueOverrunPackets.fetch_add(1, std::memory_order_relaxed);
                    queueOverrunFrames.fetch_add(droppedFrames, std::memory_order_relaxed);
                    packetQueue.erase(droppedIt);

                    const uint64_t queueNowTick = GetTickCount64();
                    if (queueNowTick - lastQueueDropLogTick >= 1000) {
                        logQueueDrop = true;
                        droppedPacketsToLog = queueDropPackets;
                        droppedFramesToLog = queueDropFrames;
                        queueDropPackets = 0;
                        queueDropFrames = 0;
                        lastQueueDropLogTick = queueNowTick;
                    }
                }
            } catch (const std::exception& error) {
                if (allocationFailureLogCount++ < kErrLogCap) {
                    DLL_Log(
                        "[AppAudioCapture] WARNING: capture queue insertion failed for PID %lu: %s; dropping "
                        "the new packet without terminating capture",
                        targetPID.load(), error.what());
                }
            }
            if (logQueueDrop) {
                DLL_Log(
                    "[AppAudioCapture] WARNING: capture queue overrun - dropped %llu packet(s) / %llu frame(s) "
                    "for PID %lu while keeping newest audio (depth=%zu)",
                    static_cast<unsigned long long>(droppedPacketsToLog),
                    static_cast<unsigned long long>(droppedFramesToLog), targetPID.load(), kMaxQueuedPackets);
            }

            if (devicePosition > 0) {
                logicalFrameCursor = devicePosition + numFramesAvailable;
            } else {
                logicalFrameCursor += numFramesAvailable;
            }

            const HRESULT releaseHr = pCaptureClient->ReleaseBuffer(numFramesAvailable);
            if (FAILED(releaseHr)) {
                if (releaseBufferErrLogCount++ < kErrLogCap) {
                    DLL_Log("[AppAudioCapture] ReleaseBuffer failed: 0x%lx - re-activating stream",
                            static_cast<unsigned long>(releaseHr));
                }
                if (!drainingAfterStop) {
                    attemptReactivate("ReleaseBuffer_failed", releaseHr);
                }
                packetLength = 0;
                break;
            }
            if (!readNextPacketSize("inner drain")) {
                break;
            }
        }
        if (drainingAfterStop) {
            break;
        }
    }

    DLL_Log("[AppAudioCapture] Capture loop exited");
    DLL_Log(
        "[AppAudioCapture] First-packet qualification summary for PID %lu: everPacket=%d currentQualified=%d "
        "qualifiedActivations=%llu eventFallback=%llu/%llu finalEpoch=%llu finalMode=%s",
        targetPID.load(), sawAnyPacket ? 1 : 0, currentActivationQualified ? 1 : 0,
        static_cast<unsigned long long>(qualifiedActivationCount),
        static_cast<unsigned long long>(eventFallbackSuccesses), static_cast<unsigned long long>(eventFallbackAttempts),
        static_cast<unsigned long long>(captureEpoch.load(std::memory_order_relaxed)),
        (activeStreamFlags & AUDCLNT_STREAMFLAGS_EVENTCALLBACK) != 0 ? "event-driven" : "polling");
    if (reactivateAttempts > 0) {
        DLL_Log("[AppAudioCapture] Stream recovery summary for PID %lu: %llu re-activation attempt(s), %llu succeeded",
                targetPID.load(), static_cast<unsigned long long>(reactivateAttempts),
                static_cast<unsigned long long>(reactivateSuccesses));
    }
    if (finalDrainPackets > 0 || finalDrainFrames > 0) {
        DLL_Log("[AppAudioCapture] Final stop drain queued %llu packet(s) / %llu frame(s) for PID %lu",
                static_cast<unsigned long long>(finalDrainPackets), static_cast<unsigned long long>(finalDrainFrames),
                targetPID.load());
    }
    if (queueDropPackets > 0 || queueDropFrames > 0) {
        DLL_Log("[AppAudioCapture] Final queue overrun summary for PID %lu: dropped %llu packet(s) / %llu frame(s)",
                targetPID.load(), static_cast<unsigned long long>(queueDropPackets),
                static_cast<unsigned long long>(queueDropFrames));
    }
    try {
        AudioPacket endMarker;
        endMarker.captureEpoch = captureEpoch.load(std::memory_order_acquire);
        endMarker.recordType = AudioPacketRecordType::EndOfStream;
        endMarker.endOfStream = true;
        size_t markerQueueDepth = 0;
        {
            std::lock_guard<std::mutex> lock(queueMutex);
            // The marker is ordered after every captured packet and is not subject to the data-packet
            // retention bound. Downstream fan-out observes it only after every route has received the tail.
            packetQueue.emplace_back(std::move(endMarker));
            if (packetReadyEvent_) {
                SetEvent(packetReadyEvent_);
            }
            markerQueueDepth = packetQueue.size();
        }
        DLL_Log("[AppAudioCapture] Queued ordered capture-end marker: epoch=%llu queueDepth=%zu",
                static_cast<unsigned long long>(captureEpoch.load(std::memory_order_relaxed)), markerQueueDepth);
    } catch (const std::exception& error) {
        DLL_Log("[AppAudioCapture] ERROR: Failed to queue ordered capture-end marker: %s", error.what());
    }
    isCapturing.store(false);
    startPendingValid.store(false, std::memory_order_release);
    if (SUCCEEDED(coInitHr)) {
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
        if (!isCapturing.load()) {
            // CaptureLoop owns the three-consecutive-miss process-exit policy.
            // Reap and clean its completed session before activating a replacement;
            // a second one-shot monitor probe used to bypass that tolerance and
            // permanently stop healthy capture on a transient OpenProcess failure.
            if (captureThread.joinable()) {
                captureThread.join();
                CleanupCapture();
                targetPID.store(0, std::memory_order_release);
            }

            // Not capturing - try to find the target process
            const auto selection = FindProcessByName(targetProcessName);
            const DWORD pid = selection.selectedProcessId;
            if (pid != 0) {
                DLL_Log("[AppAudioCapture] Selected process-tree root '%s' with PID %lu", targetProcessName.c_str(),
                        pid);
                targetPID.store(pid);
                if (!BeginAsyncStartForPID(pid)) {
                    targetPID.store(0, std::memory_order_release);
                }
            }
        }

        // Check every second, but use small intervals for responsive shutdown
        for (int i = 0; i < 10 && isMonitoring.load() && !shouldStop.load(); i++) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    DLL_Log("[AppAudioCapture] Monitor loop exited");
}

ce::process_loopback::ProcessNameSelection AppAudioCapture::FindProcessByName(const std::string& name,
                                                                              bool logSelection) {
    const auto processes = SnapshotProcessTree();
    const auto selection = ce::process_loopback::SelectProcessTreeRootByName(processes, name);
    if (logSelection && selection.selectedProcessId != 0) {
        DLL_Log(
            "[AppAudioCapture] Process-name tree resolution '%s': matches=%zu roots=%zu firstPID=%lu "
            "selectedRootPID=%lu selectedParentPID=%lu selectedNameMembers=%zu selectedProcessTreeMembers=%zu "
            "firstMatchWasRoot=%d",
            name.c_str(), selection.matchingProcessCount, selection.rootCandidateCount,
            static_cast<unsigned long>(selection.firstMatchProcessId),
            static_cast<unsigned long>(selection.selectedProcessId),
            static_cast<unsigned long>(selection.selectedParentProcessId), selection.selectedTreeSize,
            selection.selectedProcessTreeSize, selection.firstMatchProcessId == selection.selectedProcessId ? 1 : 0);
    }
    return selection;
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
