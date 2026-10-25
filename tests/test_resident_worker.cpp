#include <gtest/gtest.h>

#include "hook/runtime/resident_worker.h"

namespace {

bool g_dormant = false;
bool FakeDormancy() {
    return g_dormant;
}

class ResidentWorkerLifecycleTest : public ::testing::Test {
protected:
    void SetUp() override {
        g_dormant = false;
    }
    void TearDown() override {
        g_dormant = false;
    }
    ce::resident_worker::Lifecycle worker{&FakeDormancy};
};

}  // namespace

// Session 20261025_212736: the collector and the watchdog left their loops when the host stopped and
// nothing restarted them after the reconnect (adapterLuid=0x0 for the whole session).
TEST_F(ResidentWorkerLifecycleTest, DormancyParksTheRunAndReactivationResumesItOnce) {
    const uint64_t run = worker.BeginRun();
    ASSERT_NE(run, 0u);
    EXPECT_TRUE(worker.Continue(run));

    g_dormant = true;
    EXPECT_FALSE(worker.Continue(run));
    worker.EndRun(run);
    EXPECT_TRUE(worker.IsParked());
    EXPECT_FALSE(worker.IsRunning());

    g_dormant = false;
    const uint64_t resumed = worker.BeginResume();
    EXPECT_NE(resumed, 0u);
    EXPECT_NE(resumed, run);
    EXPECT_TRUE(worker.IsRunning());
    EXPECT_TRUE(worker.Continue(resumed));
    EXPECT_EQ(worker.BeginResume(), 0u) << "a running worker is resumed only once";
}

// The parked thread may still be unwinding when the replacement run starts: its late EndRun must not
// mark the new run finished, and its stale generation never continues.
TEST_F(ResidentWorkerLifecycleTest, LateExitOfTheParkedRunCannotEndItsReplacement) {
    const uint64_t run = worker.BeginRun();
    g_dormant = true;
    ASSERT_FALSE(worker.Continue(run));
    g_dormant = false;

    const uint64_t resumed = worker.BeginResume();
    ASSERT_NE(resumed, 0u);
    worker.EndRun(run);
    EXPECT_TRUE(worker.IsRunning());
    EXPECT_FALSE(worker.Continue(run));
    EXPECT_TRUE(worker.Continue(resumed));
}

// A reactivation that arrives before the worker looked at dormancy again finds it running; the worker
// then reads dormancy as already cleared and simply continues - no second thread.
TEST_F(ResidentWorkerLifecycleTest, WorkerThatNeverSawTheDormantWindowKeepsRunning) {
    const uint64_t run = worker.BeginRun();
    g_dormant = true;
    g_dormant = false;  // the host restarted within one loop period
    EXPECT_EQ(worker.BeginResume(), 0u);
    EXPECT_TRUE(worker.Continue(run));
    EXPECT_TRUE(worker.IsRunning());
}

TEST_F(ResidentWorkerLifecycleTest, StoppedAndOneShotRunsAreNotResumed) {
    const uint64_t stopped = worker.BeginRun();
    worker.RequestStop();
    EXPECT_FALSE(worker.Continue(stopped));
    worker.EndRun(stopped);
    EXPECT_EQ(worker.BeginResume(), 0u) << "an owner's stop is final for reactivation";

    // The watchdog ends its run itself after writing a freeze dump.
    const uint64_t oneShot = worker.BeginRun();
    ASSERT_NE(oneShot, 0u);
    worker.EndRun(oneShot);
    EXPECT_FALSE(worker.IsRunning());
    EXPECT_EQ(worker.BeginResume(), 0u);
}

// A device re-created after a host restart starts the worker through the owner's start path: that
// start supersedes the parked run, and a later reactivation finds nothing left to resume.
TEST_F(ResidentWorkerLifecycleTest, OwnerStartSupersedesAParkedRun) {
    const uint64_t run = worker.BeginRun();
    EXPECT_EQ(worker.BeginRun(), 0u) << "one run at a time";
    g_dormant = true;
    ASSERT_FALSE(worker.Continue(run));
    g_dormant = false;

    const uint64_t restarted = worker.BeginRun();
    ASSERT_NE(restarted, 0u);
    EXPECT_EQ(worker.BeginResume(), 0u);
    EXPECT_TRUE(worker.Continue(restarted));
}

// A stop request issued while the worker was parked keeps it down.
TEST_F(ResidentWorkerLifecycleTest, StopWhileParkedIsFinal) {
    const uint64_t run = worker.BeginRun();
    g_dormant = true;
    ASSERT_FALSE(worker.Continue(run));
    worker.RequestStop();
    g_dormant = false;
    EXPECT_EQ(worker.BeginResume(), 0u);
    EXPECT_FALSE(worker.IsParked());
}
