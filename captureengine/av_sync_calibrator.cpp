#include "av_sync_calibrator.h"

// clang-format off
#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <d3d11.h>
// clang-format on

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "../common/logging.h"
#include "../mediaengine/av_sync_calibration.h"  // pure logic (also pulls audio_latency_probe.h)
#include "wgc_capture.h"

// A/V self-calibration orchestration. Emits, at the same instant, a near-inaudible audio burst (to
// the default render endpoint, captured via loopback) and a white flash on a small calibration
// window (captured via WGC), N times. Because both fire together, the difference of their CAPTURED
// center timestamps is the pure A/V content offset (the trigger instant cancels). Runtime-only; it
// can only be validated on hardware. Fail-safe: any failure -> ok=false.

namespace ce::avcal {
namespace {
using namespace ce::audio;

constexpr int kRounds = 8;              // flash/burst rounds
constexpr int kFlashMs = 60;            // white + tone duration per round
constexpr int kGapMs = 200;             // dark/silent gap between rounds
constexpr int kWarmupMs = 400;          // let WGC + audio settle before the first round
constexpr int kTailMs = 400;            // keep draining after the last round
constexpr int kCalibWindowPx = 260;     // small on-screen calibration window
constexpr double kRenderFillMs = 12.0;  // shallow render top-up target (minimize buffered-ahead)
constexpr double kMaxAbsOffsetMs = 250.0;

const CLSID kCLSID_MMDeviceEnumerator = __uuidof(MMDeviceEnumerator);
const IID kIID_IMMDeviceEnumerator = __uuidof(IMMDeviceEnumerator);
const IID kIID_IAudioClient = __uuidof(IAudioClient);
const IID kIID_IAudioRenderClient = __uuidof(IAudioRenderClient);
const IID kIID_IAudioCaptureClient = __uuidof(IAudioCaptureClient);

template <typename T>
void SafeRelease(T*& p) {
    if (p) {
        p->Release();
        p = nullptr;
    }
}

bool IsFloatFormat(const WAVEFORMATEX* wf) {
    if (!wf)
        return false;
    if (wf->wFormatTag == WAVE_FORMAT_IEEE_FLOAT)
        return true;
    if (wf->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        const auto* x = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(wf);
        const GUID& g = x->SubFormat;
        return g.Data1 == 0x00000003 && g.Data2 == 0x0000 && g.Data3 == 0x0010 && g.Data4[0] == 0x80 &&
               g.Data4[1] == 0x00 && g.Data4[2] == 0x00 && g.Data4[3] == 0xaa && g.Data4[4] == 0x00 &&
               g.Data4[5] == 0x38 && g.Data4[6] == 0x9b && g.Data4[7] == 0x71;
    }
    return false;
}

std::string WideToUtf8(const wchar_t* w) {
    if (!w)
        return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1)
        return {};
    std::string s(static_cast<size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

std::string CachePath(const std::string& dir) {
    if (dir.empty())
        return {};
    std::string p = dir;
    const char b = p.empty() ? '\0' : p.back();
    if (b != '\\' && b != '/')
        p += '\\';
    p += "audio_latency_cache.ini";
    return p;
}

// ---- calibration window ------------------------------------------------------------------------

const char* kCalibClass = "CECalibWindow";
bool g_calibWhite = false;  // current calibration window color (single-threaded use)

LRESULT CALLBACK CalibWndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT r;
        GetClientRect(h, &r);
        HBRUSH br = CreateSolidBrush(g_calibWhite ? RGB(255, 255, 255) : RGB(0, 0, 0));
        FillRect(dc, &r, br);
        DeleteObject(br);
        EndPaint(h, &ps);
        return 0;
    }
    return DefWindowProcA(h, m, w, l);
}

HWND CreateCalibWindow() {
    WNDCLASSA wc = {};
    wc.lpfnWndProc = CalibWndProc;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = kCalibClass;
    wc.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    if (!RegisterClassA(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        LogWarn("[AVSyncCalib] RegisterClass failed: %lu", GetLastError());
        return nullptr;
    }
    HWND hwnd =
        CreateWindowExA(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kCalibClass, "ce_avsync_calib", WS_POPUP,
                        40, 40, kCalibWindowPx, kCalibWindowPx, nullptr, nullptr, GetModuleHandleA(nullptr), nullptr);
    if (!hwnd) {
        LogWarn("[AVSyncCalib] CreateWindowEx failed: %lu", GetLastError());
        return nullptr;
    }
    ShowWindow(hwnd, SW_SHOWNA);
    UpdateWindow(hwnd);
    return hwnd;
}

void FillWindow(HWND hwnd, bool white) {
    // Paint via WM_PAINT (BeginPaint/EndPaint) so the DWM redirection surface that WGC captures is
    // updated; drawing to GetDC() does not reliably update what WGC sees. RDW_UPDATENOW forces a
    // synchronous repaint before we continue.
    g_calibWhite = white;
    InvalidateRect(hwnd, nullptr, FALSE);
    RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
}

void PumpMessages() {
    MSG msg;
    while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
}

// ---- WGC frame luma readback -------------------------------------------------------------------

float HalfToFloat(uint16_t h) {
    const uint32_t sign = (h & 0x8000u) << 16;
    const uint32_t exp = (h & 0x7C00u) >> 10;
    const uint32_t man = (h & 0x03FFu);
    uint32_t f;
    if (exp == 0) {
        if (man == 0) {
            f = sign;
        } else {
            int e = -1;
            uint32_t m = man;
            do {
                e++;
                m <<= 1;
            } while ((m & 0x0400u) == 0);
            m &= 0x03FFu;
            f = sign | ((112 - e) << 23) | (m << 13);
        }
    } else if (exp == 0x1F) {
        f = sign | 0x7F800000u | (man << 13);
    } else {
        f = sign | ((exp + 112) << 23) | (man << 13);
    }
    float out;
    memcpy(&out, &f, sizeof(out));
    return out;
}

// Sample the center pixel of `tex` and return a normalized 0..1 brightness proxy. Handles the
// common WGC formats (BGRA8/RGBA8, FP16, R10G10B10A2); for anything else, returns a monotonic
// byte-sum proxy (white still >> black). Reuses/recreates `staging` (1x1) on the texture's own
// device. Returns <0 on failure.
double ReadCenterLuma(ID3D11Texture2D* tex, ID3D11Texture2D*& staging, ID3D11Device*& stagingDevice,
                      DXGI_FORMAT& stagingFormat) {
    if (!tex)
        return -1.0;
    D3D11_TEXTURE2D_DESC desc = {};
    tex->GetDesc(&desc);
    ID3D11Device* dev = nullptr;
    tex->GetDevice(&dev);
    if (!dev)
        return -1.0;
    ID3D11DeviceContext* ctx = nullptr;
    dev->GetImmediateContext(&ctx);
    if (!ctx) {
        dev->Release();
        return -1.0;
    }

    double luma = -1.0;
    // (Re)create a 1x1 staging texture matching the source format/device.
    if (!staging || stagingDevice != dev || stagingFormat != desc.Format) {
        SafeRelease(staging);
        D3D11_TEXTURE2D_DESC sd = {};
        sd.Width = 1;
        sd.Height = 1;
        sd.MipLevels = 1;
        sd.ArraySize = 1;
        sd.Format = desc.Format;
        sd.SampleDesc.Count = 1;
        sd.Usage = D3D11_USAGE_STAGING;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(dev->CreateTexture2D(&sd, nullptr, &staging)) || !staging) {
            LogWarn("[AVSyncCalib] staging CreateTexture2D failed (fmt=%d)", static_cast<int>(desc.Format));
            ctx->Release();
            dev->Release();
            return -1.0;
        }
        stagingDevice = dev;
        stagingFormat = desc.Format;
    }

    const UINT cx = desc.Width / 2;
    const UINT cy = desc.Height / 2;
    D3D11_BOX box = {cx, cy, 0, cx + 1, cy + 1, 1};
    ctx->CopySubresourceRegion(staging, 0, 0, 0, 0, tex, 0, &box);
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (SUCCEEDED(ctx->Map(staging, 0, D3D11_MAP_READ, 0, &mapped)) && mapped.pData) {
        const uint8_t* p = static_cast<const uint8_t*>(mapped.pData);
        switch (desc.Format) {
            case DXGI_FORMAT_B8G8R8A8_UNORM:
            case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
            case DXGI_FORMAT_R8G8B8A8_UNORM:
            case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: {
                luma = (static_cast<double>(p[0]) + p[1] + p[2]) / (3.0 * 255.0);
                break;
            }
            case DXGI_FORMAT_R16G16B16A16_FLOAT: {
                const uint16_t* h = reinterpret_cast<const uint16_t*>(p);
                const double r = HalfToFloat(h[0]), g = HalfToFloat(h[1]), b = HalfToFloat(h[2]);
                luma = std::clamp((r + g + b) / 3.0, 0.0, 1.0);
                break;
            }
            case DXGI_FORMAT_R10G10B10A2_UNORM: {
                uint32_t v;
                memcpy(&v, p, sizeof(v));
                const double r = (v & 0x3FF) / 1023.0, g = ((v >> 10) & 0x3FF) / 1023.0,
                             b = ((v >> 20) & 0x3FF) / 1023.0;
                luma = (r + g + b) / 3.0;
                break;
            }
            default: {
                // Monotonic fallback: sum first 4 bytes / max.
                luma = (static_cast<double>(p[0]) + p[1] + p[2] + p[3]) / (4.0 * 255.0);
                break;
            }
        }
        ctx->Unmap(staging, 0);
    }
    ctx->Release();
    dev->Release();
    return luma;
}

// Map an accumulated mono frame index to a QPC (100ns) using the per-packet (baseFrame, qpc) list.
uint64_t FrameToQpc100ns(const std::vector<std::pair<size_t, uint64_t>>& map, size_t frame, int sampleRate) {
    if (map.empty())
        return 0;
    size_t idx = 0;
    for (size_t i = 0; i < map.size(); ++i) {
        if (map[i].first <= frame)
            idx = i;
        else
            break;
    }
    return map[idx].second + AudioFramesToHundredNanoseconds(frame - map[idx].first, sampleRate);
}

}  // namespace

