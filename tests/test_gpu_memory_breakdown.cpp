#include <gtest/gtest.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11_4.h>
#include <mmsystem.h>

#include <filesystem>
#include <string>

#include "common/platform/raii_helpers.h"
#include "mediaengine/engine/mediaengine.h"
#include "mediaengine/engine/mediaengine_instance.h"
#include "mediaengine/video/gpu_memory_breakdown.h"
#include "source_fragment_reader.h"

// [GpuMemory]: the per-component video memory line of a recording. The
// dxgi_dup run 20261016_190423 showed a P010 reservoir of 1519 MB inside a
// 4014 MB process total with nothing naming the rest.

namespace {

namespace gm = ce::gpu_memory;
constexpr uint64_t kMb = 1024ull * 1024ull;

TEST(GpuMemoryBreakdownTest, TextureEstimatesFollowTheFormat) {
    EXPECT_EQ(gm::EstimateTextureBytes(3840, 2160, DXGI_FORMAT_P010), 3840ull * 2160 * 3);
    EXPECT_EQ(gm::EstimateTextureBytes(3840, 2160, DXGI_FORMAT_NV12), 3840ull * 2160 * 3 / 2);
    EXPECT_EQ(gm::EstimateTextureBytes(3840, 2160, DXGI_FORMAT_R10G10B10A2_UNORM), 3840ull * 2160 * 4);
    EXPECT_EQ(gm::EstimateTextureBytes(3840, 2160, DXGI_FORMAT_B8G8R8A8_UNORM), 3840ull * 2160 * 4);
    EXPECT_EQ(gm::EstimateTextureBytes(3840, 2160, DXGI_FORMAT_R16G16B16A16_FLOAT), 3840ull * 2160 * 8);
    EXPECT_EQ(gm::EstimateTextureBytes(64, 32, DXGI_FORMAT_R32G32B32A32_FLOAT), 64ull * 32 * 16);
    EXPECT_EQ(gm::EstimateTextureBytes(16, 16, DXGI_FORMAT_B8G8R8A8_UNORM, 3), 16ull * 16 * 4 * 3);
    EXPECT_EQ(gm::TextureBytes(nullptr), 0u);
}

TEST(GpuMemoryBreakdownTest, UnaccountedIsTheProcessTotalMinusNamedComponents) {
    gm::Breakdown b;
    b.processLocal = 4014 * kMb;
    b.captureReservoir = 1519 * kMb;
    b.captureReservoirSlots = 64;
    b.captureOther = 32 * kMb;
    b.encoderFrames = 474 * kMb;
    b.encoderFrameCount = 20;
    b.repeat = 48 * kMb;
    b.conversion = 0;
    b.overlays = 1 * kMb;
    EXPECT_EQ(gm::AccountedBytes(b), (1519 + 32 + 474 + 48 + 1) * kMb);
    EXPECT_EQ(gm::UnaccountedBytes(b), static_cast<int64_t>((4014 - 2074) * kMb));
    // 20261016_192321: the split capture pool and the bridge's frame pool are
    // each counted twice by the process counter; the rest is small.
    b.countedTwice = (1519 + 474) * kMb;
    EXPECT_EQ(gm::PhysicalEstimateBytes(b), static_cast<int64_t>((4014 - 1993) * kMb));
    EXPECT_EQ(gm::UnaccountedBytes(b), static_cast<int64_t>((4014 - 1993 - 2074) * kMb));
    b.countedTwice = 0;
    // Estimates may exceed a process total that was partly demoted to system memory.
    b.processLocal = 1000 * kMb;
    EXPECT_LT(gm::UnaccountedBytes(b), 0);

    const std::string line = gm::FormatBreakdown(b, "stop");
    EXPECT_NE(line.find("[GpuMemory] stage=stop processLocal=1000MB countedTwice=0MB physical~1000MB"),
              std::string::npos)
        << line;
    EXPECT_NE(line.find("captureReservoir=1519MB (64 slots)"), std::string::npos) << line;
    EXPECT_NE(line.find("encoderFrames=474MB (20 frames)"), std::string::npos) << line;
    EXPECT_NE(line.find("unaccounted=-1074MB"), std::string::npos) << line;
}

TEST(GpuMemoryBreakdownTest, LogKeyMovesInSixtyFourMegabyteSteps) {
    gm::Breakdown b;
    b.processLocal = 4014 * kMb;
    b.captureReservoir = 1519 * kMb;
    const uint64_t key = gm::BreakdownLogKey(b);
    b.processLocal += 10 * kMb;  // driver jitter stays quiet
    EXPECT_EQ(gm::BreakdownLogKey(b), key);
    b.processLocal += 64 * kMb;
    EXPECT_NE(gm::BreakdownLogKey(b), key);
}

TEST(GpuMemoryBreakdownTest, PoolTexturesAreCountedOnceAndCapped) {
    gm::EncoderGpuMemoryState state;
    int textures[3] = {};
    state.TrackPoolTexture(&textures[0]);
    state.TrackPoolTexture(&textures[1]);
    state.TrackPoolTexture(&textures[0]);
    state.TrackPoolTexture(nullptr);
    EXPECT_EQ(state.poolTextures.size(), 2u);
    for (size_t i = 0; i < gm::EncoderGpuMemoryState::kMaxTrackedPoolTextures + 8; ++i)
        state.TrackPoolTexture(reinterpret_cast<const void*>(0x1000 + i * 16));
    EXPECT_EQ(state.poolTextures.size(), gm::EncoderGpuMemoryState::kMaxTrackedPoolTextures);
    state.ResetPool();
    EXPECT_TRUE(state.poolTextures.empty());
}

TEST(GpuMemoryBreakdownTest, CaptureReportReachesTheEncoderLine) {
    MediaEngine_ReportCaptureGpuMemory(1519 * kMb, 64, 32 * kMb, true);
    gm::Breakdown b;
    gm::LoadCaptureReport(DefaultMediaEngineInstance().captureGauges, &b);
    EXPECT_TRUE(b.captureSharedAcrossDevices);
    EXPECT_EQ(b.captureReservoir, 1519 * kMb);
    EXPECT_EQ(b.captureReservoirSlots, 64u);
    EXPECT_EQ(b.captureOther, 32 * kMb);
    MediaEngine_ReportCaptureGpuMemory(0, 0, 0, false);  // pool released
    gm::LoadCaptureReport(DefaultMediaEngineInstance().captureGauges, &b);
    EXPECT_FALSE(b.captureSharedAcrossDevices);
    EXPECT_EQ(b.captureReservoir, 0u);
    EXPECT_EQ(b.captureReservoirSlots, 0u);
}

TEST(GpuMemoryBreakdownTest, ProcessMemoryQueryWorksOnTheAdapter) {
    ce::ComGuard<ID3D11Device> device;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                                 device.addressof(), nullptr, nullptr)))
        GTEST_SKIP() << "no hardware D3D11 device";
    uint64_t local = 0;
    uint64_t nonLocal = 0;
    uint64_t budget = 0;
    ASSERT_TRUE(gm::QueryProcessVideoMemory(device.get(), &local, &nonLocal, &budget));
    EXPECT_GT(budget, 0u);
    EXPECT_FALSE(gm::QueryProcessVideoMemory(nullptr, &local, &nonLocal, &budget));
}

