#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <vector>

#include "../hook/vulkan_layer/vulkan_sharpen_route_policy.h"
#include "../hook/vulkan_layer/vulkan_sharpen_state_registry.h"
#include "source_fragment_reader.h"

namespace {

using ce::vulkan_sharpen_route::Identity;
using ce::vulkan_sharpen_route::MeetsMinimumTargetSize;
using ce::vulkan_sharpen_route::MustRebuild;
using ce::vulkan_sharpen_route::Route;

std::string StripComments(const std::string& source) {
    std::string stripped;
    stripped.reserve(source.size());
    for (size_t index = 0; index < source.size();) {
        if (source.compare(index, 2, "//") == 0) {
            const size_t lineEnd = source.find('\n', index);
            if (lineEnd == std::string::npos)
                break;
            index = lineEnd;
            continue;
        }
        if (source.compare(index, 2, "/*") == 0) {
            const size_t blockEnd = source.find("*/", index + 2);
            if (blockEnd == std::string::npos)
                break;
            index = blockEnd + 2;
            continue;
        }
        stripped.push_back(source[index]);
        ++index;
    }
    return stripped;
}

Identity IdentityFor(uint64_t swapchain) {
    Identity identity;
    identity.swapchain = swapchain;
    identity.format = 44;
    identity.width = 3840;
    identity.height = 2160;
    identity.imageCount = 3;
    identity.queueFamily = 0;
    identity.route = Route::kGraphics;
    return identity;
}

std::string ReadLayerSource(const char* name) {
    return ce::test_source::ReadLogicalSource(std::filesystem::current_path() / "hook" / "vulkan_layer" / name);
}

}  // namespace

// VkQueue is externally synchronized, and on a borrowed-queue topology (a
// single graphics queue, or an async-present game that submits from one thread
// and presents from another) a CE submission racing a wrapper-locked game
// submission is driver UB - the QueueSubmit -4 / device-lost class
// (`20260922_235937`, `20260923_003755`). Every CE submit must therefore take
// ScopedBorrowedQueueSubmission around its down-call, exactly as the layer's
// own vkQueueSubmit wrappers do. This scans every translation unit in the
// layer: the three submits that used to run without the guard (the sharpen
// pass and the compute-present composite plus its fence re-arm) were found only
// by reading every call site.
TEST(VulkanSharpenLifetime, EveryQueueSubmitOutsideTheWrappersTakesTheBorrowedQueueGuard) {
    const std::filesystem::path layerDir = std::filesystem::current_path() / "hook" / "vulkan_layer";
    ASSERT_TRUE(std::filesystem::is_directory(layerDir)) << layerDir.string();

    std::string offenders;
    size_t submissions = 0;
    for (const auto& entry : std::filesystem::directory_iterator(layerDir)) {
        const std::string extension = entry.path().extension().string();
        if (extension != ".cpp" && extension != ".h")
            continue;
        const std::string name = entry.path().filename().string();
        // The wrappers themselves take the guard around the down-call they wrap.
        if (name == "layer_hooks.cpp")
            continue;
        const std::string code = StripComments(ce::test_source::ReadFile(entry.path()));
        for (size_t pos = code.find("fp_vkQueueSubmit("); pos != std::string::npos;
             pos = code.find("fp_vkQueueSubmit(", pos + 1)) {
            ++submissions;
            // Everything since the end of the previous function: a guard
            // declared in this submit's own scope has to appear in that window.
            const size_t windowBegin = code.rfind("\n}\n", pos);
            const size_t begin = windowBegin == std::string::npos ? 0 : windowBegin;
            const std::string window = code.substr(begin, pos - begin);
            if (window.find("ScopedBorrowedQueueSubmission") == std::string::npos)
                offenders += name + " ";
        }
    }

    EXPECT_GT(submissions, 0u);
    EXPECT_TRUE(offenders.empty()) << "queue submits outside a ScopedBorrowedQueueSubmission scope: " << offenders;
}

// A stand-in for SharpenState: what the registry moves around, plus whether its
// submissions have signalled (the non-blocking readiness test).
struct FakeState {
    bool initialized = false;
    Identity built = {};
    bool submissionsSignalled = false;
    int id = 0;
};
using FakeRegistry = ce::vulkan_sharpen_registry::Registry<FakeState, int>;

