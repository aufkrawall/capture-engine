#include <gtest/gtest.h>

#include "../common/rate_window_utils.h"

namespace rate_window = ce::rate_window;

TEST(RateWindowUtilsTest, SlidingRateWindowTracksRecentRateWithoutDoubleCounting) {
    rate_window::SlidingRateWindow<> window;

    for (uint64_t t = 0; t < 1000; t += 50) {
        window.AddSample(t, 6);
    }

    EXPECT_EQ(window.RatePerSecond(950, 1000), 120u);
    EXPECT_EQ(window.MinRatePerSecond(950, 250, 1000), 120u);
    EXPECT_EQ(window.MinRatePerSecond(950, 500, 1000), 120u);
}

TEST(RateWindowUtilsTest, SlidingRateWindowDetectsShortUnderfeedWindows) {
    rate_window::SlidingRateWindow<> window;

    for (uint64_t t = 0; t < 1000; t += 50) {
        const uint32_t count = (t >= 500 && t < 750) ? 4u : 6u;
        window.AddSample(t, count);
    }

    EXPECT_EQ(window.RatePerSecond(950, 1000), 110u);
    EXPECT_EQ(window.MinRatePerSecond(950, 250, 1000), 80u);
    EXPECT_EQ(window.MinRatePerSecond(950, 500, 1000), 100u);
}

TEST(RateWindowUtilsTest, SlidingRateWindowResetClearsHistory) {
    rate_window::SlidingRateWindow<> window;

    for (uint64_t t = 0; t < 500; t += 50) {
        window.AddSample(t, 6);
    }

    EXPECT_GT(window.RatePerSecond(450, 500), 0u);
    window.Reset();
    EXPECT_EQ(window.RatePerSecond(450, 500), 0u);
    EXPECT_EQ(window.MinRatePerSecond(450, 250, 500), 0u);
}
