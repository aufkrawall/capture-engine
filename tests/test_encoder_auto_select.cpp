#include <gtest/gtest.h>

#include <map>
#include <string>
#include <utility>
#include <vector>

#include "common/config/encoder_auto_request.h"
#include "common/config/live_stream_config.h"
#include "mediaengine/engine/screen_grab_adapter.h"
#include "mediaengine/video/encoder_auto_select.h"

// encoder=auto (llm-wiki/encoder-auto-selection.md): the request grammar, the
// candidate ranking per GPU vendor and the probe-driven choice, without
// hardware. test_encoder_auto_probe.cpp covers the real probe.

namespace {

using ce::encoder_auto::CodecFamily;
using ce::encoder_auto::ParseAutoEncoderRequest;
using ce::media::AutoEncoderCandidates;
using ce::media::AutoEncoderProbeResult;
using ce::media::ChooseAutoEncoder;

// NOLINTNEXTLINE(bugprone-throwing-static-initialization) - test-binary constant; allocation failure aborts the run
const std::vector<CodecFamily> kAllFamilies = {CodecFamily::kAv1, CodecFamily::kHevc, CodecFamily::kH264};

// Fake probe: `usable` maps "encoder:10" / "encoder:8" to true; records calls.
struct FakeProbe {
    std::map<std::string, bool> usable;
    std::vector<std::string> calls;

