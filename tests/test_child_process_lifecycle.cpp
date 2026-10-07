#include <gtest/gtest.h>

#include "captureengine/app/child_process_lifecycle.h"

#include <functional>
#include <stdexcept>
#include <unordered_map>

namespace {
using namespace ce::runtime::detail;

class FakeChildren final : public ChildProcessEffects {
public:
    ChildToken Spawn(ChildRole role) override {
        ++spawns;
        if (failSpawn)
            return 0;
        const ChildToken token = ++lastToken;
        alive[token] = true;
        channels[static_cast<size_t>(role)] = true;
        if (onSpawn)
            onSpawn();
        return token;
    }
    bool Running(ChildToken token) const override {
        const auto it = alive.find(token);
        EXPECT_NE(it, alive.end()) << "a closed/reused identity must not be observed";
        return it != alive.end() && it->second;
    }
    bool Connected(ChildRole role) const override {
        return channels[static_cast<size_t>(role)];
    }
    void Disconnect(ChildRole role) override {
        channels[static_cast<size_t>(role)] = false;
    }
    void RequestShutdown(ChildRole) override {
        ++shutdownRequests;
        if (onShutdown)
            onShutdown();
    }
    void Close(ChildToken token) override {
        EXPECT_FALSE(Running(token)) << "retirement alone does not prove process exit";
        ++closes;
        alive.erase(token);
    }
    bool Terminate(ChildToken token) override {
        ++terminations;
        if (allowTermination)
            alive[token] = false;
        return allowTermination;
    }
    uint64_t NowMs() const override {
        return now;
    }
    ChildWait Wait(const ChildToken* tokens, size_t count, uint32_t timeout) override {
        ++waits;
        for (size_t i = 0; i < count; ++i)
            EXPECT_TRUE(Running(tokens[i])) << "exited peers must be collected before waiting again";
        lastWait.assign(tokens, tokens + count);
        if (onWait)
            return onWait();
        EXPECT_NE(timeout, UINT32_MAX) << "failed termination must not cause an infinite wait";
        now += timeout;
        return ChildWait::Timeout;
    }
    void PumpMessages() override {
        if (onPump)
            onPump();
    }
    bool AcceptingWork() const override {
        return accepting;
    }

