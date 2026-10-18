#include <gtest/gtest.h>

#include <dxgi1_6.h>

#include <string>

#include "test_config_shared.h"

#include "mediaengine/video/encoder_auto_probe.h"
#include "mediaengine/video/video_encoder.h"

// encoder=auto against the real GPU (llm-wiki/encoder-auto-selection.md). The
// hardware cases skip on a machine without a usable hardware encoder (VMs, CI);
// on the dev box they open the real encoders.

namespace {

VideoConfig DefaultVideoConfig() {
    const std::string path = MakeTestPath("encoder_auto_probe.ini");
    WriteTextFile(path, "[Video]\n");
    AppConfig config;
    LoadConfig(path, config);
    DeleteFileA(path.c_str());
    return config.video;
}

// The first hardware adapter in high-performance order.
bool FirstHardwareAdapter(LUID* luid, uint32_t* vendorId) {
    IDXGIFactory6* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) || !factory)
        return false;
    bool found = false;
    for (UINT index = 0; !found; ++index) {
        IDXGIAdapter1* adapter = nullptr;
        if (factory->EnumAdapterByGpuPreference(index, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                                                IID_PPV_ARGS(&adapter)) == DXGI_ERROR_NOT_FOUND)
            break;
        if (!adapter)
            continue;
        DXGI_ADAPTER_DESC1 desc = {};
        if (SUCCEEDED(adapter->GetDesc1(&desc)) && (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0) {
            *luid = desc.AdapterLuid;
            *vendorId = desc.VendorId;
            found = true;
        }
        adapter->Release();
    }
    factory->Release();
    return found;
}

bool EndsWith(const std::string& value, const std::string& suffix) {
    return value.size() >= suffix.size() && value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

TEST(EncoderAutoProbeTest, DefaultConfigRequestsAuto) {
    EXPECT_EQ(DefaultVideoConfig().encoder, "auto");
}

TEST(EncoderAutoProbeTest, ProvisionalNameIsConcreteAndCompiledIn) {
    for (const char* request : {"auto", "hevc_auto", "h264_auto"}) {
        const std::string provisional = ce::media::ProvisionalAutoEncoder(request);
        EXPECT_FALSE(ce::encoder_auto::IsAutoEncoderRequest(provisional)) << request << " -> " << provisional;
        EXPECT_NE(avcodec_find_encoder_by_name(provisional.c_str()), nullptr) << request << " -> " << provisional;
    }
    EXPECT_EQ(ce::media::ProvisionalAutoEncoder("h264_auto").rfind("h264_", 0), 0u);
    EXPECT_EQ(ce::media::ProvisionalAutoEncoder("hevc_amf"), "hevc_amf");
}

TEST(EncoderAutoProbeTest, VideoEncoderInitAcceptsAnAutoRequest) {
    // Before encoder=auto, Init failed with "Codec not found: auto".
    VideoConfig config = DefaultVideoConfig();
    ASSERT_EQ(config.encoder, "auto");
    VideoEncoder encoder;
    EXPECT_TRUE(encoder.Init(config, 0, 0, config.fps, [](AVPacket*) {}));
}

TEST(EncoderAutoProbeTest, ResolvesAnEncoderThatOpensOnTheFirstGpuAndCachesIt) {
    LUID luid = {};
    uint32_t vendorId = 0;
    if (!FirstHardwareAdapter(&luid, &vendorId))
        GTEST_SKIP() << "no hardware adapter";
    const VideoConfig config = DefaultVideoConfig();
    const auto first = ce::media::ResolveAutoEncoderForAdapter("auto", luid, config);
    if (first.encoder.empty())
        GTEST_SKIP() << "no usable hardware encoder: " << first.log;
    RecordProperty("resolution", first.log);  // visible with --gtest_output=xml

    EXPECT_NE(avcodec_find_encoder_by_name(first.encoder.c_str()), nullptr);
    const char* vendorSuffix = vendorId == 0x10DE   ? "_nvenc"
                               : vendorId == 0x1002 ? "_amf"
                               : vendorId == 0x8086 ? "_qsv"
                                                    : "_mf";
    EXPECT_TRUE(EndsWith(first.encoder, vendorSuffix) || EndsWith(first.encoder, "_mf")) << first.log;
    EXPECT_NE(first.log.find("request=auto"), std::string::npos) << first.log;

    // The same GPU and contract never probes the hardware again.
    const auto second = ce::media::ResolveAutoEncoderForAdapter("auto", luid, config);
    EXPECT_EQ(second.encoder, first.encoder);
    EXPECT_NE(second.log.find(" hw=0 "), std::string::npos) << second.log;

    // A choice whose real open failed is excluded and something else (or
    // nothing) takes its place.
    const auto retry = ce::media::ResolveAutoEncoderForAdapter("auto", luid, config, {first.encoder});
    EXPECT_NE(retry.encoder, first.encoder) << retry.log;
}

TEST(EncoderAutoProbeTest, StreamingRequestResolvesToAnH264Encoder) {
    LUID luid = {};
    uint32_t vendorId = 0;
    if (!FirstHardwareAdapter(&luid, &vendorId))
        GTEST_SKIP() << "no hardware adapter";
    VideoConfig config = DefaultVideoConfig();
    config.bitDepth = "8";
    config.colorSpace = "bt709";
    const auto resolution = ce::media::ResolveAutoEncoderForAdapter("h264_auto", luid, config);
    if (resolution.encoder.empty())
        GTEST_SKIP() << "no usable H.264 hardware encoder: " << resolution.log;
    RecordProperty("resolution", resolution.log);
    EXPECT_EQ(resolution.encoder.rfind("h264_", 0), 0u) << resolution.log;
    EXPECT_NE(resolution.log.find("10bit=0"), std::string::npos) << resolution.log;
}

TEST(EncoderAutoProbeTest, UnknownAdapterResolvesToNothing) {
    LUID bogus = {};
    bogus.LowPart = 0xFFFFFFF0u;
    bogus.HighPart = 0x7FFFFFF0;
    const auto resolution = ce::media::ResolveAutoEncoderForAdapter("auto", bogus, DefaultVideoConfig());
    EXPECT_TRUE(resolution.encoder.empty());
    EXPECT_NE(resolution.log.find("not found"), std::string::npos) << resolution.log;
}

}  // namespace
