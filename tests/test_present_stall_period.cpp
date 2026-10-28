#include "hook/present/present_stall_period.h"

#include <gtest/gtest.h>

#include <cmath>
#include <string>
#include <vector>

using namespace ce::present_stall;

namespace {

// Deterministic jitter: the tests must not depend on a seed source.
struct Lcg {
    uint64_t state = 0x9E3779B97F4A7C15ull;
    double Next() {  // [0, 1)
        state = state * 6364136223846793005ull + 1442695040888963407ull;
        return static_cast<double>(state >> 11) / 9007199254740992.0;
    }
};

// A 140 fps game's down-call entries over `seconds`, each within +-300 us of its slot.
std::vector<int64_t> Calls(double seconds, Lcg& lcg) {
    std::vector<int64_t> calls;
    const double intervalUs = 1e6 / 140.0;
    for (double t = 0; t < seconds * 1e6; t += intervalUs) {
        calls.push_back(1'000'000 + static_cast<int64_t>(t + (lcg.Next() - 0.5) * 600.0));
    }
    return calls;
}

// The call in flight when each tick of an outside clock fires stalls; `share` of the ticks hit.
std::vector<int64_t> HitByClock(const std::vector<int64_t>& calls, double periodUs, double share, Lcg& lcg) {
    std::vector<int64_t> slow;
    size_t next = 0;
    for (double tick = static_cast<double>(calls.front()) + 3100.0; tick < static_cast<double>(calls.back());
         tick += periodUs) {
        while (next + 1 < calls.size() && static_cast<double>(calls[next + 1]) <= tick) {
            ++next;
        }
        if (lcg.Next() < share) {
            slow.push_back(calls[next]);
        }
    }
    return slow;
}

}  // namespace

// HotS 2026-10-28: a USB microphone flagging a discontinuity every 40 ms stalled
// the Present of whichever frame was in flight.
TEST(PresentStallPeriodTest, SlowCallsHitByA25HzDeviceClockAreAnOutsideClock) {
    Lcg lcg;
    const std::vector<int64_t> calls = Calls(10.0, lcg);
    const std::vector<int64_t> slow = HitByClock(calls, 40'000.0, 0.5, lcg);
    const PeriodLock lock = FindPeriodLock(calls, slow);
    ASSERT_TRUE(lock.analyzed);
    ASSERT_GE(lock.peakCount, 1u);
    EXPECT_TRUE(lock.locked);
    EXPECT_NEAR(lock.peaks[0].periodMs, 40.0, 0.1);
    EXPECT_FALSE(lock.peaks[0].gameCadence);
    EXPECT_LT(lock.peaks[0].controlR, kPeriodLockMaxControlR);
    EXPECT_GE(lock.z, kPeriodLockMinZ);
    EXPECT_NEAR(lock.peaks[0].callsPerPeriod, 40.0 / (1000.0 / 140.0), 0.1);

    char line[512];
    FormatPeriodLock(lock, 10'000, 0, line, sizeof(line));
    EXPECT_NE(std::string(line).find("lock=OUTSIDE CLOCK 40.0"), std::string::npos) << line;
}

// A stall every Nth frame is the game's own (or CE's per-frame) work: every call
// spreads evenly over that period, so only the whole call count gives it away.
TEST(PresentStallPeriodTest, AStallEveryFourthFrameIsTheGamesCadence) {
    Lcg lcg;
    const std::vector<int64_t> calls = Calls(10.0, lcg);
    std::vector<int64_t> slow;
    for (size_t i = 0; i < calls.size(); i += 4) {
        slow.push_back(calls[i]);
    }
    const PeriodLock lock = FindPeriodLock(calls, slow);
    ASSERT_TRUE(lock.analyzed);
    ASSERT_GE(lock.peakCount, 1u);
    EXPECT_FALSE(lock.locked);
    EXPECT_TRUE(lock.peaks[0].gameCadence);
    EXPECT_NEAR(lock.peaks[0].callsPerPeriod, std::round(lock.peaks[0].callsPerPeriod), kPeriodLockWholeCallsSlack);

    char line[512];
    FormatPeriodLock(lock, 10'000, 0, line, sizeof(line));
    EXPECT_NE(std::string(line).find("lock=game cadence"), std::string::npos) << line;
}

TEST(PresentStallPeriodTest, RandomlySlowCallsFollowNoClock) {
    Lcg lcg;
    const std::vector<int64_t> calls = Calls(10.0, lcg);
    std::vector<int64_t> slow;
    for (const int64_t call : calls) {
        if (lcg.Next() < 0.1) {
            slow.push_back(call);
        }
    }
    const PeriodLock lock = FindPeriodLock(calls, slow);
    ASSERT_TRUE(lock.analyzed);
    EXPECT_FALSE(lock.locked) << lock.peaks[0].periodMs << "ms R=" << lock.peaks[0].r << " Z=" << lock.z;
}

TEST(PresentStallPeriodTest, TooFewSlowCallsAreNotTested) {
    Lcg lcg;
    const std::vector<int64_t> calls = Calls(10.0, lcg);
    const std::vector<int64_t> slow(calls.begin(), calls.begin() + (kPeriodLockMinSlowCalls - 1));
    const PeriodLock lock = FindPeriodLock(calls, slow);
    EXPECT_FALSE(lock.analyzed);
    EXPECT_FALSE(lock.locked);
    char line[256];
    FormatPeriodLock(lock, 10'000, 0, line, sizeof(line));
    EXPECT_NE(std::string(line).find("lock=untested"), std::string::npos) << line;
}

TEST(PresentStallPeriodTest, TheRecorderHandsOverEveryCallSortedWithTheSlowOnesApart) {
    PeriodRecorder recorder;
    recorder.Note(3'000, false);
    recorder.Note(1'000, true);  // a second present thread finishing later
    recorder.Note(2'000, true);
    recorder.Note(0, true);  // no clock reading: dropped
    std::vector<int64_t> calls;
    std::vector<int64_t> slow;
    EXPECT_EQ(recorder.Drain(calls, slow), 0u);
    EXPECT_EQ(calls, (std::vector<int64_t>{1'000, 2'000, 3'000}));
    EXPECT_EQ(slow, (std::vector<int64_t>{1'000, 2'000}));

    calls.clear();
    slow.clear();
    EXPECT_EQ(recorder.Drain(calls, slow), 0u);
    EXPECT_TRUE(calls.empty());
}

TEST(PresentStallPeriodTest, ALappedRecorderKeepsTheNewestCallsAndCountsTheRest) {
    PeriodRecorder recorder;
    const size_t noted = PeriodRecorder::kCapacity + 100;
    for (size_t i = 1; i <= noted; ++i) {
        recorder.Note(static_cast<int64_t>(i), false);
    }
    std::vector<int64_t> calls;
    std::vector<int64_t> slow;
    const uint64_t lapped = recorder.Drain(calls, slow);
    ASSERT_FALSE(calls.empty());
    EXPECT_EQ(lapped + calls.size(), noted);
    EXPECT_EQ(calls.back(), static_cast<int64_t>(noted));
    EXPECT_LT(calls.size(), PeriodRecorder::kCapacity);
}
