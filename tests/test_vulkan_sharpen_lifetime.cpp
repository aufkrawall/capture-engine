#include <gtest/gtest.h>

#include <filesystem>
#include <string>

#include "../hook/vulkan_layer/vulkan_sharpen_route_policy.h"
#include "source_fragment_reader.h"

namespace {

using ce::vulkan_sharpen_route::Identity;
using ce::vulkan_sharpen_route::MeetsMinimumTargetSize;
using ce::vulkan_sharpen_route::MustRebuild;
using ce::vulkan_sharpen_route::MustSkipUnownedSwapchain;
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

// The state is per device while a device may own several live swapchains.
// Feeding a second swapchain's present to MustRebuild destroyed and rebuilt the
// whole pipeline per present - on the present thread, with up to three
// one-second fence waits in the teardown. The first live swapchain owns the
// pass and alternating presents of a second one must cost nothing.
TEST(VulkanSharpenLifetime, TwoSwapchainsAlternatingPresentsNeverRebuild) {
    constexpr uint64_t kSwapchainA = 0x1AE6AA500D0ull;
    constexpr uint64_t kSwapchainB = 0x21EEFF200D0ull;

    bool initialized = false;
    Identity built = {};
    size_t rebuilds = 0;
    size_t inits = 0;
    auto present = [&](uint64_t swapchain) {
        if (MustSkipUnownedSwapchain(initialized, built.swapchain, swapchain))
            return;
        const Identity current = IdentityFor(swapchain);
        if (initialized && MustRebuild(built, current)) {
            ++rebuilds;
            initialized = false;
        }
        if (!initialized) {
            ++inits;
            initialized = true;
            built = current;
        }
    };

    for (int frame = 0; frame < 8; ++frame)
        present(frame % 2 == 0 ? kSwapchainA : kSwapchainB);
    EXPECT_EQ(rebuilds, 0u) << "alternating presents of a second swapchain must not rebuild";
    EXPECT_EQ(inits, 1u) << "the first live swapchain wins";

    // The destroy hook releases the owning state and re-arms the choice, which
    // is also what keeps a handle-reusing recreate honest.
    initialized = false;
    present(kSwapchainB);
    EXPECT_EQ(rebuilds, 0u);
    EXPECT_EQ(inits, 2u) << "the surviving swapchain adopts the pass after the owner is destroyed";
}

TEST(VulkanSharpenLifetime, UnownedSwapchainPresentsSkipWhileTheOwnerIsLive) {
    EXPECT_TRUE(MustSkipUnownedSwapchain(true, 0x1AE6AA500D0ull, 0x21EEFF200D0ull));
    EXPECT_FALSE(MustSkipUnownedSwapchain(true, 0x1AE6AA500D0ull, 0x1AE6AA500D0ull));
    EXPECT_FALSE(MustSkipUnownedSwapchain(false, 0, 0x21EEFF200D0ull)) << "an uninitialized state adopts any chain";
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

// The skip has to run before the rebuild decision - after it, the second
// swapchain's present would already have torn the owner's state down.
TEST(VulkanSharpenLifetimeSourceTest, PresentPathSkipsForeignSwapchainsBeforeRebuilding) {
    const std::string source = StripComments(ReadLayerSource("layer_sharpen.cpp"));
    ASSERT_FALSE(source.empty());
    const size_t entry = source.find("bool SharpenPresentedFrame(");
    ASSERT_NE(entry, std::string::npos);
    const size_t skip = source.find("ce::vulkan_sharpen_route::MustSkipUnownedSwapchain(", entry);
    const size_t rebuild = source.find("ce::vulkan_sharpen_route::MustRebuild(", entry);
    const size_t init = source.find("InitializeSharpenState(", entry);
    ASSERT_NE(skip, std::string::npos) << "an unowned swapchain's present must skip the pass";
    ASSERT_NE(rebuild, std::string::npos);
    ASSERT_NE(init, std::string::npos);
    EXPECT_LT(skip, rebuild);
    EXPECT_LT(skip, init);
    EXPECT_NE(source.find("ce::vulkan_sharpen_route::MeetsMinimumTargetSize(", entry), std::string::npos);
}

// The skip is only correct because every retirement path re-arms it. The
// `oldSwapchain` retirement is the handle-reuse half of that: NVIDIA may hand
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
