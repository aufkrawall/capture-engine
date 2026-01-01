#include "audio_capture.h"
#include "mediaengine.h" // For DLL_Log
#include <iostream>

#define REFTIMES_PER_SEC 10000000
#define REFTIMES_PER_MILLISEC 10000

const CLSID CLSID_MMDeviceEnumerator = __uuidof(MMDeviceEnumerator);
const IID IID_IMMDeviceEnumerator = __uuidof(IMMDeviceEnumerator);
const IID IID_IAudioClient = __uuidof(IAudioClient);
const IID IID_IAudioCaptureClient = __uuidof(IAudioCaptureClient);

// IEEE Float subformat GUID: {00000003-0000-0010-8000-00aa00389b71}
static bool IsIEEEFloat(const GUID &g) {
  return g.Data1 == 0x00000003 && g.Data2 == 0x0000 && g.Data3 == 0x0010 &&
         g.Data4[0] == 0x80 && g.Data4[1] == 0x00 && g.Data4[2] == 0x00 &&
         g.Data4[3] == 0xaa && g.Data4[4] == 0x00 && g.Data4[5] == 0x38 &&
         g.Data4[6] == 0x9b && g.Data4[7] == 0x71;
}

AudioCapture::AudioCapture()
    : pEnumerator(NULL), pDevice(NULL), pAudioClient(NULL),
      pCaptureClient(NULL), pwfx(NULL), isCapturing(false) {}

AudioCapture::~AudioCapture() { Stop(); }

bool AudioCapture::Start(const std::string &deviceId, bool isLoopback) {
  DLL_Log("[AudioCapture] Start called: deviceId=%s loopback=%d",
          deviceId.empty() ? "default" : deviceId.c_str(), isLoopback);
  HRESULT hr;

  // Clear any stale packets from previous session
  {
    std::lock_guard<std::mutex> lock(queueMutex);
    packetQueue.clear();
  }

  hr = CoInitializeEx(
      NULL,
      COINIT_MULTITHREADED); // Or apartment threaded depends on calling thread?
  // Usually standard CoInit is fine.

  hr = CoCreateInstance(CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
                        IID_IMMDeviceEnumerator, (void **)&pEnumerator);
  if (FAILED(hr))
    return false;

  if (deviceId.empty()) {
    // For loopback (system audio), we capture from render device
    // For microphone, we capture from capture device
    EDataFlow dataFlow = isLoopback ? eRender : eCapture;
    hr = pEnumerator->GetDefaultAudioEndpoint(dataFlow, eConsole, &pDevice);
    DLL_Log("[AudioCapture] Using default %s endpoint",
            isLoopback ? "render (loopback)" : "capture (microphone)");
  } else {
    // Find device by ID or name
    // For now, try to get default based on data flow
    EDataFlow dataFlow = isLoopback ? eRender : eCapture;
    hr = pEnumerator->GetDefaultAudioEndpoint(dataFlow, eConsole, &pDevice);
    DLL_Log(
        "[AudioCapture] Using default %s endpoint (deviceId ignored for now)",
        isLoopback ? "render" : "capture");
  }
  if (FAILED(hr)) {
    DLL_Log("[AudioCapture] GetDefaultAudioEndpoint failed: 0x%x", hr);
    return false;
  }

  hr = pDevice->Activate(IID_IAudioClient, CLSCTX_ALL, NULL,
                         (void **)&pAudioClient);
  if (FAILED(hr)) {
    DLL_Log("[AudioCapture] Activate IAudioClient failed: 0x%x", hr);
    return false;
  }

  hr = pAudioClient->GetMixFormat(&pwfx);
  if (FAILED(hr)) {
    DLL_Log("[AudioCapture] GetMixFormat failed: 0x%x", hr);
    return false;
  }

  // Adjust format if needed? Usually we take what we get and resample later.

  // LOOPBACK flag only applies to render devices (system audio capture)
  // For capture devices (microphone), we don't use LOOPBACK
  DWORD flags = 0;
  if (isLoopback)
    flags |= AUDCLNT_STREAMFLAGS_LOOPBACK;

  hr = pAudioClient->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 10000000, 0,
                                pwfx, NULL); // 1 sec buffer
  if (FAILED(hr)) {
    DLL_Log("[AudioCapture] Initialize failed: 0x%x (flags=0x%x)", hr, flags);
    return false;
  }

  hr = pAudioClient->GetService(IID_IAudioCaptureClient,
                                (void **)&pCaptureClient);
  if (FAILED(hr))
    return false;

  hr = pAudioClient->Start();
  if (FAILED(hr)) {
    DLL_Log("[AudioCapture] pAudioClient->Start() failed: 0x%x", hr);
    return false;
  }

  DLL_Log("[AudioCapture] Started: channels=%d rate=%d bits=%d",
          pwfx->nChannels, pwfx->nSamplesPerSec, pwfx->wBitsPerSample);

  isCapturing = true;
  captureThread = std::thread(&AudioCapture::CaptureLoop, this);

  return true;
}

