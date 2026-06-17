#include "audio_latency_probe.h"

#include <audioclient.h>
#include <mmdeviceapi.h>
#include <windows.h>

#include <fstream>
#include <sstream>
#include <utility>
#include <vector>

#include "audio_time_utils.h"
#include "mediaengine.h"  // DLL_Log

// Render->loopback latency self-measurement (WASAPI orchestration). The pure DSP/cache/aggregation
// logic lives in audio_latency_probe.h and is unit-tested; this file only drives the audio hardware
// and is fail-safe (any failure / lack of consensus returns ok=false so the caller falls back to no
// correction rather than a guessed value). The endpoint latency is measured with SEVERAL
// independent shots and aggregated by median+consistency so a single noisy reading cannot
// over/under-correct - this is what lets the value be trusted automatically with no hand-set config.

namespace ce::audio {
namespace {

const CLSID kCLSID_MMDeviceEnumerator = __uuidof(MMDeviceEnumerator);
const IID kIID_IMMDeviceEnumerator = __uuidof(IMMDeviceEnumerator);
const IID kIID_IAudioClient = __uuidof(IAudioClient);
const IID kIID_IAudioRenderClient = __uuidof(IAudioRenderClient);
const IID kIID_IAudioCaptureClient = __uuidof(IAudioCaptureClient);
const IID kIID_IAudioClock = __uuidof(IAudioClock);

constexpr REFERENCE_TIME kRefPerSec = 10000000;  // 100-ns units per second

// Probe robustness: take several independent shots and require a consensus.
constexpr int kProbeShots = 5;
constexpr int kProbeMinAgreeingShots = 3;
constexpr double kProbeMaxSpreadMs = 8.0;  // ~half a 60 fps frame; well within the A/V floor

template <typename T>
void SafeRelease(T*& p) {
    if (p) {
        p->Release();
        p = nullptr;
    }
}

bool IsIEEEFloatFormat(const WAVEFORMATEX* wf) {
    if (!wf) {
        return false;
    }
    if (wf->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
        return true;
    }
    if (wf->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        const auto* wfex = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(wf);
        // KSDATAFORMAT_SUBTYPE_IEEE_FLOAT {00000003-0000-0010-8000-00aa00389b71}
        const GUID& g = wfex->SubFormat;
        return g.Data1 == 0x00000003 && g.Data2 == 0x0000 && g.Data3 == 0x0010 && g.Data4[0] == 0x80 &&
               g.Data4[1] == 0x00 && g.Data4[2] == 0x00 && g.Data4[3] == 0xaa && g.Data4[4] == 0x00 &&
               g.Data4[5] == 0x38 && g.Data4[6] == 0x9b && g.Data4[7] == 0x71;
    }
    return false;
}

std::string WideToUtf8(const wchar_t* w) {
    if (!w) {
        return std::string();
    }
    const int len = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 1) {
        return std::string();
    }
    std::string out(static_cast<size_t>(len - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, out.data(), len, nullptr, nullptr);
    return out;
}

std::string CacheFilePath(const std::string& cacheDir) {
    if (cacheDir.empty()) {
        return std::string();
    }
    std::string path = cacheDir;
    if (path.back() != '\\' && path.back() != '/') {
        path += '\\';
    }
    path += "audio_latency_cache.ini";
    return path;
}

bool ReadCacheFile(const std::string& path, std::vector<LatencyCacheEntry>& out) {
    if (path.empty()) {
        return false;
    }
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        return false;
    }
    std::stringstream ss;
    ss << f.rdbuf();
    return ParseLatencyCache(ss.str(), out);
}

void WriteCacheFile(const std::string& path, const std::vector<LatencyCacheEntry>& entries) {
    if (path.empty()) {
        return;
    }
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) {
        DLL_Log("[AudioLatencyProbe] WARNING: could not write cache file %s", path.c_str());
        return;
    }
    f << SerializeLatencyCache(entries);
}

// Downmix one interleaved float frame block to mono channel 0 (sufficient for narrowband
// detection; the marker is rendered into all channels).
void AppendMonoFromFloat(const float* interleaved, UINT32 frames, int channels, std::vector<float>& mono) {
    if (channels <= 0) {
        return;
    }
    for (UINT32 i = 0; i < frames; ++i) {
        mono.push_back(interleaved[static_cast<size_t>(i) * channels]);
    }
}

