#include "app_audio_capture.h"
#include "audio_capture.h" // For AudioPacket
#include "mediaengine.h"   // For DLL_Log
#include <psapi.h>
#include <tlhelp32.h>
#include <chrono>
#include <combaseapi.h>
#include <functional>
#include <propvarutil.h>

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
static bool IsIEEEFloat(const GUID &g) {
  return g.Data1 == 0x00000003 && g.Data2 == 0x0000 && g.Data3 == 0x0010 &&
         g.Data4[0] == 0x80 && g.Data4[1] == 0x00 && g.Data4[2] == 0x00 &&
         g.Data4[3] == 0xaa && g.Data4[4] == 0x00 && g.Data4[5] == 0x38 &&
         g.Data4[6] == 0x9b && g.Data4[7] == 0x71;
}

// ============================================================================
// ActivationHandler - Implements IActivateAudioInterfaceCompletionHandler
// Must also implement IAgileObject to avoid E_ILLEGAL_METHOD_CALL
// ============================================================================

// IAgileObject GUID - declared in objidlbase.h via DEFINE_GUID
// Just reference it directly without redeclaring

class AppAudioCapture::ActivationHandler
    : public IActivateAudioInterfaceCompletionHandler {
public:
  ActivationHandler(HANDLE completeEvent)
      : refCount(1), completeEvent(completeEvent), resultCode(E_FAIL),
        audioClient(nullptr) {}

  virtual ~ActivationHandler() = default;

  // IUnknown - must return S_OK for IAgileObject to prevent
  // E_ILLEGAL_METHOD_CALL
  STDMETHODIMP QueryInterface(REFIID riid, void **ppvObject) override {
    if (riid == __uuidof(IUnknown) ||
        riid == __uuidof(IActivateAudioInterfaceCompletionHandler) ||
        riid == IID_IAgileObject) {
      *ppvObject =
          static_cast<IActivateAudioInterfaceCompletionHandler *>(this);
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
  STDMETHODIMP ActivateCompleted(
      IActivateAudioInterfaceAsyncOperation *activateOperation) override {
    HRESULT hrActivate = E_FAIL;
    IUnknown *pUnk = nullptr;

    HRESULT hr = activateOperation->GetActivateResult(&hrActivate, &pUnk);
    if (SUCCEEDED(hr) && SUCCEEDED(hrActivate) && pUnk) {
      hr = pUnk->QueryInterface(__uuidof(IAudioClient),
                                reinterpret_cast<void **>(&audioClient));
      pUnk->Release();
    }

    resultCode = SUCCEEDED(hr) ? hrActivate : hr;

    // Signal completion
    SetEvent(completeEvent);
    return S_OK;
  }

  HRESULT GetResult() const { return resultCode; }
  IAudioClient *GetAudioClient() const { return audioClient; }

private:
  LONG refCount;
  HANDLE completeEvent;
  HRESULT resultCode;
  IAudioClient *audioClient;
};

// ============================================================================
// AppAudioCapture Implementation
// ============================================================================
AppAudioCapture::AppAudioCapture() {
  activationCompleteEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
}

AppAudioCapture::~AppAudioCapture() {
  Stop();
  if (activationCompleteEvent) {
    CloseHandle(activationCompleteEvent);
    activationCompleteEvent = nullptr;
  }
}

bool AppAudioCapture::IsSupported() {
  // Check Windows build version
  // Per-process loopback requires Windows 10 build 20348+
  typedef LONG(WINAPI * RtlGetVersionPtr)(PRTL_OSVERSIONINFOW);
  HMODULE hNtdll = GetModuleHandleW(L"ntdll.dll");
  if (!hNtdll)
    return false;

  RtlGetVersionPtr RtlGetVersion =
      (RtlGetVersionPtr)GetProcAddress(hNtdll, "RtlGetVersion");
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
    DLL_Log("[AppAudioCapture] Per-process audio not supported on this "
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
  targetPID.store(processId);
  targetProcessName.clear();

  return InitializeCaptureForPID(processId);
}

bool AppAudioCapture::StartByName(const std::string &processName) {
  if (!IsSupported()) {
    DLL_Log("[AppAudioCapture] Per-process audio not supported on this "
            "Windows version");
    return false;
  }

  if (isCapturing.load() || isMonitoring.load()) {
    DLL_Log("[AppAudioCapture] Already running, call Stop() first");
    return false;
  }

  DLL_Log("[AppAudioCapture] Starting monitor for process '%s'",
          processName.c_str());
  targetProcessName = processName;
  shouldStop.store(false);
  isMonitoring.store(true);

  // Start the process monitor thread
  monitorThread = std::thread(&AppAudioCapture::ProcessMonitorLoop, this);

  return true;
}

void AppAudioCapture::Stop() {
  shouldStop.store(true);

  // Stop monitoring
  isMonitoring.store(false);
  if (monitorThread.joinable()) {
    monitorThread.join();
  }

  // Stop capturing
  isCapturing.store(false);
  if (captureThread.joinable()) {
    captureThread.join();
  }

  CleanupCapture();
  targetPID.store(0);
  targetProcessName.clear();
}

bool AppAudioCapture::GetNextPacket(AudioPacket &packet) {
  std::lock_guard<std::mutex> lock(queueMutex);
  if (packetQueue.empty())
    return false;
  packet = packetQueue.front();
  packetQueue.pop_front();
  return true;
}

bool AppAudioCapture::InitializeCaptureForPID(DWORD pid) {
  HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
    DLL_Log("[AppAudioCapture] CoInitializeEx failed: 0x%x", hr);
    return false;
  }

  // Reset the completion event
  ResetEvent(activationCompleteEvent);

  // Set up activation parameters for per-process loopback
  AUDIOCLIENT_ACTIVATION_PARAMS audioParams = {};
  audioParams.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
  audioParams.ProcessLoopbackParams.TargetProcessId = pid;
  audioParams.ProcessLoopbackParams.ProcessLoopbackMode =
      PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;

  PROPVARIANT activateParams = {};
  activateParams.vt = VT_BLOB;
  activateParams.blob.cbSize = sizeof(audioParams);
  activateParams.blob.pBlobData = reinterpret_cast<BYTE *>(&audioParams);

  // Create completion handler
  auto *handler = new ActivationHandler(activationCompleteEvent);

  IActivateAudioInterfaceAsyncOperation *asyncOp = nullptr;
  hr = ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK,
                                   __uuidof(IAudioClient), &activateParams,
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

  // Get mix format - for process loopback, GetMixFormat may return E_NOTIMPL
  // In that case, we use CD quality format (44.1kHz 16-bit stereo) per
  // Microsoft sample Force format to 48kHz Stereo Float 32-bit We use
  // AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM, so Windows handles the conversion from
  // the app's actual format (e.g. 44.1k int16) to our requested format.

  if (pwfx) {
    CoTaskMemFree(pwfx);
    pwfx = nullptr;
  }

  // KSDATAFORMAT_SUBTYPE_IEEE_FLOAT
  static const GUID KSDATAFORMAT_SUBTYPE_IEEE_FLOAT = {
      0x00000003,
      0x0000,
      0x0010,
      {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

  pwfx = (WAVEFORMATEX *)CoTaskMemAlloc(sizeof(WAVEFORMATEXTENSIBLE));
  if (!pwfx) {
    DLL_Log("[AppAudioCapture] Failed to allocate format");
    CleanupCapture();
    return false;
  }

  WAVEFORMATEXTENSIBLE *wfex = (WAVEFORMATEXTENSIBLE *)pwfx;
  wfex->Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
  wfex->Format.nChannels = 2;
  wfex->Format.nSamplesPerSec = 48000;
  wfex->Format.wBitsPerSample = 32;
  wfex->Format.nBlockAlign =
      wfex->Format.nChannels * wfex->Format.wBitsPerSample / 8;
  wfex->Format.nAvgBytesPerSec =
      wfex->Format.nSamplesPerSec * wfex->Format.nBlockAlign;
  wfex->Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
  wfex->Samples.wValidBitsPerSample = 32;
  wfex->dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
  wfex->SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;

  // Initialize audio client - per Microsoft sample, use LOOPBACK +
  // AUTOCONVERTPCM AUTOCONVERTPCM tells Windows to convert the process audio to
  // our format Use 10ms buffer (100000 hns) to reduce latency and burstiness
  hr = pAudioClient->Initialize(
      AUDCLNT_SHAREMODE_SHARED,
      AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 100000,
      AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM, // Auto-convert to our format
      pwfx, nullptr);
  if (FAILED(hr)) {
    // Try without EVENTCALLBACK
    DLL_Log("[AppAudioCapture] Initialize with EVENTCALLBACK failed: 0x%x, "
            "trying without",
            hr);
    hr = pAudioClient->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                  AUDCLNT_STREAMFLAGS_LOOPBACK |
                                      AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM,
                                  2000000, 0, pwfx, nullptr);
    if (FAILED(hr)) {
      // Try without any special flags
      DLL_Log("[AppAudioCapture] Initialize with LOOPBACK failed: 0x%x, trying "
              "plain",
              hr);
      hr = pAudioClient->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 2000000, 0,
                                    pwfx, nullptr);
      if (FAILED(hr)) {
        DLL_Log("[AppAudioCapture] Initialize failed: 0x%x", hr);
        CleanupCapture();
        return false;
      }
    }
  }

  // Get capture client
  hr = pAudioClient->GetService(__uuidof(IAudioCaptureClient),
                                reinterpret_cast<void **>(&pCaptureClient));
  if (FAILED(hr)) {
    DLL_Log("[AppAudioCapture] GetService IAudioCaptureClient failed: 0x%x",
            hr);
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

  DLL_Log("[AppAudioCapture] Started: PID=%lu channels=%d rate=%d bits=%d", pid,
          pwfx->nChannels, pwfx->nSamplesPerSec, pwfx->wBitsPerSample);

  // Clear any stale packets
  {
    std::lock_guard<std::mutex> lock(queueMutex);
    packetQueue.clear();
  }

  isCapturing.store(true);
  captureThread = std::thread(&AppAudioCapture::CaptureLoop, this);

  return true;
}

void AppAudioCapture::CleanupCapture() {
  if (pAudioClient) {
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
}

void AppAudioCapture::CaptureLoop() {
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  DLL_Log("[AppAudioCapture] Capture loop started for PID %lu",
          targetPID.load());

  UINT32 packetLength = 0;
  HRESULT hr;
  BYTE *pData;
  UINT32 numFramesAvailable;
  DWORD flags;
  UINT64 devicePosition;

  UINT64 qpcPosition;
  uint64_t lastQpcPosition = 0; // Track QPC for synthesis continuity

  // Debug: Drift tracking variables (non-static to support multiple instances)
  uint64_t firstDevicePos = 0;
  uint64_t firstQpcPos = 0;
  bool firstSet = false;
  int logCounter = 0;

  while (isCapturing.load() && !shouldStop.load()) {
    // Check if target process is still running
    if (!IsProcessRunning(targetPID.load())) {
      DLL_Log("[AppAudioCapture] Target process %lu exited", targetPID.load());
      break;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    hr = pCaptureClient->GetNextPacketSize(&packetLength);
    if (FAILED(hr)) {
      static int errCount = 0;
      if (errCount++ < 5) {
        DLL_Log("[AppAudioCapture] GetNextPacketSize failed: 0x%x", hr);
      }

      // If error, also sleep and synth silence? No, usually fatal or transient.
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
      continue;
    }

    // Heartbeat for silence synthesis - use high-resolution timer
    // NOTE: These must be non-static to support multiple AppAudioCapture
    // instances (e.g., one for game, one for Brave) - each needs independent
    // timing
    auto &lastRealTime = m_lastRealTime;
    int64_t &synthesizedMs = m_synthesizedMs;
    bool &heartbeatInit = m_heartbeatInit;

    if (!heartbeatInit) {
      lastRealTime = std::chrono::steady_clock::now();
      synthesizedMs = 0;
      heartbeatInit = true;
    }

    if (packetLength == 0) {
      // WASAPI Process Loopback stops sending packets during silence.
      // We must synthesize silence to keep the AudioEncoder pipeline alive.
      // Otherwise, large gaps accumulate and trigger the "Gap Too Large" cap
      // logic later.

      // HIGH-PRECISION TIMING: Calculate actual elapsed time since last real
      // packet
      auto now = std::chrono::steady_clock::now();
      auto realElapsedMs =
          std::chrono::duration_cast<std::chrono::milliseconds>(now -
                                                                lastRealTime)
              .count();

      // Synthesize silence to catch up to real time (in 20ms chunks)
      while (synthesizedMs + 20 <= realElapsedMs) {
        // Use pwfx if available, otherwise defaults
        int sampleRate = 48000;
        int channels = 2;
        int bitsPerSample = 32;
        int blockAlign = 8;
        bool isFloat = true;
        int validBits = 32;

        if (pwfx) {
          sampleRate = pwfx->nSamplesPerSec;
          channels = pwfx->nChannels;
          bitsPerSample = pwfx->wBitsPerSample;
          blockAlign = pwfx->nBlockAlign;

          if (pwfx->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
            isFloat = true;
          } else if (pwfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
            WAVEFORMATEXTENSIBLE *wfex =
                reinterpret_cast<WAVEFORMATEXTENSIBLE *>(pwfx);
            if (IsIEEEFloat(wfex->SubFormat))
              isFloat = true;
            else
              isFloat = false;
          } else {
            isFloat = false;
          }
          validBits = bitsPerSample; // simplified
        }

        int samples = sampleRate / 50; // 20ms
        int bytesPerSample = bitsPerSample / 8;
        // If blockAlign is set, use it for safer size calc
        if (blockAlign == 0)
          blockAlign = channels * bytesPerSample;

        size_t byteCount = samples * blockAlign;

        AudioPacket silencePacket;

        // CRITICAL: Ensure QPC continuity.
        // Don't just sample "Now", calculate strictly from previous position to
        // avoid jitter.
        LARGE_INTEGER freq;
        QueryPerformanceFrequency(&freq);

        // Valid packet updates lastQpcPosition. If we are pure synthesis
        // (start), init it.
        if (lastQpcPosition == 0) {
          LARGE_INTEGER qpc;
          QueryPerformanceCounter(&qpc);
          lastQpcPosition = qpc.QuadPart;
        }

        // Advance logic QPC by 20ms
        uint64_t qpcIncrement = freq.QuadPart / 50;
        lastQpcPosition += qpcIncrement;

        silencePacket.timestamp = (lastQpcPosition * 1000) / freq.QuadPart;
        silencePacket.devicePosition = 0;
        silencePacket.qpcPosition = lastQpcPosition;

        silencePacket.data.resize(byteCount, 0); // Zeroed

        // Populate format info
        silencePacket.channels = channels;
        silencePacket.sampleRate = sampleRate;
        silencePacket.bitsPerSample = bitsPerSample;
        silencePacket.blockAlign = blockAlign;
        silencePacket.isFloat = isFloat;
        silencePacket.validBitsPerSample = validBits;

        {
          std::lock_guard<std::mutex> lock(queueMutex);
          packetQueue.push_back(std::move(silencePacket));
          // Log every ~1s (50 calls)
          static int logCounter = 0;
          if (logCounter++ % 50 == 0) {
            DLL_Log("[AppAudio] Synthesizing silence (source idle) QPC=%llu",
                    lastQpcPosition);
          }
        }

        // Advance synthesized time by 20ms
        synthesizedMs += 20;
      }

      std::this_thread::sleep_for(std::chrono::milliseconds(1));
      continue;
    }

    // Valid packet received, reset heartbeat for silence synthesis
    lastRealTime = std::chrono::steady_clock::now();
    synthesizedMs = 0;
    heartbeatInit = true;

    while (packetLength != 0 && isCapturing.load()) {
      hr = pCaptureClient->GetBuffer(&pData, &numFramesAvailable, &flags,
                                     &devicePosition, &qpcPosition);
      if (FAILED(hr))
        break;

      // Debug: Check drift
      if (!firstSet && devicePosition > 0) {
        firstDevicePos = devicePosition;
        firstQpcPos = qpcPosition;
        firstSet = true;
        // Use targetProcessName/PID to identify the source in logs
        DLL_Log(
            "[AppAudioCapture] Source Sync Start (%lu): DevPos=%llu QPC=%llu",
            targetPID.load(), firstDevicePos, firstQpcPos);
      } else if (firstSet && logCounter++ % 500 == 0) { // Log every ~5 seconds
        LARGE_INTEGER freq;
        QueryPerformanceFrequency(&freq);

        double samplesDuration =
            (double)(devicePosition - firstDevicePos) / pwfx->nSamplesPerSec;
        double qpcDuration =
            (double)(qpcPosition - firstQpcPos) / freq.QuadPart;
        double driftMs = (samplesDuration - qpcDuration) * 1000.0;

        DLL_Log("[AppAudioCapture] Source Sync (%lu): Duration Samples=%.4fs, "
                "QPC=%.4fs, Drift=%.2f ms (%.4f%%)",
                targetPID.load(), samplesDuration, qpcDuration, driftMs,
                qpcDuration > 0 ? (driftMs / (qpcDuration * 1000.0) * 100.0)
                                : 0.0);
      }

      // Build packet with format info
      AudioPacket packet;
      packet.channels = pwfx->nChannels;
      packet.sampleRate = pwfx->nSamplesPerSec;
      packet.bitsPerSample = pwfx->wBitsPerSample;
      packet.blockAlign = pwfx->nBlockAlign;
      packet.blockAlign = pwfx->nBlockAlign;
      packet.validBitsPerSample = 0;
      packet.devicePosition = devicePosition; // Store for debug drift analysis
      packet.qpcPosition = qpcPosition;       // Store for debug drift analysis

      // CRITICAL: Update lastQpcPosition for synthesis continuity
      lastQpcPosition = qpcPosition;

      // Check for float format
      packet.isFloat = false;
      if (pwfx->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
        packet.isFloat = true;
      } else if (pwfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        WAVEFORMATEXTENSIBLE *wfex =
            reinterpret_cast<WAVEFORMATEXTENSIBLE *>(pwfx);
        if (IsIEEEFloat(wfex->SubFormat)) {
          packet.isFloat = true;
        }
        packet.validBitsPerSample = wfex->Samples.wValidBitsPerSample;
      }

      // Convert QPC to MS
      LARGE_INTEGER qpc;
      qpc.QuadPart = qpcPosition;
      LARGE_INTEGER freq;
      QueryPerformanceFrequency(&freq);
      packet.timestamp = (qpc.QuadPart * 1000) / freq.QuadPart;

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
        packetQueue.push_back(packet);
      }

      pCaptureClient->ReleaseBuffer(numFramesAvailable);
      hr = pCaptureClient->GetNextPacketSize(&packetLength);
      if (FAILED(hr))
        break;
    }
  }

  DLL_Log("[AppAudioCapture] Capture loop exited");
  isCapturing.store(false);
  CoUninitialize();
}

void AppAudioCapture::ProcessMonitorLoop() {
  DLL_Log("[AppAudioCapture] Monitor loop started for '%s'",
          targetProcessName.c_str());

  while (isMonitoring.load() && !shouldStop.load()) {
    // Check if we're already capturing
    if (isCapturing.load()) {
      // Check if target process is still running
      DWORD pid = targetPID.load();
      if (pid != 0 && !IsProcessRunning(pid)) {
        DLL_Log("[AppAudioCapture] Target process %lu exited, stopping capture",
                pid);
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
        DLL_Log("[AppAudioCapture] Found process '%s' with PID %lu",
                targetProcessName.c_str(), pid);
        targetPID.store(pid);
        if (InitializeCaptureForPID(pid)) {
          DLL_Log("[AppAudioCapture] Started capture for discovered process");
        } else {
          DLL_Log("[AppAudioCapture] Failed to start capture for PID %lu", pid);
          targetPID.store(0);
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

DWORD AppAudioCapture::FindProcessByName(const std::string &name) {
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
      WideCharToMultiByte(CP_UTF8, 0, pe32.szExeFile, -1, exeName, MAX_PATH,
                          nullptr, nullptr);

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
