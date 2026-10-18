#include "encoder_auto_probe.h"

#include <dxgi1_6.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <map>
#include <mutex>

#include "encoder_auto_select.h"
#include "mediaengine/engine/screen_grab_adapter.h"
#include "video_encoder_internal.h"

namespace ce::media {

namespace {

// Large enough for every encoder's minimum size, small enough to allocate
// nothing noticeable; the real open at the recording's size still decides
// (VideoEncoder::EnsureDevice retires a candidate whose real open fails).
constexpr int kProbeWidth = 1280;
constexpr int kProbeHeight = 720;
constexpr int kProbeFallbackFps = 60;

// Serializes probes and guards the caches. Never held while any other CE lock
// is taken: probes run on a private device.
// NOLINTNEXTLINE(bugprone-throwing-static-initialization) - std::mutex-family constructors are noexcept on this toolchain
std::mutex g_AutoEncoderMutex;
// "luid|encoder|pix_fmt|sw_format|hdr" -> open result.
// NOLINTNEXTLINE(bugprone-throwing-static-initialization) - empty std::map construction does not allocate
std::map<std::string, AutoEncoderProbeResult> g_ProbeCache;
// Lowercased request -> last resolved encoder.
// NOLINTNEXTLINE(bugprone-throwing-static-initialization) - empty std::map construction does not allocate
std::map<std::string, std::string> g_LastResolution;

std::string Lowercase(std::string value) {
    for (char& ch : value)
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return value;
}

std::string LuidText(const LUID& luid) {
    char text[24];
    std::snprintf(text, sizeof(text), "%08lX:%08lX", static_cast<unsigned long>(luid.HighPart),
                  static_cast<unsigned long>(luid.LowPart));
    return text;
}

std::string AvErrorText(int error) {
    char text[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(error, text, sizeof(text));
    return text;
}

struct AdapterInfo {
    IDXGIAdapter1* adapter = nullptr;  // owned
    uint32_t vendorId = 0;
    bool software = false;
    std::string name;

    AdapterInfo() = default;
    AdapterInfo(const AdapterInfo&) = delete;
    AdapterInfo& operator=(const AdapterInfo&) = delete;
    ~AdapterInfo() {
        if (adapter)
            adapter->Release();
    }
};

void Describe(IDXGIAdapter1* adapter, AdapterInfo* info) {
    DXGI_ADAPTER_DESC1 desc = {};
    if (FAILED(adapter->GetDesc1(&desc)))
        return;
    info->vendorId = desc.VendorId;
    info->software = (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
    for (const wchar_t* text = desc.Description; *text && info->name.size() < 64; ++text)
        info->name.push_back(*text < 128 ? static_cast<char>(*text) : '?');
}

bool FindAdapter(const LUID& luid, AdapterInfo* info) {
    IDXGIFactory4* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) || !factory)
        return false;
    const HRESULT hr = factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&info->adapter));
    factory->Release();
    if (FAILED(hr) || !info->adapter) {
        info->adapter = nullptr;
        return false;
    }
    Describe(info->adapter, info);
    return true;
}

// The probe's own device, created on the first hardware probe of a resolution.
class ProbeDevice {
public:
    explicit ProbeDevice(IDXGIAdapter1* adapter) : adapter_(adapter) {}
    ProbeDevice(const ProbeDevice&) = delete;
    ProbeDevice& operator=(const ProbeDevice&) = delete;
    ~ProbeDevice() {
        if (device_)
            device_->Release();
    }

    ID3D11Device* Get(HRESULT* failure) {
        if (device_ || attempted_) {
            *failure = result_;
            return device_;
        }
        attempted_ = true;
        const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
        result_ = D3D11CreateDevice(adapter_, D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                                    D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels,
                                    ARRAYSIZE(levels), D3D11_SDK_VERSION, &device_, nullptr, nullptr);
        if (FAILED(result_))
            device_ = nullptr;
        *failure = result_;
        return device_;
    }

private:
    IDXGIAdapter1* adapter_ = nullptr;
    ID3D11Device* device_ = nullptr;
    bool attempted_ = false;
    HRESULT result_ = S_OK;
};

struct Precheck {
    const AVCodec* codec = nullptr;
    ResolvedVideoFormat format;
    ce::video::EncoderOptionPlan plan;
    bool hdr = false;
};

