#include <gtest/gtest.h>

#include <d3d11.h>
#include <dxgi1_6.h>

#include <cstdio>
#include <functional>
#include <string>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/dict.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
#include <libavutil/opt.h>
}

#include "common/config/config.h"
#include "mediaengine/engine/av_log_open_capture.h"
#include "mediaengine/engine/mediaengine.h"
#include "mediaengine/video/video_encoder_options.h"

namespace {

struct AmfMatrixCase {
    std::string label;
    VideoConfig config;
    bool use10Bit = false;
    bool expectPlanError = false;
};

VideoConfig MakeBaseAmfConfig(const char* encoder, const char* rateControl) {
    VideoConfig config;
    config.encoder = encoder;
    config.fps = 60;
    config.rateControl = rateControl;
    config.bitrate = "50Mbps";
    config.maxBitrate = "100Mbps";
    config.profile = "auto";
    config.bFrames = 0;
    config.amfUsage = "transcoding";
    config.amfPreset = "speed";
    config.amfQp = 23;
    config.amfAsyncDepth = 0;
    config.amfPreanalysis = "auto";
    config.amfLookahead = "off";
    return config;
}

std::vector<AmfMatrixCase> BuildAmfMatrixCases() {
    std::vector<AmfMatrixCase> cases;

    auto add = [&](const char* encoder, const char* rc, bool use10Bit, const char* variant,
                   const std::function<void(VideoConfig*)>& tweak, bool expectError = false) {
        VideoConfig cfg = MakeBaseAmfConfig(encoder, rc);
        if (tweak) {
            tweak(&cfg);
        }
        std::string label = std::string(encoder) + " " + (use10Bit ? "P010" : "NV12") + " " + rc + " " + variant;
        cases.push_back({std::move(label), std::move(cfg), use10Bit, expectError});
    };

    for (const char* enc : {"h264_amf", "hevc_amf", "av1_amf"}) {
        const bool supports10Bit = std::string(enc) != "h264_amf";
        const bool supportsBFrames = std::string(enc) != "hevc_amf";

        // Core rate-control baselines (8-bit NV12)
        for (const char* rc : {"VBR", "CBR", "CQ", "HQVBR", "HQCBR", "CQP"}) {
            add(enc, rc, false, "baseline", nullptr);
        }

        // Individual feature toggles under 8-bit VBR
        add(enc, "VBR", false, "spatial_aq", [](VideoConfig* c) { c->amfSpatialAq = true; });
        add(enc, "VBR", false, "temporal_aq", [](VideoConfig* c) { c->amfTemporalAq = true; });
        add(enc, "VBR", false, "lookahead=20", [](VideoConfig* c) { c->amfLookahead = "20"; });
        add(enc, "VBR", false, "pa+spatial+temporal+hmqb", [](VideoConfig* c) {
            c->amfLookahead = "20";
            c->amfSpatialAq = true;
            c->amfTemporalAq = true;
            c->amfAqStrength = 2;
            c->amfHighMotionQualityBoost = true;
        });
        add(enc, "VBR", false, "hw_hmqb_no_pa", [](VideoConfig* c) { c->amfHighMotionQualityBoost = true; });
        add(enc, "VBR", false, "preencode", [](VideoConfig* c) { c->amfPreencode = true; });
        add(enc, "CBR", false, "hrd+filler", [](VideoConfig* c) {
            c->amfEnforceHrd = true;
            c->amfFillerData = true;
        });
        if (supportsBFrames) {
            add(enc, "VBR", false, "b_frames=2", [](VideoConfig* c) {
                c->bFrames = 2;
                c->amfBRefMode = "enabled";
            });
        }

        // 10-bit P010 (HDR) cells
        if (supports10Bit) {
            add(enc, "VBR", true, "baseline", nullptr);
            add(enc, "VBR", true, "spatial_aq+hmqb", [](VideoConfig* c) {
                c->amfSpatialAq = true;
                c->amfHighMotionQualityBoost = true;
            });
            add(enc, "VBR", true, "degraded_pa_assists", [](VideoConfig* c) {
                c->amfLookahead = "20";
                c->amfTemporalAq = true;
                c->amfSpatialAq = true;
            });
            add(enc, "CBR", true, "hrd+filler", [](VideoConfig* c) {
                c->amfEnforceHrd = true;
                c->amfFillerData = true;
            });
            add(enc, "CQP", true, "baseline", nullptr);
            add(enc, "CQ", true, "pa_rejected", nullptr, /*expectError=*/true);
        }
    }

    return cases;
}

bool CreateAmdD3D11Device(ID3D11Device** outDevice, std::string* outAdapterName) {
    *outDevice = nullptr;
    IDXGIFactory6* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) || !factory) {
        return false;
    }
    bool found = false;
    for (UINT i = 0; !found; ++i) {
        IDXGIAdapter1* adapter = nullptr;
        if (factory->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter)) ==
            DXGI_ERROR_NOT_FOUND) {
            break;
        }
        if (!adapter) {
            continue;
        }
        DXGI_ADAPTER_DESC1 desc = {};
        if (SUCCEEDED(adapter->GetDesc1(&desc)) && (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0 &&
            desc.VendorId == 0x1002) {
            ID3D11DeviceContext* ctx = nullptr;
            D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
            if (SUCCEEDED(D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
                                            nullptr, 0, D3D11_SDK_VERSION, outDevice, &level, &ctx))) {
                if (ctx) {
                    ctx->Release();
                }
                if (outAdapterName) {
                    char utf8[256] = {};
                    std::snprintf(utf8, sizeof(utf8), "%ls", desc.Description);
                    *outAdapterName = utf8;
                }
                found = true;
            }
        }
        adapter->Release();
    }
    factory->Release();
    return found;
}