// One independent render->loopback latency measurement on `device`. Activates fresh render +
// loopback clients (an IAudioClient can only be Initialized once), plays the marker, captures the
// loopback, detects the marker center, and computes latency = loopback center QPC - render
// presentation-center QPC. Returns false (no value) on any error/inconclusive condition. Releases
// all interfaces it creates.
bool MeasureOnceMs(IMMDevice* device, const WAVEFORMATEX* mixFormat, const ProbeMarkerSpec& spec, int sampleRate,
                   int channels, double* outMs) {
    IAudioClient* renderClient = nullptr;
    IAudioRenderClient* renderService = nullptr;
    IAudioClock* renderClock = nullptr;
    IAudioClient* loopbackClient = nullptr;
    IAudioCaptureClient* captureService = nullptr;
    bool renderStarted = false;
    bool loopbackStarted = false;

    auto shotCleanup = [&]() {
        if (renderClient && renderStarted) {
            renderClient->Stop();
        }
        if (loopbackClient && loopbackStarted) {
            loopbackClient->Stop();
        }
        SafeRelease(captureService);
        SafeRelease(loopbackClient);
        SafeRelease(renderClock);
        SafeRelease(renderService);
        SafeRelease(renderClient);
    };

    const REFERENCE_TIME bufferDuration = static_cast<REFERENCE_TIME>(kRefPerSec * 1.0);  // 1 s

    HRESULT hr = device->Activate(kIID_IAudioClient, CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&renderClient));
    if (FAILED(hr) || !renderClient) {
        DLL_Log("[AudioLatencyProbe] Activate(render IAudioClient) failed: 0x%x", hr);
        shotCleanup();
        return false;
    }
    hr = renderClient->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, bufferDuration, 0, mixFormat, nullptr);
    if (FAILED(hr)) {
        DLL_Log("[AudioLatencyProbe] render Initialize failed: 0x%x", hr);
        shotCleanup();
        return false;
    }
    UINT32 renderBufferFrames = 0;
    if (FAILED(renderClient->GetBufferSize(&renderBufferFrames))) {
        shotCleanup();
        return false;
    }
    hr = renderClient->GetService(kIID_IAudioRenderClient, reinterpret_cast<void**>(&renderService));
    if (FAILED(hr) || !renderService) {
        DLL_Log("[AudioLatencyProbe] GetService(IAudioRenderClient) failed: 0x%x", hr);
        shotCleanup();
        return false;
    }
    hr = renderClient->GetService(kIID_IAudioClock, reinterpret_cast<void**>(&renderClock));
    if (FAILED(hr) || !renderClock) {
        DLL_Log("[AudioLatencyProbe] GetService(IAudioClock) failed: 0x%x", hr);
        shotCleanup();
        return false;
    }
    UINT64 clockFreq = 0;
    if (FAILED(renderClock->GetFrequency(&clockFreq)) || clockFreq == 0) {
        shotCleanup();
        return false;
    }

    hr = device->Activate(kIID_IAudioClient, CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&loopbackClient));
    if (FAILED(hr) || !loopbackClient) {
        DLL_Log("[AudioLatencyProbe] Activate(loopback IAudioClient) failed: 0x%x", hr);
        shotCleanup();
        return false;
    }
    hr = loopbackClient->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, bufferDuration, 0,
                                    mixFormat, nullptr);
    if (FAILED(hr)) {
        DLL_Log("[AudioLatencyProbe] loopback Initialize failed: 0x%x", hr);
        shotCleanup();
        return false;
    }
    hr = loopbackClient->GetService(kIID_IAudioCaptureClient, reinterpret_cast<void**>(&captureService));
    if (FAILED(hr) || !captureService) {
        DLL_Log("[AudioLatencyProbe] GetService(IAudioCaptureClient) failed: 0x%x", hr);
        shotCleanup();
        return false;
    }

    std::vector<float> markerMono;
    GenerateProbeMarkerMono(spec, markerMono);
    const UINT32 markerTotalFrames = static_cast<UINT32>(markerMono.size());
    if (markerTotalFrames == 0 || markerTotalFrames > renderBufferFrames) {
        shotCleanup();
        return false;
    }

    BYTE* renderBuf = nullptr;
    hr = renderService->GetBuffer(markerTotalFrames, &renderBuf);
    if (FAILED(hr) || !renderBuf) {
        DLL_Log("[AudioLatencyProbe] render GetBuffer failed: 0x%x", hr);
        shotCleanup();
        return false;
    }
    float* renderFloats = reinterpret_cast<float*>(renderBuf);
    for (UINT32 i = 0; i < markerTotalFrames; ++i) {
        const float s = markerMono[i];
        for (int c = 0; c < channels; ++c) {
            renderFloats[static_cast<size_t>(i) * channels + c] = s;
        }
    }
    renderService->ReleaseBuffer(markerTotalFrames, 0);

    // Start loopback first so the capture is already running when audio plays.
    if (FAILED(loopbackClient->Start())) {
        shotCleanup();
        return false;
    }
    loopbackStarted = true;
    if (FAILED(renderClient->Start())) {
        shotCleanup();
        return false;
    }
    renderStarted = true;

    // Capture loop: accumulate mono loopback samples and record (cumulativeFrame, qpc) per packet so
    // we can map the detected center frame to a QPC. Also sample the render clock to map render
    // stream frame 0 -> QPC. Bound the loop so a silent/failed endpoint cannot hang.
    std::vector<float> capturedMono;
    capturedMono.reserve(static_cast<size_t>(markerTotalFrames) * 2);
    std::vector<std::pair<size_t, uint64_t>> packetFrameQpc;  // (cumulativeMonoFrame, qpc100ns)
    uint64_t renderFrame0Qpc100ns = 0;
    bool haveRenderFrame0 = false;

    const double captureSeconds = static_cast<double>(spec.totalFrames()) / sampleRate + 0.5;  // margin
    const DWORD deadlineTick = GetTickCount() + static_cast<DWORD>(captureSeconds * 1000.0) + 500;
    const DWORD pollSleepMs = 5;

    while (GetTickCount() < deadlineTick) {
        if (!haveRenderFrame0) {
            UINT64 pos = 0, posQpc100ns = 0;
            if (SUCCEEDED(renderClock->GetPosition(&pos, &posQpc100ns)) && pos > 0 && posQpc100ns > 0) {
                const uint64_t framesPresented =
                    static_cast<uint64_t>((static_cast<double>(pos) / static_cast<double>(clockFreq)) * sampleRate);
                const uint64_t presented100ns = AudioFramesToHundredNanoseconds(framesPresented, sampleRate);
                if (posQpc100ns > presented100ns) {
                    renderFrame0Qpc100ns = posQpc100ns - presented100ns;
                    haveRenderFrame0 = true;
                }
            }
        }

        UINT32 packetFrames = 0;
        HRESULT pr = captureService->GetNextPacketSize(&packetFrames);
        if (FAILED(pr)) {
            break;
        }
        if (packetFrames == 0) {
            Sleep(pollSleepMs);
            continue;
        }
        BYTE* data = nullptr;
        UINT32 framesAvail = 0;
        DWORD flags = 0;
        UINT64 devPos = 0, qpcPos = 0;
        pr = captureService->GetBuffer(&data, &framesAvail, &flags, &devPos, &qpcPos);
        if (FAILED(pr)) {
            break;
        }
        const size_t baseFrame = capturedMono.size();
        if (qpcPos > 0) {
            packetFrameQpc.emplace_back(baseFrame, static_cast<uint64_t>(qpcPos));
        }
        if (data && framesAvail > 0 && (flags & AUDCLNT_BUFFERFLAGS_SILENT) == 0) {
            AppendMonoFromFloat(reinterpret_cast<const float*>(data), framesAvail, channels, capturedMono);
        } else if (framesAvail > 0) {
            capturedMono.insert(capturedMono.end(), framesAvail, 0.0f);
        }
        captureService->ReleaseBuffer(framesAvail);

        if (haveRenderFrame0 &&
            capturedMono.size() >= static_cast<size_t>(markerTotalFrames) + static_cast<size_t>(sampleRate / 2)) {
            break;
        }
    }

    if (!haveRenderFrame0 || packetFrameQpc.empty() || capturedMono.size() < static_cast<size_t>(spec.leadInFrames)) {
        shotCleanup();
        return false;
    }

    const int centerFrame =
        DetectMarkerCenterFrame(capturedMono.data(), capturedMono.size(), sampleRate, spec.markerFreqHz);
    if (centerFrame < 0) {
        shotCleanup();
        return false;
    }

    // Map the detected marker-center frame -> loopback QPC via the packet whose base frame is the
    // largest <= centerFrame.
    size_t pktIdx = 0;
    for (size_t i = 0; i < packetFrameQpc.size(); ++i) {
        if (packetFrameQpc[i].first <= static_cast<size_t>(centerFrame)) {
            pktIdx = i;
        } else {
            break;
        }
    }
    const size_t pktBaseFrame = packetFrameQpc[pktIdx].first;
    const uint64_t pktBaseQpc = packetFrameQpc[pktIdx].second;
    const uint64_t centerOffset100ns =
        AudioFramesToHundredNanoseconds(static_cast<uint64_t>(centerFrame) - pktBaseFrame, sampleRate);
    const uint64_t loopbackCenterQpc100ns = pktBaseQpc + centerOffset100ns;

    // Render presentation QPC of the marker CENTER (leadInFrames + markerFrames/2 into the stream).
    // Aligning centers cancels the Hann-window envelope on both sides.
    const uint64_t renderCenterFrame =
        static_cast<uint64_t>(spec.leadInFrames) + static_cast<uint64_t>(spec.markerFrames / 2);
    const uint64_t renderCenterQpc100ns =
        renderFrame0Qpc100ns + AudioFramesToHundredNanoseconds(renderCenterFrame, sampleRate);

    const double latencyMs = ComputeRenderLatencyMs(loopbackCenterQpc100ns, renderCenterQpc100ns);
    DLL_Log(
        "[AudioLatencyProbe] shot: centerFrame=%d (capFrames=%zu) loopbackCenterQpc=%llu renderCenterQpc=%llu "
        "renderFrame0Qpc=%llu latency=%.3f ms",
        centerFrame, capturedMono.size(), static_cast<unsigned long long>(loopbackCenterQpc100ns),
        static_cast<unsigned long long>(renderCenterQpc100ns), static_cast<unsigned long long>(renderFrame0Qpc100ns),
        latencyMs);

    if (!IsPlausibleLatencyMs(latencyMs)) {
        shotCleanup();
        return false;  // outlier shot discarded; the median gate handles the rest
    }

    if (outMs) {
        *outMs = latencyMs;
    }
    shotCleanup();
    return true;
}

}  // namespace