    ce::media::AutoEncoderProbe Bind() {
        return [this](const std::string& encoder, bool tenBit) {
            const std::string key = encoder + (tenBit ? ":10" : ":8");
            calls.push_back(key);
            const auto it = usable.find(key);
            const bool ok = it != usable.end() && it->second;
            return AutoEncoderProbeResult{ok, ok ? "ok" : "open: Function not implemented"};
        };
    }
};

TEST(EncoderAutoRequestTest, AutoAndCodecRestrictedFormsParseCaseInsensitively) {
    std::vector<CodecFamily> families;
    ASSERT_TRUE(ParseAutoEncoderRequest("auto", &families));
    EXPECT_EQ(families, kAllFamilies);
    EXPECT_TRUE(ParseAutoEncoderRequest("AUTO", nullptr));
    ASSERT_TRUE(ParseAutoEncoderRequest("Hevc_Auto", &families));
    EXPECT_EQ(families, std::vector<CodecFamily>{CodecFamily::kHevc});
    ASSERT_TRUE(ParseAutoEncoderRequest("h264_auto", &families));
    EXPECT_EQ(families, std::vector<CodecFamily>{CodecFamily::kH264});
    ASSERT_TRUE(ParseAutoEncoderRequest("av1_auto", &families));
    EXPECT_EQ(families, std::vector<CodecFamily>{CodecFamily::kAv1});
}

TEST(EncoderAutoRequestTest, ConcreteAndMalformedNamesAreNotAutoRequests) {
    for (const char* name : {"av1_nvenc", "hevc_amf", "h264_qsv", "av1_mf", "", "_auto", "vp9_auto", "autox",
                             "auto_hevc", "h264auto"}) {
        EXPECT_FALSE(ce::encoder_auto::IsAutoEncoderRequest(name)) << name;
    }
}

TEST(EncoderAutoSelectTest, VendorWrappersRankBeforeMediaFoundationBestCodecFirst) {
    EXPECT_EQ(AutoEncoderCandidates(kAllFamilies, ce::media::kVendorNvidia),
              (std::vector<std::string>{"av1_nvenc", "hevc_nvenc", "h264_nvenc", "av1_mf", "hevc_mf", "h264_mf"}));
    EXPECT_EQ(AutoEncoderCandidates(kAllFamilies, ce::media::kVendorAmd),
              (std::vector<std::string>{"av1_amf", "hevc_amf", "h264_amf", "av1_mf", "hevc_mf", "h264_mf"}));
    EXPECT_EQ(AutoEncoderCandidates(kAllFamilies, ce::media::kVendorIntel),
              (std::vector<std::string>{"av1_qsv", "hevc_qsv", "h264_qsv", "av1_mf", "hevc_mf", "h264_mf"}));
}

TEST(EncoderAutoSelectTest, UnknownVendorsGetMediaFoundationOnly) {
    constexpr uint32_t kVendorQualcomm = 0x5143;
    EXPECT_EQ(AutoEncoderCandidates(kAllFamilies, kVendorQualcomm),
              (std::vector<std::string>{"av1_mf", "hevc_mf", "h264_mf"}));
    EXPECT_EQ(AutoEncoderCandidates(kAllFamilies, 0), (std::vector<std::string>{"av1_mf", "hevc_mf", "h264_mf"}));
}

TEST(EncoderAutoSelectTest, CodecRestrictedRequestsKeepOnlyThatFamily) {
    EXPECT_EQ(AutoEncoderCandidates({CodecFamily::kH264}, ce::media::kVendorAmd),
              (std::vector<std::string>{"h264_amf", "h264_mf"}));
}

TEST(EncoderAutoSelectTest, FirstUsableCandidateWinsAndLaterOnesAreNeverProbed) {
    FakeProbe probe;
    probe.usable = {{"av1_nvenc:10", true}, {"hevc_nvenc:10", true}};
    const auto choice = ChooseAutoEncoder(AutoEncoderCandidates(kAllFamilies, ce::media::kVendorNvidia), true,
                                          probe.Bind());
    EXPECT_EQ(choice.encoder, "av1_nvenc");
    EXPECT_TRUE(choice.tenBit);
    EXPECT_EQ(probe.calls, std::vector<std::string>{"av1_nvenc:10"});
    EXPECT_EQ(choice.probes, 1u);
}

TEST(EncoderAutoSelectTest, MissingAv1FallsBackToTenBitHevc) {
    // A GPU generation with HEVC but no AV1 encoder.
    FakeProbe probe;
    probe.usable = {{"hevc_nvenc:10", true}, {"hevc_nvenc:8", true}, {"h264_nvenc:8", true}};
    const auto choice = ChooseAutoEncoder(AutoEncoderCandidates(kAllFamilies, ce::media::kVendorNvidia), true,
                                          probe.Bind());
    EXPECT_EQ(choice.encoder, "hevc_nvenc");
    EXPECT_TRUE(choice.tenBit);
    EXPECT_EQ(probe.calls, (std::vector<std::string>{"av1_nvenc:10", "hevc_nvenc:10"}));
    EXPECT_EQ(choice.trail, "av1_nvenc:10=open: Function not implemented hevc_nvenc:10=ok");
}

TEST(EncoderAutoSelectTest, TenBitPreferenceBeatsCodecRankOnlyAfterEveryTenBitCandidateFailed) {
    // 8-bit-only HEVC hardware: the 10-bit pass finds nothing, so the best
    // 8-bit candidate wins (HEVC before H.264).
    FakeProbe probe;
    probe.usable = {{"hevc_amf:8", true}, {"h264_amf:8", true}};
    const auto choice = ChooseAutoEncoder(AutoEncoderCandidates(kAllFamilies, ce::media::kVendorAmd), true,
                                          probe.Bind());
    EXPECT_EQ(choice.encoder, "hevc_amf");
    EXPECT_FALSE(choice.tenBit);
    ASSERT_EQ(probe.calls.size(), 8u);  // six 10-bit probes, then av1 and hevc at 8-bit
    EXPECT_EQ(probe.calls[6], "av1_amf:8");
    EXPECT_EQ(probe.calls[7], "hevc_amf:8");
}

TEST(EncoderAutoSelectTest, TenBitCapableCandidateWinsOverBetterRankedEightBitOnlyOne) {
    // AV1 opens only at 8-bit, HEVC at 10-bit: an HDR recording needs the
    // latter, so with bit_depth auto it is chosen.
    FakeProbe probe;
    probe.usable = {{"av1_qsv:8", true}, {"hevc_qsv:10", true}};
    const auto choice = ChooseAutoEncoder(AutoEncoderCandidates(kAllFamilies, ce::media::kVendorIntel), true,
                                          probe.Bind());
    EXPECT_EQ(choice.encoder, "hevc_qsv");
    EXPECT_TRUE(choice.tenBit);
}

TEST(EncoderAutoSelectTest, ForcedEightBitProbesOnlyEightBit) {
    FakeProbe probe;
    probe.usable = {{"av1_qsv:8", true}, {"hevc_qsv:10", true}};
    const auto choice = ChooseAutoEncoder(AutoEncoderCandidates(kAllFamilies, ce::media::kVendorIntel), false,
                                          probe.Bind());
    EXPECT_EQ(choice.encoder, "av1_qsv");
    EXPECT_FALSE(choice.tenBit);
    EXPECT_EQ(probe.calls, std::vector<std::string>{"av1_qsv:8"});
}

TEST(EncoderAutoSelectTest, MediaFoundationIsTheFallbackWhenTheVendorSdkCannotOpen) {
    FakeProbe probe;
    probe.usable = {{"hevc_mf:8", true}};
    const auto choice = ChooseAutoEncoder(AutoEncoderCandidates(kAllFamilies, ce::media::kVendorNvidia), true,
                                          probe.Bind());
    EXPECT_EQ(choice.encoder, "hevc_mf");
    EXPECT_FALSE(choice.tenBit);
}

TEST(EncoderAutoSelectTest, NothingUsableYieldsAnEmptyChoiceWithACappedTrail) {
    FakeProbe probe;
    const auto choice = ChooseAutoEncoder(AutoEncoderCandidates(kAllFamilies, ce::media::kVendorNvidia), true,
                                          probe.Bind());
    EXPECT_TRUE(choice.encoder.empty());
    EXPECT_EQ(choice.probes, 12u);
    EXPECT_NE(choice.trail.find(" +4 more"), std::string::npos) << choice.trail;
    EXPECT_TRUE(ChooseAutoEncoder({}, true, probe.Bind()).encoder.empty());
}

// --- Screen-grab GPU for encoder=auto (screen_grab_adapter.h) ---

ce::media::AdapterCandidate Adapter(uint32_t vendor, bool drivesMonitor, bool software = false) {
    ce::media::AdapterCandidate candidate;
    candidate.vendorId = vendor;
    candidate.drivesTargetMonitor = drivesMonitor;
    candidate.software = software;
    return candidate;
}

TEST(EncoderAutoScreenGrabTest, MonitorGpuWithAnEncoderWinsAndIsTheOnlyOneProbed) {
    // Hybrid laptop: NVIDIA dGPU first in high-performance order, Intel iGPU
    // drives the panel.
    const std::vector<ce::media::AdapterCandidate> adapters = {Adapter(ce::media::kVendorNvidia, false),
                                                               Adapter(ce::media::kVendorIntel, true)};
    std::vector<size_t> asked;
    const auto choice = ce::media::ChooseScreenGrabAdapter(adapters, 0, [&](size_t index) {
        asked.push_back(index);
        return true;
    });
    EXPECT_EQ(choice.index, 1u);
    EXPECT_STREQ(choice.reason, "auto-encoder-drives-monitor");
    EXPECT_FALSE(choice.crossAdapterCapture);
    EXPECT_FALSE(choice.autoEncoderMissing);
    EXPECT_EQ(asked, std::vector<size_t>{1});
}

TEST(EncoderAutoScreenGrabTest, MonitorGpuWithoutAnEncoderIsPassedOver) {
    const std::vector<ce::media::AdapterCandidate> adapters = {Adapter(0x1414, false, true),
                                                               Adapter(ce::media::kVendorAmd, false),
                                                               Adapter(0x1234, true)};
    std::vector<size_t> asked;
    const auto choice = ce::media::ChooseScreenGrabAdapter(adapters, 0, [&](size_t index) {
        asked.push_back(index);
        return index == 1;
    });
    EXPECT_EQ(choice.index, 1u);
    EXPECT_STREQ(choice.reason, "auto-encoder-high-performance");
    EXPECT_TRUE(choice.crossAdapterCapture);
    EXPECT_EQ(asked, (std::vector<size_t>{2, 1}));  // software adapters are never probed
}

TEST(EncoderAutoScreenGrabTest, NoEncoderAnywhereKeepsTheVendorFreeChoice) {
    const std::vector<ce::media::AdapterCandidate> adapters = {Adapter(ce::media::kVendorNvidia, false),
                                                               Adapter(ce::media::kVendorIntel, true)};
    const auto choice = ce::media::ChooseScreenGrabAdapter(adapters, 0, [](size_t) { return false; });
    EXPECT_EQ(choice.index, 1u);
    EXPECT_STREQ(choice.reason, "drives-monitor");
    EXPECT_TRUE(choice.autoEncoderMissing);
}

// --- Live streaming keeps auto, restricted to H.264 (live_stream_config.cpp) ---

TEST(EncoderAutoLiveStreamTest, AutoRequestsStreamWithTheBestH264EncoderOfTheGpu) {
    for (const char* configured : {"auto", "AUTO", "hevc_auto", "av1_auto", "h264_auto"}) {
        VideoConfig video{};
        video.encoder = configured;
        video.fps = 60;
        AudioConfig system{};
        system.enabled = true;
        std::vector<AudioConfig> audio{system};
        ce::live_stream::ProfileSettings settings;
        settings.service = ce::live_stream::Service::kTwitch;
        settings.destination = "rtmps://ingest.example/app/test_key";
        std::string error;
        ASSERT_TRUE(ce::live_stream::ApplyProfile(settings, &video, &audio, &error)) << configured << ": " << error;
        EXPECT_EQ(video.encoder, "h264_auto") << configured;
    }
}

}  // namespace
