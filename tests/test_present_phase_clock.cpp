#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <string>

#include "hook/metrics/present_phase_clock.h"
#include "source_fragment_reader.h"

// The phase clock behind a present's debug sample (the "[PERF] DX12 Present summary" line) and DX11 CSV row.
// Until 2026-10-17 the wrapper's D3D12 Present gated every start stamp by the DX11-only CSV flag while the
// sample's fields were written unconditionally: metrics_update, process_frame_external, fps_limiter and
// present_call came out as "now - 0" (the absolute QPC in microseconds, cut to int32) and swapchain_acquire
// as 0. The wrapper's use of the clock is pinned against its source below.
namespace {

int64_t g_fakeNowUs = 0;
int g_clockReads = 0;

int64_t FakeClock() {
    ++g_clockReads;
    return g_fakeNowUs;
}

// The QPC in microseconds of a machine that has been up for 100 hours: nowhere near int32.
constexpr int64_t kUptimeUs = 360'000'000'000ll;

class PresentPhaseClockTest : public ::testing::Test {
protected:
    void SetUp() override {
        g_fakeNowUs = kUptimeUs;
        g_clockReads = 0;
    }
};

std::string ReadSource(const char* relative) {
    return ce::test_source::ReadLogicalSource(std::filesystem::current_path() / relative);
}

std::string Between(const std::string& text, const char* from, const char* to) {
    const size_t begin = text.find(from);
    const size_t end = begin == std::string::npos ? std::string::npos : text.find(to, begin);
    return begin == std::string::npos || end == std::string::npos ? std::string() : text.substr(begin, end - begin);
}

}  // namespace

TEST_F(PresentPhaseClockTest, AFrameNobodyReadsCostsNoClockRead) {
    const ce::PresentPhaseClock clock(false, &FakeClock);
    EXPECT_FALSE(clock.Enabled());
    const int64_t start = clock.Now();
    g_fakeNowUs += 500;
    EXPECT_EQ(start, 0);
    EXPECT_EQ(clock.ElapsedUs(start), 0);
    EXPECT_EQ(clock.ElapsedUs(kUptimeUs), 0) << "even a stamp from another clock measures nothing";
    EXPECT_EQ(g_clockReads, 0);
}

TEST_F(PresentPhaseClockTest, AStampedPhaseMeasuresItsDurationNotTheAbsoluteClock) {
    const ce::PresentPhaseClock clock(true, &FakeClock);
    const int64_t start = clock.Now();
    EXPECT_EQ(start, kUptimeUs);
    g_fakeNowUs += 1234;
    EXPECT_EQ(clock.ElapsedUs(start), 1234);
}

TEST_F(PresentPhaseClockTest, APhaseThatWasNeverStampedMeasuresZeroNotNowMinusZero) {
    // The pre-fix D3D12 sample: the stamp was 0 (gated off) while the subtraction ran anyway.
    const ce::PresentPhaseClock clock(true, &FakeClock);
    EXPECT_NE(static_cast<int32_t>(kUptimeUs - 0), 0) << "the old composition read this as a duration";
    EXPECT_EQ(clock.ElapsedUs(0), 0);
}

// The wrapper's Present on a D3D12 swapchain: no CSV row (perfLoggingEnabled is DX11-only), a debug sample.
TEST_F(PresentPhaseClockTest, ASampledD3D12PresentTimesEveryPhaseOfItsSample) {
    const bool perfLoggingEnabled = false;
    const bool sampleThisFrame = true;
    const ce::PresentPhaseClock clock(perfLoggingEnabled || sampleThisFrame, &FakeClock);

    const int64_t acquireStart = clock.Now();
    g_fakeNowUs += 3;
    const int32_t acquire = clock.ElapsedUs(acquireStart);
    const int64_t metricsStart = clock.Now();
    g_fakeNowUs += 7;
    const int32_t metrics = clock.ElapsedUs(metricsStart);
    const int64_t processFrameStart = clock.Now();
    g_fakeNowUs += 900;
    const int32_t processFrame = clock.ElapsedUs(processFrameStart);
    const int64_t limiterStart = clock.Now();
    g_fakeNowUs += 4000;
    const int32_t limiter = clock.ElapsedUs(limiterStart);
    const int64_t presentCallStart = clock.Now();
    g_fakeNowUs += 12000;
    const int32_t presentCall = clock.ElapsedUs(presentCallStart);

    EXPECT_EQ(acquire, 3);
    EXPECT_EQ(metrics, 7);
    EXPECT_EQ(processFrame, 900);
    EXPECT_EQ(limiter, 4000);
    EXPECT_EQ(presentCall, 12000);
}