RenderLatencyProbeResult MeasureRenderEndpointLatency(const std::string& cacheDir, bool forceRemeasure) {
    RenderLatencyProbeResult result;

    HRESULT hrInit = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool coInitOwned = SUCCEEDED(hrInit);
    if (hrInit == RPC_E_CHANGED_MODE) {
        // Already initialized in a different model on this thread; fine, just must not uninitialize.
        hrInit = S_OK;
    } else if (FAILED(hrInit)) {
        DLL_Log("[AudioLatencyProbe] CoInitializeEx failed: 0x%x", hrInit);
        return result;
    }

    IMMDeviceEnumerator* enumerator = nullptr;
    IMMDevice* device = nullptr;
    IAudioClient* probeClient = nullptr;  // transient, only for GetMixFormat
    WAVEFORMATEX* mixFormat = nullptr;
    wchar_t* deviceIdW = nullptr;

    auto cleanup = [&]() {
        SafeRelease(probeClient);
        SafeRelease(device);
        SafeRelease(enumerator);
        if (mixFormat) {
            CoTaskMemFree(mixFormat);
            mixFormat = nullptr;
        }
        if (deviceIdW) {
            CoTaskMemFree(deviceIdW);
            deviceIdW = nullptr;
        }
        if (coInitOwned) {
            CoUninitialize();
        }
    };

    HRESULT hr = CoCreateInstance(kCLSID_MMDeviceEnumerator, nullptr, CLSCTX_ALL, kIID_IMMDeviceEnumerator,
                                  reinterpret_cast<void**>(&enumerator));
    if (FAILED(hr) || !enumerator) {
        DLL_Log("[AudioLatencyProbe] CoCreateInstance(MMDeviceEnumerator) failed: 0x%x", hr);
        cleanup();
        return result;
    }

    hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
    if (FAILED(hr) || !device) {
        DLL_Log("[AudioLatencyProbe] GetDefaultAudioEndpoint(eRender) failed: 0x%x", hr);
        cleanup();
        return result;
    }

    if (SUCCEEDED(device->GetId(&deviceIdW))) {
        result.deviceKey = WideToUtf8(deviceIdW);
    }

    hr = device->Activate(kIID_IAudioClient, CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&probeClient));
    if (FAILED(hr) || !probeClient) {
        DLL_Log("[AudioLatencyProbe] Activate(render IAudioClient) failed: 0x%x", hr);
        cleanup();
        return result;
    }
    hr = probeClient->GetMixFormat(&mixFormat);
    if (FAILED(hr) || !mixFormat) {
        DLL_Log("[AudioLatencyProbe] GetMixFormat failed: 0x%x", hr);
        cleanup();
        return result;
    }

    const int sampleRate = static_cast<int>(mixFormat->nSamplesPerSec);
    const int channels = static_cast<int>(mixFormat->nChannels);
    result.sampleRate = sampleRate;
    result.channels = channels;
    const std::string deviceKey = MakeRenderEndpointCacheKey(result.deviceKey, sampleRate, channels);
    result.deviceKey = deviceKey;

    // Cache lookup (unless forced).
    const std::string cachePath = CacheFilePath(cacheDir);
    std::vector<LatencyCacheEntry> cache;
    ReadCacheFile(cachePath, cache);
    if (!forceRemeasure) {
        double cached = 0.0;
        if (LookupLatencyCache(cache, deviceKey, &cached) && IsPlausibleLatencyMs(cached)) {
            DLL_Log("[AudioLatencyProbe] Cache hit: key=%s latency=%.3f ms (no probe needed)", deviceKey.c_str(),
                    cached);
            result.ok = true;
            result.fromCache = true;
            result.latencyMs = cached;
            cleanup();
            return result;
        }
    }

    if (!IsIEEEFloatFormat(mixFormat)) {
        DLL_Log("[AudioLatencyProbe] Render mix format is not IEEE float (tag=%u bits=%u); skipping probe",
                static_cast<unsigned>(mixFormat->wFormatTag), static_cast<unsigned>(mixFormat->wBitsPerSample));
        cleanup();
        return result;
    }

    const ProbeMarkerSpec spec = ResolveProbeMarkerSpec(sampleRate);
    if (!spec.valid) {
        DLL_Log("[AudioLatencyProbe] No usable marker for rate=%d; skipping probe", sampleRate);
        cleanup();
        return result;
    }

    // probeClient was only needed for GetMixFormat; per-shot clients are activated fresh.
    SafeRelease(probeClient);

    DLL_Log(
        "[AudioLatencyProbe] Probing: key=%s rate=%d ch=%d markerFreq=%.0fHz amp=%.3f frames(lead=%d marker=%d "
        "tail=%d) shots=%d",
        deviceKey.c_str(), sampleRate, channels, spec.markerFreqHz, spec.amplitude, spec.leadInFrames,
        spec.markerFrames, spec.tailFrames, kProbeShots);

    std::vector<double> shots;
    shots.reserve(kProbeShots);
    for (int i = 0; i < kProbeShots; ++i) {
        double ms = 0.0;
        if (MeasureOnceMs(device, mixFormat, spec, sampleRate, channels, &ms)) {
            shots.push_back(ms);
        }
    }

    const MedianLatencyResult agg = MedianWithConsistency(shots, kProbeMinAgreeingShots, kProbeMaxSpreadMs);
    if (!agg.ok) {
        DLL_Log(
            "[AudioLatencyProbe] no consensus (%zu/%d shots, need %d within %.1f ms); auto-detect unavailable, "
            "falling back to no correction",
            shots.size(), kProbeShots, kProbeMinAgreeingShots, kProbeMaxSpreadMs);
        cleanup();
        return result;
    }

    result.ok = true;
    result.measured = true;
    result.latencyMs = agg.latencyMs;

    UpsertLatencyCache(cache, deviceKey, agg.latencyMs);
    WriteCacheFile(cachePath, cache);
    DLL_Log("[AudioLatencyProbe] Measured (median of %d/%d agreeing shots): key=%s latency=%.3f ms -> %s",
            agg.agreeingCount, kProbeShots, deviceKey.c_str(), agg.latencyMs,
            cachePath.empty() ? "<no cache>" : cachePath.c_str());

    cleanup();
    return result;
}

}  // namespace ce::audio