// The state was per device while a device may own several live swapchains:
// the second one either rebuilt the whole pipeline per present (with up to
// three one-second fence waits on the present thread) or, after the first
// fix, went unfiltered. Each swapchain now owns a state, so alternating
// presents of two swapchains both get filtered and never rebuild.
TEST(VulkanSharpenLifetime, TwoSwapchainsAlternatingPresentsBothFilterWithoutRebuilding) {
    constexpr int kDevice = 1;
    constexpr uint64_t kSwapchainA = 0x1AE6AA500D0ull;
    constexpr uint64_t kSwapchainB = 0x21EEFF200D0ull;

    FakeRegistry registry;
    size_t inits = 0;
    size_t rebuilds = 0;
    size_t filtered = 0;
    int nextId = 1;
    auto present = [&](uint64_t swapchain) {
        FakeState* state = &registry.Live(kDevice, swapchain);
        const Identity current = IdentityFor(swapchain);
        if (state->initialized && MustRebuild(state->built, current)) {
            ++rebuilds;
            registry.Retire(kDevice, swapchain);
            state = &registry.Live(kDevice, swapchain);
        }
        if (!state->initialized) {
            ++inits;
            state->initialized = true;
            state->built = current;
            state->id = nextId++;
        }
        ++filtered;
    };

    for (int frame = 0; frame < 8; ++frame)
        present(frame % 2 == 0 ? kSwapchainA : kSwapchainB);
    EXPECT_EQ(rebuilds, 0u) << "alternating presents of a second swapchain must not rebuild";
    EXPECT_EQ(inits, 2u) << "each swapchain builds its own state once";
    EXPECT_EQ(filtered, 8u) << "the second swapchain is filtered too";
    EXPECT_EQ(registry.LiveCount(kDevice), 2u);
    EXPECT_EQ(registry.RetiredCount(kDevice), 0u);
}

// A rebuild (present-queue family moved, as DOOM Eternal's "present from
// compute" does mid-swapchain) retires the old state instead of destroying it
// on the present thread; it is destroyed on a later present once its fences
// have signalled, never waited for.
TEST(VulkanSharpenLifetime, RebuildRetiresTheOldStateAndReapsItOnlyOnceSignalled) {
    constexpr int kDevice = 1;
    constexpr uint64_t kSwapchain = 0x1AE6AA500D0ull;
    FakeRegistry registry;
    FakeState& first = registry.Live(kDevice, kSwapchain);
    first.initialized = true;
    first.id = 1;

    registry.Retire(kDevice, kSwapchain);
    FakeState& second = registry.Live(kDevice, kSwapchain);
    EXPECT_FALSE(second.initialized) << "the rebuild starts from a fresh state";
    second.initialized = true;
    second.id = 2;
    EXPECT_EQ(registry.RetiredCount(kDevice), 1u);

    const auto ready = [](const FakeState& state) { return state.submissionsSignalled; };
    EXPECT_TRUE(registry.TakeReadyRetired(kDevice, ready).empty()) << "still in flight: kept, not waited for";
    EXPECT_EQ(registry.RetiredCount(kDevice), 1u);

    std::vector<FakeState> taken = registry.TakeReadyRetired(kDevice, [](const FakeState&) { return true; });
    ASSERT_EQ(taken.size(), 1u);
    EXPECT_EQ(taken[0].id, 1);
    EXPECT_EQ(registry.RetiredCount(kDevice), 0u);
    ASSERT_NE(registry.FindLive(kDevice, kSwapchain), nullptr);
    EXPECT_EQ(registry.FindLive(kDevice, kSwapchain)->id, 2) << "reaping never touches the live state";
}

// Swapchain destruction must release every state over it - the live one AND a
// retired one still waiting for its fences, since both hold views of its
// images - and nothing of another swapchain or device. Device teardown takes
// the rest.
TEST(VulkanSharpenLifetime, SwapchainDestroyTakesLiveAndRetiredStatesOfThatSwapchainOnly) {
    FakeRegistry registry;
    registry.Live(1, 0xA).id = 1;
    registry.Retire(1, 0xA);
    registry.Live(1, 0xA).id = 2;
    registry.Live(1, 0xB).id = 3;
    registry.Live(2, 0xA).id = 4;

    std::vector<FakeState> taken = registry.TakeForSwapchain(1, 0xA);
    ASSERT_EQ(taken.size(), 2u);
    EXPECT_EQ(registry.LiveCount(1), 1u);
    EXPECT_EQ(registry.RetiredCount(1), 0u);
    EXPECT_EQ(registry.LiveCount(2), 1u) << "the same handle value on another device is another swapchain";
    // A handle the driver reuses for the replacement swapchain starts fresh.
    EXPECT_EQ(registry.Live(1, 0xA).id, 0);

    registry.Retire(1, 0xB);
    EXPECT_EQ(registry.TakeForDevice(1).size(), 2u);
    EXPECT_EQ(registry.LiveCount(1), 0u);
    EXPECT_EQ(registry.RetiredCount(1), 0u);
    EXPECT_EQ(registry.LiveCount(2), 1u);
}

// Switching the filter off retires every live state on the device at once;
// they are released as their submissions finish.
TEST(VulkanSharpenLifetime, FilterOffRetiresEveryLiveStateOfTheDevice) {
    FakeRegistry registry;
    registry.Live(1, 0xA).initialized = true;
    registry.Live(1, 0xB).initialized = true;
    registry.Live(2, 0xC).initialized = true;
    registry.RetireAll(1);
    EXPECT_EQ(registry.LiveCount(1), 0u);
    EXPECT_EQ(registry.RetiredCount(1), 2u);
    EXPECT_EQ(registry.LiveCount(2), 1u);
}

