#include <gtest/gtest.h>

#include <dxgi1_6.h>

#include <string>

#include "test_config_shared.h"

#include "mediaengine/video/encoder_auto_probe.h"
#include "mediaengine/video/video_encoder.h"
#include "source_fragment_reader.h"
#include "video_encoder_test_access.h"

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

TEST(EncoderAutoProbeTest, FirstFrameReinitializationKeepsTheAutoBinding) {
    // The first frame re-runs Init on savedConfig, which already holds a
    // concrete name; that used to clear the request and with it every later
    // reselection and the retry after a failed real open.
    VideoConfig config = DefaultVideoConfig();
    ASSERT_EQ(config.encoder, "auto");
    VideoEncoder encoder;
    ASSERT_TRUE(encoder.Init(config, 0, 0, config.fps, [](AVPacket*) {}));
    const std::string provisional = VideoEncoderTestAccess::EncoderName(encoder);
    ASSERT_FALSE(ce::encoder_auto::IsAutoEncoderRequest(provisional)) << provisional;
    ce::media::AutoEncoderBinding& binding = VideoEncoderTestAccess::AutoEncoder(encoder);
    ASSERT_EQ(binding.request, "auto");
    // A resolution with an exclusion on some GPU, as an earlier open failure leaves it.
    binding.resolved = true;
    binding.adapter.LowPart = 0x1234u;
    binding.excluded = {"failed_encoder"};

    ASSERT_TRUE(VideoEncoderTestAccess::InitializeFirstInjectFrame(encoder, 64, 32));
    EXPECT_EQ(binding.request, "auto");
    EXPECT_TRUE(binding.resolved);
    EXPECT_EQ(binding.adapter.LowPart, 0x1234u);
    ASSERT_EQ(binding.excluded.size(), 1u);
    EXPECT_EQ(binding.excluded.front(), "failed_encoder");
    EXPECT_EQ(VideoEncoderTestAccess::EncoderName(encoder), provisional);

    // A new recording's Init starts a fresh binding.
    ASSERT_TRUE(encoder.Init(config, 0, 0, config.fps, [](AVPacket*) {}));
    EXPECT_EQ(binding.request, "auto");
    EXPECT_FALSE(binding.resolved);
    EXPECT_TRUE(binding.excluded.empty());
}

TEST(EncoderAutoProbeTest, FirstFrameReinitializationNeverInventsAnAutoRequest) {
    VideoConfig config = DefaultVideoConfig();
    config.encoder = ce::media::ProvisionalAutoEncoder("auto");  // concrete and compiled in
    VideoEncoder encoder;
    ASSERT_TRUE(encoder.Init(config, 0, 0, config.fps, [](AVPacket*) {}));
    ASSERT_TRUE(VideoEncoderTestAccess::InitializeFirstInjectFrame(encoder, 64, 32));
    EXPECT_TRUE(VideoEncoderTestAccess::AutoEncoder(encoder).request.empty());
    EXPECT_EQ(VideoEncoderTestAccess::EncoderName(encoder), config.encoder);
}

TEST(EncoderAutoProbeTest, InternalReinitializationGoesThroughTheBindingKeepingHelper) {
    // The screen-grab first frame needs a device and a real open to reach its
    // Init; both internal sites must use the helper the test above covers.
    for (const char* unit : {"video_encoder_encode.cpp", "video_encoder_framegrab.cpp"}) {
        const std::string source = ce::test_source::ReadFile(ce::test_source::FindSource("mediaengine", unit));
        ASSERT_FALSE(source.empty()) << unit;
        EXPECT_EQ(source.find("Init(savedConfig"), std::string::npos) << unit;
        EXPECT_NE(source.find("ReinitWithSavedConfig("), std::string::npos) << unit;
    }
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