TEST(AmfHardwareMatrixProbe, PlannedMatrixOptionsAreAcceptedByBundledFfmpeg) {
    const std::vector<AmfMatrixCase> cases = BuildAmfMatrixCases();
    ASSERT_FALSE(cases.empty());

    for (const AmfMatrixCase& item : cases) {
        const AVCodec* codec = avcodec_find_encoder_by_name(item.config.encoder.c_str());
        ASSERT_NE(codec, nullptr) << item.label;

        const ce::video::EncoderOptionPlan plan =
            ce::video::BuildEncoderOptionPlan(item.config, item.use10Bit, "420", item.use10Bit);
        if (item.expectPlanError) {
            EXPECT_FALSE(plan.errors.empty()) << item.label;
            continue;
        }
        ASSERT_TRUE(plan.errors.empty()) << item.label << ": " << plan.errors.front();

        AVCodecContext* ctx = avcodec_alloc_context3(codec);
        ASSERT_NE(ctx, nullptr) << item.label;
        for (const ce::video::EncoderOption& option : plan.generatedOptions) {
            const int res = av_opt_set(ctx, option.key.c_str(), option.value.c_str(), AV_OPT_SEARCH_CHILDREN);
            EXPECT_GE(res, 0) << item.label << " rejected option " << option.key << "=" << option.value;
        }
        avcodec_free_context(&ctx);
    }
}