// Everything that decides a candidate without the GPU.
AutoEncoderProbeResult CheckOutputContract(const std::string& encoder, bool tenBit, const VideoConfig& base,
                                           Precheck* out) {
    out->codec = avcodec_find_encoder_by_name(encoder.c_str());
    if (!out->codec)
        return {false, "not-in-build"};
    VideoConfig config = base;
    config.encoder = encoder;
    // A 10-bit capable choice must also carry HDR unless SDR is forced.
    out->hdr = tenBit && _stricmp(config.colorSpace.c_str(), "bt709") != 0;
    std::string error;
    std::string warning;
    if (!ResolveVideoFormat(config, out->hdr, tenBit, out->codec, &out->format, &error, &warning))
        return {false, "format"};
    const bool live = ce::live_stream::IsLiveStreamTarget(config.outputDir);
    const std::string hint = "capture." + config.container;
    const AVOutputFormat* container =
        live ? av_guess_format("flv", nullptr, nullptr) : av_guess_format(nullptr, hint.c_str(), nullptr);
    if (container && avformat_query_codec(container, out->codec->id, FF_COMPLIANCE_NORMAL) == 0)
        return {false, "container"};
    out->plan = ce::video::BuildEncoderOptionPlan(config, out->format.use10Bit, out->format.chroma, out->hdr);
    if (!out->plan.errors.empty())
        return {false, "plan"};
    return {true, "ok"};
}

// Opens the encoder the way EnsureDevice/ConfigureAndOpenCodec do, minus the
// recording: D3D11 frames on `device` (Quick Sync derived from them), the
// option plan's codec fields and generated plus required options.
AutoEncoderProbeResult OpenOnDevice(ID3D11Device* device, const Precheck& check, const VideoConfig& config) {
    AVBufferRef* deviceCtx = nullptr;
    AVBufferRef* framesCtx = nullptr;
    AVBufferRef* qsvDeviceCtx = nullptr;
    AVBufferRef* qsvFramesCtx = nullptr;
    AVCodecContext* codecCtx = nullptr;
    AVDictionary* options = nullptr;
    auto finish = [&](AutoEncoderProbeResult result) {
        av_dict_free(&options);
        avcodec_free_context(&codecCtx);
        av_buffer_unref(&qsvFramesCtx);
        av_buffer_unref(&qsvDeviceCtx);
        av_buffer_unref(&framesCtx);
        av_buffer_unref(&deviceCtx);
        return result;
    };

    deviceCtx = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
    if (!deviceCtx)
        return finish({false, "hwdevice"});
    auto* hwDevice = reinterpret_cast<AVHWDeviceContext*>(deviceCtx->data);
    reinterpret_cast<AVD3D11VADeviceContext*>(hwDevice->hwctx)->device = device;
    device->AddRef();  // FFmpeg owns one reference
    if (av_hwdevice_ctx_init(deviceCtx) < 0)
        return finish({false, "hwdevice"});
    if (!DeviceSupportsHwFrameSwFormat(deviceCtx, check.format.d3d11SwFormat))
        return finish({false, "sw_format"});

    framesCtx = av_hwframe_ctx_alloc(deviceCtx);
    if (!framesCtx)
        return finish({false, "frames"});
    auto* frames = reinterpret_cast<AVHWFramesContext*>(framesCtx->data);
    frames->format = AV_PIX_FMT_D3D11;
    frames->sw_format = check.format.d3d11SwFormat;
    frames->width = kProbeWidth;
    frames->height = kProbeHeight;
    frames->initial_pool_size = 0;
    reinterpret_cast<AVD3D11VAFramesContext*>(frames->hwctx)->BindFlags |= D3D11_BIND_RENDER_TARGET;
    if (av_hwframe_ctx_init(framesCtx) < 0)
        return finish({false, "frames"});

    AVBufferRef* encoderDevice = deviceCtx;
    AVBufferRef* encoderFrames = framesCtx;
    if (check.format.codecPixFmt == AV_PIX_FMT_QSV) {
        int ret = av_hwdevice_ctx_create_derived(&qsvDeviceCtx, AV_HWDEVICE_TYPE_QSV, deviceCtx, 0);
        if (ret < 0 || !qsvDeviceCtx)
            return finish({false, "qsv-device: " + AvErrorText(ret)});
        ret = av_hwframe_ctx_create_derived(&qsvFramesCtx, AV_PIX_FMT_QSV, qsvDeviceCtx, framesCtx,
                                            AV_HWFRAME_MAP_READ | AV_HWFRAME_MAP_DIRECT);
        if (ret < 0 || !qsvFramesCtx)
            return finish({false, "qsv-frames: " + AvErrorText(ret)});
        encoderDevice = qsvDeviceCtx;
        encoderFrames = qsvFramesCtx;
    }

    codecCtx = avcodec_alloc_context3(check.codec);
    if (!codecCtx)
        return finish({false, "context"});
    const int fps = config.fps > 0 ? config.fps : kProbeFallbackFps;
    codecCtx->width = kProbeWidth;
    codecCtx->height = kProbeHeight;
    codecCtx->pix_fmt = check.format.codecPixFmt;
    codecCtx->time_base = {1, fps};
    codecCtx->framerate = {fps, 1};
    codecCtx->hw_device_ctx = av_buffer_ref(encoderDevice);
    codecCtx->hw_frames_ctx = av_buffer_ref(encoderFrames);
    if (!codecCtx->hw_device_ctx || !codecCtx->hw_frames_ctx)
        return finish({false, "context"});
    codecCtx->color_primaries = check.hdr ? AVCOL_PRI_BT2020 : AVCOL_PRI_BT709;
    codecCtx->color_trc = check.hdr ? AVCOL_TRC_SMPTE2084 : AVCOL_TRC_BT709;
    codecCtx->colorspace = check.hdr ? AVCOL_SPC_BT2020_NCL : AVCOL_SPC_BT709;
    const ce::video::EncoderOptionPlan& plan = check.plan;
    codecCtx->bit_rate = plan.bitRate.value_or(0);
    codecCtx->rc_max_rate = plan.maxBitRate.value_or(0);
    codecCtx->rc_buffer_size = static_cast<int>(plan.bufferSize.value_or(0));
    if (plan.globalQuality.has_value())
        codecCtx->global_quality =
            plan.scaleGlobalQualityByQp2Lambda ? *plan.globalQuality * FF_QP2LAMBDA : *plan.globalQuality;
    if (plan.codecProfile.has_value())
        codecCtx->profile = *plan.codecProfile;
    if (plan.useConstantQscale)
        codecCtx->flags |= AV_CODEC_FLAG_QSCALE;
    if (plan.compressionLevel.has_value())
        codecCtx->compression_level = *plan.compressionLevel;
    if (plan.useLowDelay)
        codecCtx->flags |= AV_CODEC_FLAG_LOW_DELAY;
    codecCtx->max_b_frames = plan.maxBFrames;
    codecCtx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    for (const auto& option : plan.generatedOptions)
        av_dict_set(&options, option.key.c_str(), option.value.c_str(), 0);
    for (const auto& option : plan.requiredOptions)
        av_dict_set(&options, option.key.c_str(), option.value.c_str(), 0);

    const int ret = avcodec_open2(codecCtx, check.codec, &options);
    if (ret < 0)
        return finish({false, "open: " + AvErrorText(ret)});
    return finish({true, "ok"});
}

}  // namespace

