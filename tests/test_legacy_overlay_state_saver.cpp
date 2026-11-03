#include <gtest/gtest.h>

#include <cstdint>
#include <map>
#include <set>
#include <tuple>
#include <vector>

#include "hook/overlay/legacy_overlay_state_saver.h"

// The D3D9/D3D8/D3D7 overlays save the application's state by hand: each
// state the draw changes is read once and written back afterwards, and only
// what actually changed. tests/test_dx9_overlay_backend.cpp checks the whole
// backend on a real D3D9 device; D3D8 and D3D7 have no 64-bit runtime, so the
// shared bookkeeping is checked here against a fake device.
namespace {

using CustomOverlay::LegacyOverlayStateKind;

using Key = std::tuple<LegacyOverlayStateKind, uint32_t, uint32_t>;

struct FakeDevice {
    std::map<Key, uint32_t> values;
    std::set<Key> unreadable;
    std::vector<Key> reads;
    std::vector<std::pair<Key, uint32_t>> writes;
};

struct FakeAccess {
    using Device = FakeDevice;
    static bool Read(Device* device, LegacyOverlayStateKind kind, uint32_t stage, uint32_t state, uint32_t* value) {
        const Key key{kind, stage, state};
        device->reads.push_back(key);
        if (device->unreadable.count(key))
            return false;
        *value = device->values[key];
        return true;
    }
    static bool Write(Device* device, LegacyOverlayStateKind kind, uint32_t stage, uint32_t state, uint32_t value) {
        const Key key{kind, stage, state};
        device->writes.emplace_back(key, value);
        device->values[key] = value;
        return true;
    }
};

using States = CustomOverlay::LegacyOverlayStates<FakeAccess>;

constexpr uint32_t kBlend = 27;
constexpr uint32_t kCull = 22;
constexpr uint32_t kColorOp = 1;

Key Render(uint32_t state) {
    return {LegacyOverlayStateKind::kRender, 0, state};
}

}  // namespace

TEST(LegacyOverlayStateSaverTest, RestoresTheApplicationsValueOfEachStateTheDrawChanged) {
    FakeDevice device;
    device.values[Render(kBlend)] = 0;
    device.values[Render(kCull)] = 3;
    device.values[{LegacyOverlayStateKind::kStage, 1, kColorOp}] = 4;

    States states(&device, true);
    states.Render(kBlend, 1);
    states.Render(kCull, 1);
    states.Stage(1, kColorOp, 1);
    EXPECT_EQ(device.values[Render(kBlend)], 1u);
    EXPECT_EQ(states.Saved(), 3u);

    states.Restore();
    EXPECT_EQ(device.values[Render(kBlend)], 0u);
    EXPECT_EQ(device.values[Render(kCull)], 3u);
    EXPECT_EQ((device.values[{LegacyOverlayStateKind::kStage, 1, kColorOp}]), 4u);
    EXPECT_EQ(states.Unread(), 0u);
}

TEST(LegacyOverlayStateSaverTest, AValueTheApplicationAlreadyHasIsWrittenNeitherWay) {
    FakeDevice device;
    device.values[Render(kBlend)] = 1;

    States states(&device, true);
    states.Render(kBlend, 1);
    states.Restore();
    EXPECT_TRUE(device.writes.empty());
}

TEST(LegacyOverlayStateSaverTest, EachStateIsReadOnceAndItsLastValueDecidesTheRestore) {
    FakeDevice device;
    device.values[{LegacyOverlayStateKind::kStage, 0, kColorOp}] = 4;

    // The draw loop switches stage 0 between its textured and untextured modes.
    States states(&device, true);
    states.Stage(0, kColorOp, 4);
    states.Stage(0, kColorOp, 2);
    states.Stage(0, kColorOp, 2);
    EXPECT_EQ(device.reads.size(), 1u);
    EXPECT_EQ(device.writes.size(), 1u) << "a repeated value is not written again";

    states.Restore();
    EXPECT_EQ((device.values[{LegacyOverlayStateKind::kStage, 0, kColorOp}]), 4u);

    // Back on the application's value by the end: nothing to restore.
    device.writes.clear();
    States again(&device, true);
    again.Stage(0, kColorOp, 2);
    again.Stage(0, kColorOp, 4);
    device.writes.clear();
    again.Restore();
    EXPECT_TRUE(device.writes.empty());
}

TEST(LegacyOverlayStateSaverTest, KindsAndStagesAreSeparateStates) {
    FakeDevice device;
    device.values[{LegacyOverlayStateKind::kStage, 0, kColorOp}] = 4;
    device.values[{LegacyOverlayStateKind::kStage, 1, kColorOp}] = 5;
    device.values[{LegacyOverlayStateKind::kSampler, 0, kColorOp}] = 6;

    States states(&device, true);
    states.Stage(0, kColorOp, 1);
    states.Stage(1, kColorOp, 1);
    states.Sampler(0, kColorOp, 1);
    EXPECT_EQ(states.Saved(), 3u);
    states.Restore();
    EXPECT_EQ((device.values[{LegacyOverlayStateKind::kStage, 0, kColorOp}]), 4u);
    EXPECT_EQ((device.values[{LegacyOverlayStateKind::kStage, 1, kColorOp}]), 5u);
    EXPECT_EQ((device.values[{LegacyOverlayStateKind::kSampler, 0, kColorOp}]), 6u);
}

TEST(LegacyOverlayStateSaverTest, AStateWhoseValueCannotBeReadIsLeftAlone) {
    FakeDevice device;
    device.values[Render(kBlend)] = 7;
    device.unreadable.insert(Render(kBlend));

    States states(&device, true);
    states.Render(kBlend, 1);
    EXPECT_EQ(device.values[Render(kBlend)], 7u) << "never changed without a value to put back";
    EXPECT_EQ(states.Unread(), 1u);
    states.Restore();
    EXPECT_TRUE(device.writes.empty());
}

TEST(LegacyOverlayStateSaverTest, BeyondCapacityStatesAreLeftAlone) {
    FakeDevice device;
    States states(&device, true);
    for (uint32_t state = 0; state < States::kCapacity + 2; ++state)
        states.Render(state, 1);
    EXPECT_EQ(states.Saved(), States::kCapacity);
    EXPECT_EQ(states.Unread(), 2u);
    EXPECT_EQ(device.values[Render(States::kCapacity)], 0u);
    states.Restore();
    for (uint32_t state = 0; state < States::kCapacity + 2; ++state)
        EXPECT_EQ(device.values[Render(state)], 0u) << state;
}

TEST(LegacyOverlayStateSaverTest, AnUnreadableDeviceIsWrittenThroughAndRestoredByItsBlock) {
    // A pure device: the backend saves it with a D3DSBT_ALL block instead.
    FakeDevice device;
    States states(&device, false);
    states.Render(kBlend, 1);
    states.Render(kBlend, 1);
    EXPECT_TRUE(device.reads.empty());
    EXPECT_EQ(device.writes.size(), 2u);
    device.writes.clear();
    states.Restore();
    EXPECT_TRUE(device.writes.empty());
    EXPECT_EQ(states.Unread(), 0u);
}
