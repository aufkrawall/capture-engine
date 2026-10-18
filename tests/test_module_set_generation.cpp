#include <gtest/gtest.h>

#include <atomic>

#include "hook/runtime/module_set_generation.h"

// Per-frame module checks (DXVK's version resource behind the D3D11 overlay's API name) keep their
// answer until a DLL loads or unloads, and check every time when no loader notifications exist.
namespace {

TEST(ModuleSetGenerationTest, AnAnswerIsKeptUntilTheModuleSetChanges) {
    std::atomic<unsigned long long> generation{0};
    std::atomic<bool> answer{false};
    int computed = 0;
    bool value = true;
    const auto compute = [&] {
        ++computed;
        return value;
    };
    for (int frame = 0; frame < 100; ++frame)
        EXPECT_TRUE(CachedForModuleSet(7, generation, answer, compute));
    EXPECT_EQ(computed, 1);
    // A module came or went: checked again, and the new answer kept.
    value = false;
    EXPECT_FALSE(CachedForModuleSet(8, generation, answer, compute));
    EXPECT_FALSE(CachedForModuleSet(8, generation, answer, compute));
    EXPECT_EQ(computed, 2);
}

TEST(ModuleSetGenerationTest, WithoutNotificationsEveryCallChecks) {
    std::atomic<unsigned long long> generation{0};
    std::atomic<bool> answer{false};
    int computed = 0;
    const auto compute = [&] {
        ++computed;
        return true;
    };
    for (int frame = 0; frame < 3; ++frame)
        EXPECT_TRUE(CachedForModuleSet(0, generation, answer, compute));
    EXPECT_EQ(computed, 3);
}

}  // namespace