bool AdapterLuidOfDevice(ID3D11Device* device, LUID* luid) {
    if (!device || !luid)
        return false;
    IDXGIDevice* dxgiDevice = nullptr;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dxgiDevice))) || !dxgiDevice)
        return false;
    IDXGIAdapter* adapter = nullptr;
    HRESULT hr = dxgiDevice->GetAdapter(&adapter);
    dxgiDevice->Release();
    if (FAILED(hr) || !adapter)
        return false;
    DXGI_ADAPTER_DESC desc = {};
    hr = adapter->GetDesc(&desc);
    adapter->Release();
    if (FAILED(hr))
        return false;
    *luid = desc.AdapterLuid;
    return true;
}

AutoEncoderResolution ResolveAutoEncoderForAdapter(const std::string& request, const LUID& luid,
                                                   const VideoConfig& config,
                                                   const std::vector<std::string>& excluded) {
    AutoEncoderResolution resolution;
    std::vector<encoder_auto::CodecFamily> families;
    if (!encoder_auto::ParseAutoEncoderRequest(request, &families)) {
        resolution.log = "[EncoderAuto] '" + request + "' is not an auto request";
        return resolution;
    }

    std::lock_guard<std::mutex> lock(g_AutoEncoderMutex);
    const auto started = std::chrono::steady_clock::now();
    AdapterInfo adapter;
    if (!FindAdapter(luid, &adapter)) {
        resolution.log = "[EncoderAuto] request=" + request + " adapter luid=" + LuidText(luid) +
                         " not found; no encoder chosen";
        return resolution;
    }

    std::vector<std::string> candidates;
    if (!adapter.software) {
        for (std::string& candidate : AutoEncoderCandidates(families, adapter.vendorId)) {
            if (std::find(excluded.begin(), excluded.end(), candidate) == excluded.end())
                candidates.push_back(std::move(candidate));
        }
    }

    const std::string luidText = LuidText(luid);
    ProbeDevice device(adapter.adapter);
    size_t hardwareProbes = 0;
    size_t cacheHits = 0;
    const AutoEncoderProbe probe = [&](const std::string& encoder, bool tenBit) -> AutoEncoderProbeResult {
        Precheck check;
        const AutoEncoderProbeResult contract = CheckOutputContract(encoder, tenBit, config, &check);
        if (!contract.usable)
            return contract;
        const std::string key = luidText + '|' + encoder + '|' + GetPixFmtNameSafe(check.format.codecPixFmt) + '|' +
                                GetPixFmtNameSafe(check.format.d3d11SwFormat) + (check.hdr ? "|hdr" : "|sdr");
        if (const auto cached = g_ProbeCache.find(key); cached != g_ProbeCache.end()) {
            ++cacheHits;
            return cached->second;
        }
        HRESULT deviceResult = S_OK;
        ID3D11Device* probeDevice = device.Get(&deviceResult);
        if (!probeDevice) {
            char detail[40];
            std::snprintf(detail, sizeof(detail), "device 0x%08lX", static_cast<unsigned long>(deviceResult));
            return {false, detail};  // not cached: says nothing about the encoder
        }
        const auto probeStarted = std::chrono::steady_clock::now();
        const AutoEncoderProbeResult result = OpenOnDevice(probeDevice, check, config);
        const auto probeMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now() - probeStarted)
                                 .count();
        ++hardwareProbes;
        DLL_Log("[EncoderAuto] Probe %s %s/%s%s on \"%s\": %s (%lld ms)", encoder.c_str(),
                GetPixFmtNameSafe(check.format.codecPixFmt), GetPixFmtNameSafe(check.format.d3d11SwFormat),
                check.hdr ? " hdr" : "", adapter.name.c_str(), result.usable ? "ok" : result.detail.c_str(),
                static_cast<long long>(probeMs));
        g_ProbeCache[key] = result;
        return result;
    };

    const bool wantTenBit = _stricmp(config.bitDepth.c_str(), "8") != 0;
    const AutoEncoderChoice choice = ChooseAutoEncoder(candidates, wantTenBit, probe);
    resolution.encoder = choice.encoder;
    if (!choice.encoder.empty())
        g_LastResolution[Lowercase(request)] = choice.encoder;

    const auto totalMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
    char line[256];
    std::snprintf(line, sizeof(line),
                  "[EncoderAuto] request=%s adapter=\"%s\" vendor=0x%04X luid=%s%s bit_depth=%s -> %s "
                  "(10bit=%d candidates=%zu excluded=%zu probes=%zu hw=%zu cached=%zu %lldms) trail: ",
                  request.c_str(), adapter.name.c_str(), adapter.vendorId, luidText.c_str(),
                  adapter.software ? " software" : "", config.bitDepth.c_str(),
                  choice.encoder.empty() ? "none usable" : choice.encoder.c_str(), choice.tenBit ? 1 : 0,
                  candidates.size(), excluded.size(), choice.probes, hardwareProbes, cacheHits,
                  static_cast<long long>(totalMs));
    resolution.log = line + (choice.trail.empty() ? std::string("(none)") : choice.trail);
    return resolution;
}