    std::unordered_map<ChildToken, bool> alive;
    std::array<bool, 4> channels{};
    std::vector<ChildToken> lastWait;
    std::function<void()> onSpawn;
    std::function<void()> onPump;
    std::function<void()> onShutdown;
    std::function<ChildWait()> onWait;
    ChildToken lastToken = 0;
    uint64_t now = 0;
    unsigned spawns = 0;
    unsigned closes = 0;
    unsigned waits = 0;
    unsigned shutdownRequests = 0;
    unsigned terminations = 0;
    bool accepting = true;
    bool failSpawn = false;
    bool allowTermination = true;
};

class ChildProcessLifecycleTest : public ::testing::Test {
protected:
    void TearDown() override {
        effects.onSpawn = {};
        effects.onPump = {};
        effects.onShutdown = {};
        effects.onWait = {};
        effects.allowTermination = true;
        children.BeginShutdown();
        EXPECT_TRUE(children.Drain(0, true));
        EXPECT_TRUE(effects.alive.empty());
    }
    FakeChildren effects;
    ChildProcessLifecycle children{effects};
};

TEST_F(ChildProcessLifecycleTest, FailedSpawnCannotPublishReadinessOrAnActiveIdentity) {
    effects.failSpawn = true;
    EXPECT_FALSE(children.Ensure(ChildRole::Inject, 100));
    EXPECT_FALSE(children.Present(ChildRole::Inject));
    EXPECT_FALSE(children.Ready(ChildRole::Inject));
    effects.failSpawn = false;
    EXPECT_TRUE(children.Ensure(ChildRole::Inject, 100));
    EXPECT_EQ(effects.spawns, 2u);
}

TEST_F(ChildProcessLifecycleTest, FinalizingMediaStaysOwnedWhileAFreshAuthenticatedChildStarts) {
    ASSERT_TRUE(children.Ensure(ChildRole::Media, 100));
    const auto old = effects.lastToken;
    const auto oldGeneration = children.Generation(ChildRole::Media);
    children.Retire(ChildRole::Media);
    EXPECT_FALSE(children.Present(ChildRole::Media));
    EXPECT_FALSE(children.Ready(ChildRole::Media));
    EXPECT_TRUE(children.HasRetired(ChildRole::Media));
    EXPECT_EQ(effects.closes, 0u);
    ASSERT_TRUE(children.Ensure(ChildRole::Media, 100));
    EXPECT_GT(children.Generation(ChildRole::Media), oldGeneration);
    EXPECT_NE(effects.lastToken, old);
    EXPECT_TRUE(effects.alive.at(old));
    children.BeginShutdown();
    EXPECT_FALSE(children.Drain(10, false));
    EXPECT_EQ(effects.lastWait.size(), 2u) << "shutdown must include the previous finalizer";
    EXPECT_TRUE(children.Drain(0, true));
    EXPECT_EQ(effects.closes, 2u);
}

TEST_F(ChildProcessLifecycleTest, BrokenChannelCannotReplaceAStillRunningPeer) {
    ASSERT_TRUE(children.Ensure(ChildRole::Inject, 100));
    effects.channels[0] = false;
    EXPECT_FALSE(children.Ensure(ChildRole::Inject, 5000));
    EXPECT_EQ(effects.now, 2000u);
    EXPECT_EQ(effects.spawns, 1u);
    EXPECT_EQ(effects.closes, 0u);
}

TEST_F(ChildProcessLifecycleTest, ExitedPeerGetsAFreshBindingAndIsClosedOnce) {
    ASSERT_TRUE(children.Ensure(ChildRole::Inject, 100));
    effects.alive.at(effects.lastToken) = false;
    ASSERT_TRUE(children.Ensure(ChildRole::Inject, 100));
    EXPECT_EQ(effects.spawns, 2u);
    EXPECT_EQ(effects.closes, 1u);
    EXPECT_TRUE(children.Ready(ChildRole::Inject));
}

TEST_F(ChildProcessLifecycleTest, RetirementDuringMessagePumpingCancelsTheOldEnsure) {
    ASSERT_TRUE(children.Ensure(ChildRole::Media, 100));
    effects.channels[1] = false;
    effects.onWait = [] { return ChildWait::Messages; };
    effects.onPump = [&] { children.Retire(ChildRole::Media); };
    EXPECT_FALSE(children.Ensure(ChildRole::Media, 100));
    EXPECT_EQ(effects.spawns, 1u);
    EXPECT_FALSE(children.Present(ChildRole::Media));
    EXPECT_TRUE(children.HasRetired(ChildRole::Media));
}

TEST_F(ChildProcessLifecycleTest, ShutdownDuringSpawnRetainsTheLateSuccessWithoutResurrection) {
    effects.onSpawn = [&] { children.BeginShutdown(); };
    EXPECT_FALSE(children.Ensure(ChildRole::Inject, 100));
    EXPECT_FALSE(children.Present(ChildRole::Inject));
    EXPECT_TRUE(children.HasRetired(ChildRole::Inject));
    EXPECT_FALSE(children.Ensure(ChildRole::Media, 100));
    EXPECT_EQ(effects.spawns, 1u);
}

TEST_F(ChildProcessLifecycleTest, RecursiveEnsureCannotCreateASecondPeerDuringStartup) {
    bool reentered = false;
    effects.onSpawn = [&] {
        if (!reentered) {
            reentered = true;
            EXPECT_FALSE(children.Ensure(ChildRole::Inject, 100));
        }
    };
    ASSERT_TRUE(children.Ensure(ChildRole::Inject, 100));
    EXPECT_EQ(effects.spawns, 1u);
}

TEST_F(ChildProcessLifecycleTest, ShutdownWaitsOnlyOnLiveIdentitiesAndIncludesRetiredPeers) {
    ASSERT_TRUE(children.Ensure(ChildRole::Media, 100));
    children.Retire(ChildRole::Media);
    const auto old = effects.lastToken;
    ASSERT_TRUE(children.Ensure(ChildRole::Inject, 100));
    children.BeginShutdown();
    effects.alive.at(old) = false;
    effects.onWait = [&] {
        EXPECT_EQ(effects.lastWait.size(), 1u);
        effects.alive.at(effects.lastToken) = false;
        return ChildWait::Exited;
    };
    EXPECT_TRUE(children.Drain(100, false));
    EXPECT_EQ(effects.waits, 1u);
    EXPECT_EQ(effects.closes, 2u);
}

TEST_F(ChildProcessLifecycleTest, FailedTerminationRetainsOwnershipAndDoesNotWaitInfinitely) {
    ASSERT_TRUE(children.Ensure(ChildRole::Media, 100));
    children.BeginShutdown();
    effects.allowTermination = false;
    EXPECT_FALSE(children.Drain(0, true));
    EXPECT_EQ(effects.waits, 0u);
    EXPECT_EQ(effects.closes, 0u);
    EXPECT_TRUE(children.HasRetired(ChildRole::Media));
    effects.alive.at(effects.lastToken) = false;
    EXPECT_TRUE(children.Drain(0, false));
    EXPECT_EQ(effects.closes, 1u);
}

TEST_F(ChildProcessLifecycleTest, ThrowingShutdownEffectRetainsActiveOwnershipAndAllowsRetry) {
    ASSERT_TRUE(children.Ensure(ChildRole::Media, 100));
    effects.onShutdown = [] { throw std::runtime_error("controlled shutdown failure"); };
    EXPECT_THROW(children.BeginShutdown(), std::runtime_error);
    EXPECT_TRUE(children.Present(ChildRole::Media));
    EXPECT_FALSE(children.Ensure(ChildRole::Inject, 100));
    EXPECT_EQ(effects.closes, 0u);
    effects.onShutdown = {};
    children.BeginShutdown();
    EXPECT_FALSE(children.Present(ChildRole::Media));
    EXPECT_TRUE(children.Drain(0, true));
    EXPECT_TRUE(effects.alive.empty());
}
}  // namespace