// Decide's own floor is only 32 px, which let a tiny auxiliary swapchain build
// the full pipeline. The overlay's runtime-eligibility floor applies to the
// filter as well.
TEST(VulkanSharpenLifetime, TinyAuxiliarySwapchainsStayBelowTheFloor) {
    EXPECT_TRUE(MeetsMinimumTargetSize(320, 180));
    EXPECT_TRUE(MeetsMinimumTargetSize(3840, 2160));
    EXPECT_FALSE(MeetsMinimumTargetSize(319, 180));
    EXPECT_FALSE(MeetsMinimumTargetSize(320, 179));
    EXPECT_FALSE(MeetsMinimumTargetSize(0, 0));
}

// The present path keys its state by swapchain, reaps retired states first,
// and never tears a state down itself: every DestroySharpenState in the
// present path would be a fence wait inside the game's vkQueuePresentKHR.
TEST(VulkanSharpenLifetimeSourceTest, PresentPathKeysStatesPerSwapchainAndNeverWaits) {
    const std::string source = StripComments(ReadLayerSource("layer_sharpen.cpp"));
    ASSERT_FALSE(source.empty());
    const size_t entry = source.find("bool SharpenPresentedFrame(");
    ASSERT_NE(entry, std::string::npos);
    const size_t entryEnd = source.find("\n}\n", entry);
    ASSERT_NE(entryEnd, std::string::npos);
    const std::string present = source.substr(entry, entryEnd - entry);
    const size_t reap = present.find("ReapRetiredSharpenStatesLocked(device, disp)");
    const size_t live = present.find("layer_sharpen_g_Registry.Live(device, SharpenSwapchainKey(swapchain))");
    const size_t rebuild = present.find("ce::vulkan_sharpen_route::MustRebuild(");
    const size_t retire = present.find("layer_sharpen_g_Registry.Retire(device, SharpenSwapchainKey(swapchain))");
    ASSERT_NE(reap, std::string::npos);
    ASSERT_NE(live, std::string::npos);
    ASSERT_NE(rebuild, std::string::npos);
    ASSERT_NE(retire, std::string::npos);
    EXPECT_LT(reap, live);
    EXPECT_LT(rebuild, retire);
    EXPECT_NE(present.find("layer_sharpen_g_Registry.RetireAll(device)"), std::string::npos);
    EXPECT_EQ(present.find("DestroySharpenState("), std::string::npos) << "a teardown on the present thread waits";
    EXPECT_EQ(present.find("MustSkipUnownedSwapchain"), std::string::npos);
    EXPECT_NE(present.find("ce::vulkan_sharpen_route::MeetsMinimumTargetSize("), std::string::npos);

    // The reaper asks, it does not wait.
    const std::string setup = StripComments(ReadLayerSource("layer_sharpen_setup.cpp"));
    const size_t retired = setup.find("bool SharpenStateSubmissionsRetired(");
    ASSERT_NE(retired, std::string::npos);
    const std::string retiredBody = setup.substr(retired, setup.find("\n}\n", retired) - retired);
    EXPECT_NE(retiredBody.find("VK_TRUE, 0)"), std::string::npos);

    // The destroy path takes the live and the retired states of the swapchain.
    const size_t release = source.find("void ReleaseSharpenForSwapchain(");
    ASSERT_NE(release, std::string::npos);
    EXPECT_NE(source.find("layer_sharpen_g_Registry.TakeForSwapchain(device, SharpenSwapchainKey(swapchain))", release),
              std::string::npos);
}

// Per-swapchain states are only correct because every retirement path
// releases them. The `oldSwapchain` retirement is the handle-reuse half: NVIDIA may hand
// the replacement swapchain the old handle (DOOM Eternal `20260922_235937`), so
// the old state must be gone before the driver creates the new swapchain.
TEST(VulkanSharpenLifetimeSourceTest, OldSwapchainRetirementReleasesSharpenBeforeTheRecreate) {
    const std::string source = StripComments(ReadLayerSource("vulkan_layer_swapchain.cpp"));
    ASSERT_FALSE(source.empty());
    const size_t create = source.find("Capture_vkCreateSwapchainKHR(VkDevice device");
    ASSERT_NE(create, std::string::npos);
    const size_t oldSwapchain = source.find("if (pCreateInfo->oldSwapchain != VK_NULL_HANDLE)", create);
    const size_t release = source.find("ReleaseSharpenForSwapchain(oldSd->device, pCreateInfo->oldSwapchain)", create);
    const size_t driverCreate = source.find("fp_vkCreateSwapchainKHR(device, pFinalCI", create);
    ASSERT_NE(oldSwapchain, std::string::npos);
    ASSERT_NE(release, std::string::npos) << "the old state must retire before a handle-reusing recreate";
    ASSERT_NE(driverCreate, std::string::npos);
    EXPECT_LT(oldSwapchain, release);
    EXPECT_LT(release, driverCreate);
}