std::string ProvisionalAutoEncoder(const std::string& request) {
    std::vector<encoder_auto::CodecFamily> families;
    if (!encoder_auto::ParseAutoEncoderRequest(request, &families))
        return request;
    {
        std::lock_guard<std::mutex> lock(g_AutoEncoderMutex);
        if (const auto last = g_LastResolution.find(Lowercase(request)); last != g_LastResolution.end())
            return last->second;
    }

    uint32_t vendorId = 0;
    IDXGIFactory6* factory = nullptr;
    if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) && factory) {
        for (UINT index = 0; vendorId == 0; ++index) {
            IDXGIAdapter1* adapter = nullptr;
            if (factory->EnumAdapterByGpuPreference(index, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                                                    IID_PPV_ARGS(&adapter)) == DXGI_ERROR_NOT_FOUND)
                break;
            if (!adapter)
                continue;
            AdapterInfo info;
            Describe(adapter, &info);
            adapter->Release();
            if (!info.software)
                vendorId = info.vendorId;
        }
        factory->Release();
    }
    const std::vector<std::string> candidates = AutoEncoderCandidates(families, vendorId);
    for (const std::string& candidate : candidates) {
        if (avcodec_find_encoder_by_name(candidate.c_str()))
            return candidate;
    }
    return candidates.empty() ? request : candidates.front();
}

}  // namespace ce::media