void AudioCapture::Stop() {
  isCapturing = false;
  if (captureThread.joinable())
    captureThread.join();

  if (pAudioClient)
    pAudioClient->Stop();

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
}

void AudioCapture::CaptureLoop() {
  CoInitializeEx(NULL, COINIT_MULTITHREADED);
  DLL_Log("[AudioCapture] CaptureLoop started");

  UINT32 packetLength = 0;
  HRESULT hr;
  BYTE *pData;
  UINT32 numFramesAvailable;
  DWORD flags;
  UINT64 devicePosition;

  UINT64 qpcPosition;

  // Debug: Drift tracking variables (non-static for per-instance tracking)
  uint64_t firstDevicePos = 0;
  uint64_t firstQpcPos = 0;
  bool firstSet = false;
  int logCounter = 0;

  while (isCapturing) {
    // Sleep for half buffer duration roughly?
    // Or just poll. Sleep 10ms is usually fine for audio.
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    hr = pCaptureClient->GetNextPacketSize(&packetLength);
    if (FAILED(hr)) {
      static int errCount = 0;
      if (errCount++ < 5) {
        DLL_Log("[AudioCapture] GetNextPacketSize failed: 0x%x", hr);
      }
      continue;
    }

    static int loopCount = 0;
    if (loopCount++ < 3 && packetLength > 0) {
      DLL_Log("[AudioCapture] Got packetLength=%u", packetLength);
    }

    while (packetLength != 0) {
      hr = pCaptureClient->GetBuffer(&pData, &numFramesAvailable, &flags,
                                     &devicePosition, &qpcPosition);
      if (FAILED(hr))
        break;

      // Debug: Check drift between Device Position (samples) and QPC (time)
      // devicePosition is cumulative frame count
      // qpcPosition is QPC timestamp at that position

      if (!firstSet && devicePosition > 0) {
        firstDevicePos = devicePosition;
        firstQpcPos = qpcPosition;
        firstSet = true;
        DLL_Log("[AudioCapture] Source Sync Start: DevPos=%llu QPC=%llu", firstDevicePos, firstQpcPos);
      } else if (firstSet && logCounter++ % 500 == 0) { // Log every ~5 seconds
         LARGE_INTEGER freq;
         QueryPerformanceFrequency(&freq);
         
         double samplesDuration = (double)(devicePosition - firstDevicePos) / pwfx->nSamplesPerSec;
         double qpcDuration = (double)(qpcPosition - firstQpcPos) / freq.QuadPart;
         double driftMs = (samplesDuration - qpcDuration) * 1000.0;
         
         DLL_Log("[AudioCapture] Source Sync: Duration Samples=%.4fs, QPC=%.4fs, Drift=%.2f ms (%.4f%%)", 
                 samplesDuration, qpcDuration, driftMs, (driftMs / (qpcDuration*1000.0) * 100.0));
      }

      // Build packet with format info
      AudioPacket packet;
      packet.channels = pwfx->nChannels;
      packet.sampleRate = pwfx->nSamplesPerSec;
      packet.bitsPerSample = pwfx->wBitsPerSample;
      packet.blockAlign = pwfx->nBlockAlign;
      packet.validBitsPerSample = 0; // Default: same as bitsPerSample
      packet.devicePosition = devicePosition; // Store for debugging if needed
      packet.qpcPosition = qpcPosition; // Store for debugging if needed

      // Check if float format and extract validBitsPerSample from
      // WAVEFORMATEXTENSIBLE
      packet.isFloat = false;
      if (pwfx->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
        packet.isFloat = true;
      } else if (pwfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        WAVEFORMATEXTENSIBLE *wfex = (WAVEFORMATEXTENSIBLE *)pwfx;
        if (IsIEEEFloat(wfex->SubFormat)) {
          packet.isFloat = true;
        }
        // Extract valid bits - important for 24-bit audio in 32-bit container
        packet.validBitsPerSample = wfex->Samples.wValidBitsPerSample;
      }

      // Convert QPC to MS
      LARGE_INTEGER qpc;
      qpc.QuadPart = qpcPosition;
      LARGE_INTEGER freq;
      QueryPerformanceFrequency(&freq);
      packet.timestamp = (qpc.QuadPart * 1000) / freq.QuadPart;

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
        packetQueue.push_back(packet);
      }

      pCaptureClient->ReleaseBuffer(numFramesAvailable);
      hr = pCaptureClient->GetNextPacketSize(&packetLength);
    }
  }

  DLL_Log("[AudioCapture] CaptureLoop exited");
  CoUninitialize();
}

bool AudioCapture::GetNextPacket(AudioPacket &packet) {
  std::lock_guard<std::mutex> lock(queueMutex);
  if (packetQueue.empty())
    return false;
  packet = packetQueue.front();
  packetQueue.pop_front(); // O(1) for deque vs O(n) for vector
  return true;
}
