#include <gtest/gtest.h>

#include <vector>

#include "mediaengine/engine/screen_grab_adapter.h"

// The shared screen-grab device used DXGI adapter 0. On a hybrid laptop that
// is the integrated GPU driving the panel, where NVENC cannot open; on a
// multi-GPU desktop the captured monitor may hang off another GPU.

namespace {

using ce::media::AdapterCandidate;
using ce::media::ChooseScreenGrabAdapter;
using ce::media::kVendorAmd;
using ce::media::kVendorIntel;
using ce::media::kVendorNvidia;

AdapterCandidate Adapter(uint32_t vendor, bool drivesMonitor, bool software = false) {
    AdapterCandidate candidate;
    candidate.vendorId = vendor;
    candidate.drivesTargetMonitor = drivesMonitor;
    candidate.software = software;
    return candidate;
}

constexpr uint32_t kVendorMicrosoft = 0x1414;

TEST(ScreenGrabAdapterTest, EncoderVendorsFollowTheWrapperSuffix) {
    EXPECT_EQ(ce::media::EncoderVendorId("av1_nvenc"), kVendorNvidia);
    EXPECT_EQ(ce::media::EncoderVendorId("hevc_amf"), kVendorAmd);
    EXPECT_EQ(ce::media::EncoderVendorId("h264_qsv"), kVendorIntel);
    EXPECT_EQ(ce::media::EncoderVendorId("h264_mf"), 0u);
    EXPECT_EQ(ce::media::EncoderVendorId("libx264"), 0u);
    EXPECT_EQ(ce::media::EncoderVendorId(""), 0u);
}

TEST(ScreenGrabAdapterTest, SingleGpuKeepsTheOnlyHardwareAdapter) {
    const std::vector<AdapterCandidate> adapters = {Adapter(kVendorNvidia, true),
                                                    Adapter(kVendorMicrosoft, false, true)};
    const auto choice = ChooseScreenGrabAdapter(adapters, kVendorNvidia);
    EXPECT_EQ(choice.index, 0u);
    EXPECT_FALSE(choice.crossAdapterCapture);
    EXPECT_FALSE(choice.encoderVendorMissing);
}

TEST(ScreenGrabAdapterTest, HybridLaptopPutsNvencOnTheDiscreteGpu) {
    // High-performance order: dGPU first, iGPU drives the panel.
    const std::vector<AdapterCandidate> adapters = {Adapter(kVendorNvidia, false), Adapter(kVendorIntel, true)};
    const auto nvenc = ChooseScreenGrabAdapter(adapters, kVendorNvidia);
    EXPECT_EQ(nvenc.index, 0u);
    EXPECT_TRUE(nvenc.crossAdapterCapture);  // unavoidable: the panel is on the iGPU
    // Quick Sync and vendor-neutral encoders stay on the GPU that drives the panel.
    EXPECT_EQ(ChooseScreenGrabAdapter(adapters, kVendorIntel).index, 1u);
    const auto neutral = ChooseScreenGrabAdapter(adapters, 0);
    EXPECT_EQ(neutral.index, 1u);
    EXPECT_FALSE(neutral.crossAdapterCapture);
}

TEST(ScreenGrabAdapterTest, ExternalMonitorOnTheDiscreteGpuAvoidsCrossAdapterCapture) {
    const std::vector<AdapterCandidate> adapters = {Adapter(kVendorNvidia, true), Adapter(kVendorIntel, false)};
    const auto choice = ChooseScreenGrabAdapter(adapters, kVendorNvidia);
    EXPECT_EQ(choice.index, 0u);
    EXPECT_FALSE(choice.crossAdapterCapture);
}

TEST(ScreenGrabAdapterTest, TwoGpusOfTheEncoderVendorPreferTheOneDrivingTheMonitor) {
    const std::vector<AdapterCandidate> adapters = {Adapter(kVendorNvidia, false), Adapter(kVendorNvidia, true)};
    EXPECT_EQ(ChooseScreenGrabAdapter(adapters, kVendorNvidia).index, 1u);
}

TEST(ScreenGrabAdapterTest, MissingEncoderVendorFallsBackToTheMonitorGpu) {
    const std::vector<AdapterCandidate> adapters = {Adapter(kVendorAmd, false), Adapter(kVendorIntel, true)};
    const auto choice = ChooseScreenGrabAdapter(adapters, kVendorNvidia);
    EXPECT_EQ(choice.index, 1u);
    EXPECT_TRUE(choice.encoderVendorMissing);
    EXPECT_FALSE(choice.crossAdapterCapture);
}

TEST(ScreenGrabAdapterTest, SoftwareAdaptersAreNeverChosen) {
    const std::vector<AdapterCandidate> onlySoftware = {Adapter(kVendorMicrosoft, true, true)};
    const auto choice = ChooseScreenGrabAdapter(onlySoftware, 0);
    EXPECT_EQ(choice.index, SIZE_MAX);
    EXPECT_EQ(ChooseScreenGrabAdapter({}, kVendorNvidia).index, SIZE_MAX);
}

}  // namespace