AvCalibrationResult MeasureAvOffset(ID3D11Device* d3dDevice, const std::string& cacheDir, bool forceRemeasure) {
    AvCalibrationResult result;
    if (!d3dDevice) {
        LogWarn("[AVSyncCalib] no D3D11 device; skipping A/V calibration");
        return result;
    }

    HRESULT hrInit = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool coOwned = SUCCEEDED(hrInit);
    if (hrInit == RPC_E_CHANGED_MODE)
        hrInit = S_OK;
    if (FAILED(hrInit)) {
        LogWarn("[AVSyncCalib] CoInitializeEx failed: 0x%lx", static_cast<unsigned long>(hrInit));
        return result;
    }

    IMMDeviceEnumerator* enumr = nullptr;
    IMMDevice* device = nullptr;
    IAudioClient* renderClient = nullptr;
    IAudioRenderClient* renderSvc = nullptr;
    IAudioClient* loopClient = nullptr;
    IAudioCaptureClient* capSvc = nullptr;
    WAVEFORMATEX* mix = nullptr;
    wchar_t* devIdW = nullptr;
    HWND wnd = nullptr;
    WGCCapture* wgc = nullptr;
    ID3D11Texture2D* staging = nullptr;
    ID3D11Device* stagingDevice = nullptr;
    DXGI_FORMAT stagingFormat = DXGI_FORMAT_UNKNOWN;

    auto cleanup = [&]() {
        if (renderClient)
            renderClient->Stop();
        if (loopClient)
            loopClient->Stop();
        if (wgc) {
            if (wgc->IsCapturing())
                wgc->StopCapture();
            delete wgc;
            wgc = nullptr;
        }
        if (wnd)
            DestroyWindow(wnd);
        SafeRelease(staging);
        SafeRelease(capSvc);
        SafeRelease(loopClient);
        SafeRelease(renderSvc);
        SafeRelease(renderClient);
        SafeRelease(device);
        SafeRelease(enumr);
        if (mix) {
            CoTaskMemFree(mix);
            mix = nullptr;
        }
        if (devIdW) {
            CoTaskMemFree(devIdW);
            devIdW = nullptr;
        }
        if (coOwned)
            CoUninitialize();
    };

    HRESULT hr = CoCreateInstance(kCLSID_MMDeviceEnumerator, nullptr, CLSCTX_ALL, kIID_IMMDeviceEnumerator,
                                  reinterpret_cast<void**>(&enumr));
    if (FAILED(hr) || !enumr || FAILED(enumr->GetDefaultAudioEndpoint(eRender, eConsole, &device)) || !device) {
        LogWarn("[AVSyncCalib] default render endpoint unavailable");
        cleanup();
        return result;
    }
    std::string devId;
    if (SUCCEEDED(device->GetId(&devIdW)))
        devId = WideToUtf8(devIdW);

    if (FAILED(device->Activate(kIID_IAudioClient, CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&renderClient))) ||
        FAILED(renderClient->GetMixFormat(&mix)) || !mix) {
        LogWarn("[AVSyncCalib] render mix format unavailable");
        cleanup();
        return result;
    }
    const int sampleRate = static_cast<int>(mix->nSamplesPerSec);
    const int channels = static_cast<int>(mix->nChannels);
    result.sampleRate = sampleRate;
    result.channels = channels;

    // Display geometry for the cache key (primary monitor resolution).
    const int dispW = GetSystemMetrics(SM_CXSCREEN);
    const int dispH = GetSystemMetrics(SM_CYSCREEN);
    const std::string key = MakeAvCalibrationCacheKey(devId, sampleRate, channels, dispW, dispH);
    result.key = key;

    const std::string cachePath = CachePath(cacheDir);
    std::vector<LatencyCacheEntry> cache;
    {
        std::ifstream f(cachePath, std::ios::binary);
        if (f) {
            std::stringstream ss;
            ss << f.rdbuf();
            ParseLatencyCache(ss.str(), cache);
        }
    }
    if (!forceRemeasure) {
        double cached = 0.0;
        if (LookupLatencyCache(cache, key, &cached) && IsPlausibleLatencyMs(cached)) {
            LogInfo("[AVSyncCalib] cache hit: key=%s offset=%.3f ms (no calibration needed)", key.c_str(), cached);
            result.ok = true;
            result.fromCache = true;
            result.offsetMs = cached;
            cleanup();
            return result;
        }
    }

    if (!IsFloatFormat(mix)) {
        LogWarn("[AVSyncCalib] render mix not float (tag=%u); skipping", static_cast<unsigned>(mix->wFormatTag));
        cleanup();
        return result;
    }
    const ProbeMarkerSpec spec = ResolveProbeMarkerSpec(sampleRate);
    if (!spec.valid) {
        LogWarn("[AVSyncCalib] no usable marker for rate=%d; skipping", sampleRate);
        cleanup();
        return result;
    }

    // Render: shared, ~100ms buffer. We keep it shallowly filled and inject a tone during bursts.
    const REFERENCE_TIME bufDur = static_cast<REFERENCE_TIME>(10000000.0 * 0.1);
    if (FAILED(renderClient->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, bufDur, 0, mix, nullptr)) ||
        FAILED(renderClient->GetService(kIID_IAudioRenderClient, reinterpret_cast<void**>(&renderSvc)))) {
        LogWarn("[AVSyncCalib] render init failed");
        cleanup();
        return result;
    }
    UINT32 renderBufFrames = 0;
    renderClient->GetBufferSize(&renderBufFrames);

    if (FAILED(device->Activate(kIID_IAudioClient, CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&loopClient))) ||
        FAILED(
            loopClient->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, bufDur, 0, mix, nullptr)) ||
        FAILED(loopClient->GetService(kIID_IAudioCaptureClient, reinterpret_cast<void**>(&capSvc)))) {
        LogWarn("[AVSyncCalib] loopback init failed");
        cleanup();
        return result;
    }

    wnd = CreateCalibWindow();
    if (!wnd) {
        cleanup();
        return result;
    }
    FillWindow(wnd, false);
    PumpMessages();

    wgc = new WGCCapture();
    if (!wgc->InitForWindow(d3dDevice, wnd) || !wgc->StartCapture()) {
        LogWarn("[AVSyncCalib] WGC InitForWindow/StartCapture failed");
        cleanup();
        return result;
    }

    LARGE_INTEGER qpf;
    QueryPerformanceFrequency(&qpf);
    const uint64_t qpcFreq = static_cast<uint64_t>(qpf.QuadPart);

    if (FAILED(loopClient->Start()) || FAILED(renderClient->Start())) {
        LogWarn("[AVSyncCalib] audio Start failed");
        cleanup();
        return result;
    }

    LogInfo("[AVSyncCalib] Calibrating: key=%s rate=%d ch=%d markerFreq=%.0fHz disp=%dx%d rounds=%d flashMs=%d",
            key.c_str(), sampleRate, channels, spec.markerFreqHz, dispW, dispH, kRounds, kFlashMs);

    // Accumulators.
    std::vector<float> mono;
    std::vector<std::pair<size_t, uint64_t>> pktMap;  // (cumulative mono frame, qpc100ns)
    std::vector<double> vidLuma;
    std::vector<uint64_t> vidQpc;  // 100ns

    const double w = 2.0 * 3.14159265358979323846 * spec.markerFreqHz / sampleRate;
    double phase = 0.0;
    const UINT32 fillTarget = static_cast<UINT32>(sampleRate * (kRenderFillMs / 1000.0));

    auto topUpRender = [&](bool burst) {
        UINT32 padding = 0;
        if (FAILED(renderClient->GetCurrentPadding(&padding)))
            return;
        if (padding >= fillTarget)
            return;
        const UINT32 toWrite = std::min(fillTarget - padding, renderBufFrames - padding);
        if (toWrite == 0)
            return;
        BYTE* buf = nullptr;
        if (FAILED(renderSvc->GetBuffer(toWrite, &buf)) || !buf)
            return;
        float* f = reinterpret_cast<float*>(buf);
        for (UINT32 i = 0; i < toWrite; ++i) {
            const float s = burst ? static_cast<float>(spec.amplitude * std::sin(phase)) : 0.0f;
            if (burst)
                phase += w;
            for (int c = 0; c < channels; ++c)
                f[static_cast<size_t>(i) * channels + c] = s;
        }
        renderSvc->ReleaseBuffer(toWrite, 0);
    };

    int vframeLog = 0;  // diagnostic: log the first N captured frames' luma
    auto drainWgc = [&]() {
        WGCCapturedFrame fr;
        int guard = 0;
        while (wgc->GetNextFrame(fr) && guard++ < 64) {
            const double luma = ReadCenterLuma(fr.texture, staging, stagingDevice, stagingFormat);
            if (luma >= 0.0) {
                const uint64_t q = RawQpcToHundredNanoseconds(static_cast<uint64_t>(fr.timestamp), qpcFreq);
                vidLuma.push_back(luma);
                vidQpc.push_back(q);
                if (vframeLog++ < 48) {
                    LogInfo("[AVSyncCalib] vframe luma=%.3f qpc=%llu", luma, static_cast<unsigned long long>(q));
                }
            }
            if (fr.texture)
                fr.texture->Release();
        }
    };

    auto drainAudio = [&]() {
        UINT32 pkt = 0;
        int guard = 0;
        while (SUCCEEDED(capSvc->GetNextPacketSize(&pkt)) && pkt > 0 && guard++ < 64) {
            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            UINT64 devpos = 0, qpc = 0;
            if (FAILED(capSvc->GetBuffer(&data, &frames, &flags, &devpos, &qpc)))
                break;
            const size_t base = mono.size();
            if (qpc > 0)
                pktMap.emplace_back(base, static_cast<uint64_t>(qpc));
            if (data && frames > 0 && (flags & AUDCLNT_BUFFERFLAGS_SILENT) == 0) {
                const float* fl = reinterpret_cast<const float*>(data);
                for (UINT32 i = 0; i < frames; ++i)
                    mono.push_back(fl[static_cast<size_t>(i) * channels]);
            } else if (frames > 0) {
                mono.insert(mono.end(), frames, 0.0f);
            }
            capSvc->ReleaseBuffer(frames);
        }
    };

    // Warmup (prime render with silence, let WGC settle), discarding accumulators afterward.
    DWORD warmEnd = GetTickCount() + kWarmupMs;
    while (GetTickCount() < warmEnd) {
        PumpMessages();
        topUpRender(false);
        drainWgc();
        drainAudio();
        Sleep(3);
    }
    mono.clear();
    pktMap.clear();
    vidLuma.clear();
    vidQpc.clear();
    vframeLog = 0;

    auto nowQpc100ns = [&]() {
        LARGE_INTEGER n;
        QueryPerformanceCounter(&n);
        return RawQpcToHundredNanoseconds(static_cast<uint64_t>(n.QuadPart), qpcFreq);
    };

    // Rounds: white+tone for kFlashMs, then dark+silence for kGapMs.
    for (int r = 0; r < kRounds; ++r) {
        const uint64_t triggerQpc = nowQpc100ns();
        FillWindow(wnd, true);
        LogInfo("[AVSyncCalib] round %d white+burst trigger qpc=%llu", r, static_cast<unsigned long long>(triggerQpc));
        DWORD flashEnd = GetTickCount() + kFlashMs;
        while (GetTickCount() < flashEnd) {
            PumpMessages();
            topUpRender(true);
            drainWgc();
            drainAudio();
            Sleep(2);
        }
        FillWindow(wnd, false);
        DWORD gapEnd = GetTickCount() + kGapMs;
        while (GetTickCount() < gapEnd) {
            PumpMessages();
            topUpRender(false);
            drainWgc();
            drainAudio();
            Sleep(2);
        }
    }
    // Tail drain.
    DWORD tailEnd = GetTickCount() + kTailMs;
    while (GetTickCount() < tailEnd) {
        PumpMessages();
        topUpRender(false);
        drainWgc();
        drainAudio();
        Sleep(3);
    }

    renderClient->Stop();
    loopClient->Stop();
    wgc->StopCapture();

    // ---- Detect video flash centers ----
    double vMin = 1e9, vMax = -1e9;
    for (double l : vidLuma) {
        vMin = std::min(vMin, l);
        vMax = std::max(vMax, l);
    }
    LogInfo("[AVSyncCalib] video luma frames=%zu rawMin=%.3f rawMax=%.3f", vidLuma.size(), vidLuma.empty() ? 0.0 : vMin,
            vidLuma.empty() ? 0.0 : vMax);
    std::vector<double> vidNorm = vidLuma;
    const bool vidOk = NormalizeByMax(vidNorm);
    std::vector<uint64_t> videoCenters = vidOk ? DetectHighRunCenters(vidNorm, vidQpc) : std::vector<uint64_t>{};

    // ---- Detect audio burst centers (Goertzel marker-band power runs) ----
    std::vector<uint64_t> audioCenters;
    {
        const int win = std::max(64, sampleRate / 200);
        const int hop = std::max(1, win / 2);
        std::vector<double> powers;
        std::vector<uint64_t> powerQpc;
        if (mono.size() >= static_cast<size_t>(win)) {
            for (size_t s = 0; s + win <= mono.size(); s += hop) {
                powers.push_back(
                    GoertzelPower(mono.data() + s, win, static_cast<double>(sampleRate), spec.markerFreqHz));
                powerQpc.push_back(FrameToQpc100ns(pktMap, s + win / 2, sampleRate));
            }
        }
        double pMax = 0.0;
        for (double p : powers)
            pMax = std::max(pMax, p);
        int aboveHalf = 0;
        for (double p : powers)
            if (pMax > 0.0 && p >= 0.6 * pMax)
                ++aboveHalf;
        LogInfo("[AVSyncCalib] audio powerWindows=%zu rawMax=%.6g windowsAbove0.6max=%d", powers.size(), pMax,
                aboveHalf);
        if (NormalizeByMax(powers))
            audioCenters = DetectHighRunCenters(powers, powerQpc);
    }

    for (size_t i = 0; i < videoCenters.size() && i < 12; ++i)
        LogInfo("[AVSyncCalib] videoCenter[%zu] qpc=%llu", i, static_cast<unsigned long long>(videoCenters[i]));
    for (size_t i = 0; i < audioCenters.size() && i < 12; ++i)
        LogInfo("[AVSyncCalib] audioCenter[%zu] qpc=%llu", i, static_cast<unsigned long long>(audioCenters[i]));

    LogInfo("[AVSyncCalib] detected videoFlashes=%zu audioBursts=%zu (capturedVidFrames=%zu monoFrames=%zu)",
            videoCenters.size(), audioCenters.size(), vidLuma.size(), mono.size());

    std::vector<double> offsets = PairAvOffsetsMs(audioCenters, videoCenters, kMaxAbsOffsetMs);
    const MedianLatencyResult agg = MedianWithConsistency(offsets, std::max(3, kRounds / 2), 8.0);
    if (!agg.ok) {
        LogWarn("[AVSyncCalib] no consensus (%zu paired offsets); A/V calibration inconclusive, falling back",
                offsets.size());
        cleanup();
        return result;
    }
    if (!IsPlausibleLatencyMs(agg.latencyMs) || agg.latencyMs < 0.0) {
        LogWarn("[AVSyncCalib] implausible offset %.3f ms; rejecting", agg.latencyMs);
        cleanup();
        return result;
    }

    result.ok = true;
    result.measured = true;
    result.offsetMs = agg.latencyMs;

    UpsertLatencyCache(cache, key, agg.latencyMs);
    if (!cachePath.empty()) {
        std::ofstream f(cachePath, std::ios::binary | std::ios::trunc);
        if (f)
            f << SerializeLatencyCache(cache);
    }
    LogInfo("[AVSyncCalib] Measured A/V offset (median of %d agreeing rounds): key=%s offset=%.3f ms -> %s",
            agg.agreeingCount, key.c_str(), agg.latencyMs, cachePath.empty() ? "<no cache>" : cachePath.c_str());

    cleanup();
    return result;
}

}  // namespace ce::avcal