TEST_F(PresentPhaseClockTest, AnUnsampledD3D12PresentReadsNoClock) {
    const bool perfLoggingEnabled = false;  // DX11-only
    const bool sampleThisFrame = false;
    const ce::PresentPhaseClock clock(perfLoggingEnabled || sampleThisFrame, &FakeClock);
    const int64_t start = clock.Now();
    g_fakeNowUs += 50;
    EXPECT_EQ(clock.ElapsedUs(start), 0);
    EXPECT_EQ(g_clockReads, 0);
}

TEST(PresentPhaseClockSourceTest, TheWrappersPresentStampsPhasesForItsDebugSampleToo) {
    constexpr size_t npos = std::string::npos;
    const std::string body = Between(
        ReadSource("hook/wrappers/dxgi_swapchain_wrap_present.cpp"),
        "HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::Present(UINT SyncInterval, UINT Flags) {", "\n}\n");
    ASSERT_FALSE(body.empty());

    // The old flag: the CSV's (DX11-only) switch alone gated clocks that a D3D12 sample's fields consume.
    EXPECT_EQ(body.find("phaseTimingEnabled"), npos);
    const size_t sampleDecision = body.find(
        "const bool sampleThisFrame = debugFrameNum != 0 && "
        "PerfLogger::Get().ShouldSampleDetailedFrame(debugFrameNum);");
    const size_t clockDecl =
        body.find("const ce::PresentPhaseClock phaseClock(perfLoggingEnabled || sampleThisFrame);");
    const size_t acquireStart = body.find("const int64_t acquireStartUs = phaseClock.Now();");
    const size_t lock = body.find("std::lock_guard<std::mutex> lock(m_ResourceLock);");
    ASSERT_NE(sampleDecision, npos);
    ASSERT_NE(clockDecl, npos);
    ASSERT_NE(acquireStart, npos);
    ASSERT_NE(lock, npos);
    EXPECT_LT(sampleDecision, clockDecl) << "the clock needs to know whether a sample is owed";
    EXPECT_LT(clockDecl, acquireStart);
    EXPECT_LT(acquireStart, lock) << "the acquire is timed from before the lock";
    EXPECT_NE(body.find("(Flags & DXGI_PRESENT_TEST) == 0 && PerfLogger::Get().IsEnabled()"), npos)
        << "a status probe takes no sample tick";

    // Every sample field the wrapper writes is a stamped phase, never `GetQpcUs() - <maybe-unset stamp>`.
    for (const char* assignment :
         {"activeDebugSample->swapchainAcquireUs = acquireUs;",
          "activeDebugSample->metricsUpdateUs = phaseClock.ElapsedUs(metricsUpdateStartUs);",
          "activeDebugSample->processFrameExternalUs = phaseClock.ElapsedUs(processFrameStartUs);",
          "activeDebugSample->fpsLimiterUs = phaseClock.ElapsedUs(limiterStartUs);",
          "activeDebugSample->presentCallUs = phaseClock.ElapsedUs(presentCallStartUs);",
          "const int32_t acquireUs = phaseClock.ElapsedUs(acquireStartUs);"}) {
        EXPECT_NE(body.find(assignment), npos) << assignment;
    }
    for (const char* stamp : {"const int64_t metricsUpdateStartUs = phaseClock.Now();",
                              "const int64_t processFrameStartUs = phaseClock.Now();",
                              "const int64_t limiterStartUs = phaseClock.Now();",
                              "const int64_t presentCallStartUs = phaseClock.Now();"}) {
        EXPECT_NE(body.find(stamp), npos) << stamp;
    }
}