// Opt-in Radeon hardware sweep:
//   tests\unit_tests.exe --gtest_also_run_disabled_tests --gtest_filter=AmfHardwareMatrixProbe.DISABLED_*
// Opens every planned AMF combination on a real AMD D3D11 device and prints a
// pass/fail table with the captured AMF internal rejection trace for any failure.
TEST(AmfHardwareMatrixProbe, DISABLED_PrintHardwareCompatibilityMatrix) {
    ID3D11Device* device = nullptr;
    std::string adapterName;
    if (!CreateAmdD3D11Device(&device, &adapterName)) {
        GTEST_SKIP() << "no AMD hardware adapter available";
    }

    MediaEngine_SetLogCallback([](const char*) {});
    std::printf("[AMF Matrix] Adapter: %s\n", adapterName.c_str());

    const std::vector<AmfMatrixCase> cases = BuildAmfMatrixCases();
    int passed = 0;
    int failed = 0;
    for (const AmfMatrixCase& item : cases) {
        if (item.expectPlanError) {
            continue;
        }
        const AVCodec* codec = avcodec_find_encoder_by_name(item.config.encoder.c_str());
        const ce::video::EncoderOptionPlan plan =
            ce::video::BuildEncoderOptionPlan(item.config, item.use10Bit, "420", item.use10Bit);
        if (!codec || !plan.errors.empty()) {
            std::printf("  [PLAN-ERR] %-46s %s\n", item.label.c_str(),
                        plan.errors.empty() ? "missing codec" : plan.errors.front().c_str());
            ++failed;
            continue;
        }

        AVBufferRef* devCtx = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
        auto* hwDev = reinterpret_cast<AVHWDeviceContext*>(devCtx->data);
        reinterpret_cast<AVD3D11VADeviceContext*>(hwDev->hwctx)->device = device;
        device->AddRef();
        ASSERT_GE(av_hwdevice_ctx_init(devCtx), 0);

        AVBufferRef* framesCtx = av_hwframe_ctx_alloc(devCtx);
        auto* frames = reinterpret_cast<AVHWFramesContext*>(framesCtx->data);
        frames->format = AV_PIX_FMT_D3D11;
        frames->sw_format = item.use10Bit ? AV_PIX_FMT_P010 : AV_PIX_FMT_NV12;
        frames->width = 1280;
        frames->height = 720;
        frames->initial_pool_size = 0;
        reinterpret_cast<AVD3D11VAFramesContext*>(frames->hwctx)->BindFlags |= D3D11_BIND_RENDER_TARGET;
        ASSERT_GE(av_hwframe_ctx_init(framesCtx), 0);

        AVCodecContext* ctx = avcodec_alloc_context3(codec);
        ctx->width = 1280;
        ctx->height = 720;
        ctx->pix_fmt = AV_PIX_FMT_D3D11;
        ctx->time_base = {1, item.config.fps};
        ctx->framerate = {item.config.fps, 1};
        ctx->hw_device_ctx = av_buffer_ref(devCtx);
        ctx->hw_frames_ctx = av_buffer_ref(framesCtx);
        ctx->color_primaries = item.use10Bit ? AVCOL_PRI_BT2020 : AVCOL_PRI_BT709;
        ctx->color_trc = item.use10Bit ? AVCOL_TRC_SMPTE2084 : AVCOL_TRC_BT709;
        ctx->colorspace = item.use10Bit ? AVCOL_SPC_BT2020_NCL : AVCOL_SPC_BT709;
        ctx->bit_rate = plan.bitRate.value_or(0);
        ctx->rc_max_rate = plan.maxBitRate.value_or(0);
        ctx->rc_buffer_size = static_cast<int>(plan.bufferSize.value_or(0));
        ctx->max_b_frames = plan.maxBFrames;
        ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

        AVDictionary* opts = nullptr;
        for (const ce::video::EncoderOption& opt : plan.generatedOptions) {
            av_dict_set(&opts, opt.key.c_str(), opt.value.c_str(), 0);
        }

        ce::media::ScopedAvOpenDebugCapture debugCapture;
        const int ret = avcodec_open2(ctx, codec, &opts);
        av_dict_free(&opts);

        if (ret >= 0) {
            std::printf("  [PASS]     %-46s\n", item.label.c_str());
            ++passed;
        } else {
            char errbuf[AV_ERROR_MAX_STRING_SIZE] = {};
            av_strerror(ret, errbuf, sizeof(errbuf));
            const std::string trace = debugCapture.LastLineTruncated(180);
            std::printf("  [FAIL]     %-46s err=%s%s%s\n", item.label.c_str(), errbuf,
                        trace.empty() ? "" : " | ", trace.c_str());
            ++failed;
        }

        avcodec_free_context(&ctx);
        av_buffer_unref(&framesCtx);
        av_buffer_unref(&devCtx);
    }

    std::printf("[AMF Matrix] Summary: %d passed, %d failed\n", passed, failed);
    av_log_set_callback(av_log_default_callback);
    MediaEngine_SetLogCallback(nullptr);
    device->Release();
    EXPECT_EQ(failed, 0);
}

}  // namespace