std::string CaptureSource(const char* name) {
    return ce::test_source::ReadLogicalSource(std::filesystem::current_path() / "captureengine" / "media" / name);
}

TEST(GpuMemoryBreakdownTest, CaptureResetKeepsTheRecordingSetup) {
    // ForceReset rebuilds WGCCapture::Impl; the retained-YUV choice, the
    // reservation mode and the memory report used to fall back to defaults
    // (RGB reservoir) after every capture recovery.
    const std::string source = CaptureSource("wgc_capture.cpp");
    const size_t reset = source.find("void WGCCapture::ForceReset()");
    ASSERT_NE(reset, std::string::npos);
    const std::string body = source.substr(reset, source.find("\nvoid WGCCapture::", reset + 1) - reset);
    EXPECT_NE(body.find("impl_->retainedYuvEnabled_ = retainedYuvEnabled;"), std::string::npos);
    EXPECT_NE(body.find("impl_->videoMemoryReservationMode_ = videoMemoryReservationMode;"), std::string::npos);
    EXPECT_NE(body.find("impl_->reportsGpuMemory_ = reportsGpuMemory;"), std::string::npos);
    // Only the recording capture reports; the screenshot capture keeps quiet.
    EXPECT_NE(CaptureSource("media_main_wgc.cpp").find("SetReportsGpuMemory(true)"), std::string::npos);
    EXPECT_EQ(CaptureSource("screenshot.cpp").find("SetReportsGpuMemory"), std::string::npos);
}

}  // namespace
