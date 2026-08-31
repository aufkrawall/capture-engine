#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <map>
#include <vector>

#include "../hook/common/vulkan_dxgi_fifo_policy.h"
#include "../hook/common/vulkan_dxgi_fifo_registry.h"
#include "../hook/common/vulkan_wsi_surface_table.h"

namespace {

using ce::vulkan_dxgi_fifo_policy::ApplyFinalDxgiFifoParameters;
using ce::vulkan_dxgi_fifo_policy::ShouldArmFinalDxgiPresent;
using ce::vulkan_dxgi_fifo_policy::ShouldForceFinalDxgiFifo;
using ce::vulkan_dxgi_fifo_policy::ShouldRewriteFinalPresent;
using ce::vulkan_dxgi_fifo_registry::kInvalidSlot;
using ce::vulkan_dxgi_fifo_registry::ObservedSwapchainRegistry;
using ce::vulkan_wsi_surfaces::kMaxLiveSurfaceHwnds;
using ce::vulkan_wsi_surfaces::LiveSurfaceHwndTable;
using ce::vulkan_wsi_surfaces::SelectWindowsToRetireOnInstanceDestroy;

// Minimal stand-in for VulkanLayerState::SurfaceRecord: the sweep policy only
// needs `.window` and `.instance` from the map's values.
struct Record {
    HWND window;
    const void* instance;
};
using SurfaceMap = std::map<const void*, Record>;

const auto hwnd = [](uintptr_t value) { return reinterpret_cast<HWND>(value); };

} // namespace

// The final-DXGI SyncInterval=1 backstop is intentionally retired. It replaced
// VRR with a fixed refresh grid below Vulkan and caused 4x generated groups to
// run fast and then freeze. Native VK_EXT_present_timing now owns the ceiling.
TEST(VulkanDxgiFifoScopingTest, FinalDxgiPresentNeverArms) {
    EXPECT_FALSE(ShouldArmFinalDxgiPresent(true, "fifo"));
    EXPECT_FALSE(ShouldArmFinalDxgiPresent(true, "adaptive"));
    EXPECT_FALSE(ShouldArmFinalDxgiPresent(false, "fifo"));
    EXPECT_FALSE(ShouldArmFinalDxgiPresent(false, "adaptive"));
    EXPECT_FALSE(ShouldArmFinalDxgiPresent(true, "mailbox"));
    EXPECT_FALSE(ShouldArmFinalDxgiPresent(true, "off"));
    EXPECT_FALSE(ShouldArmFinalDxgiPresent(true, "default"));
    EXPECT_FALSE(ShouldArmFinalDxgiPresent(true, ""));
    EXPECT_FALSE(ShouldArmFinalDxgiPresent(true, "FIFO"));
}

TEST(VulkanDxgiFifoScopingTest, FinalDxgiFifoRequiresLiveOwnershipAndLifecycle) {
    EXPECT_TRUE(ShouldForceFinalDxgiFifo(true, true, false));
    EXPECT_FALSE(ShouldForceFinalDxgiFifo(false, true, false));
    EXPECT_FALSE(ShouldForceFinalDxgiFifo(true, false, false));
    EXPECT_FALSE(ShouldForceFinalDxgiFifo(true, true, true));
}

// The final native DXGI vblank contract: forced FIFO presents on the next
// vertical blank, without tearing, may block, and never discards queued
// presents. DO_NOT_SEQUENCE (0x2) and unrelated flags survive; TEST queries and
// already-correct calls are byte-identical no-ops.
TEST(VulkanDxgiFifoScopingTest, FinalDxgiFifoUsesVblankAndForbidsTearing) {
    uint32_t syncInterval = 0;
    uint32_t flags = 0x200u | 0x2u | 0x4u;
    EXPECT_TRUE(ApplyFinalDxgiFifoParameters(true, syncInterval, flags));
    EXPECT_EQ(syncInterval, 1u);
    EXPECT_EQ(flags, 0x2u);

    // DO_NOT_WAIT (0x8) is cleared so the forced FIFO present may block on the
    // vblank; 0x2 is DO_NOT_SEQUENCE and must survive the same rewrite.
    syncInterval = 0;
    flags = 0x8u | 0x2u | 0x4u;
    EXPECT_TRUE(ApplyFinalDxgiFifoParameters(true, syncInterval, flags));
    EXPECT_EQ(syncInterval, 1u);
    EXPECT_EQ(flags, 0x2u);

    // DXGI_PRESENT_RESTART (0x4) discards all outstanding queued presents -
    // the one behavior strict FIFO exists to prevent - so it is cleared even
    // on an otherwise already-correct interval=1 call.
    syncInterval = 1;
    flags = 0x4u;
    EXPECT_TRUE(ApplyFinalDxgiFifoParameters(true, syncInterval, flags));
    EXPECT_EQ(syncInterval, 1u);
    EXPECT_EQ(flags, 0x0u);

    // Already-correct call: byte-identical and reports no change.
    syncInterval = 1;
    flags = 0x2u;
    EXPECT_FALSE(ApplyFinalDxgiFifoParameters(true, syncInterval, flags));
    EXPECT_EQ(syncInterval, 1u);
    EXPECT_EQ(flags, 0x2u);

    // DXGI_PRESENT_TEST (0x1) never presents; pass it through byte-identical
    // even while forced.
    syncInterval = 0;
    flags = 0x1u | 0x200u;
    EXPECT_FALSE(ApplyFinalDxgiFifoParameters(true, syncInterval, flags));
    EXPECT_EQ(syncInterval, 0u);
    EXPECT_EQ(flags, 0x1u | 0x200u);

    syncInterval = 0;
    flags = 0x200u;
    EXPECT_FALSE(ApplyFinalDxgiFifoParameters(false, syncInterval, flags));
    EXPECT_EQ(syncInterval, 0u);
    EXPECT_EQ(flags, 0x200u);
}

// The rewrite only ever fires on a swapchain the creation detours observed:
// no force, or a foreign instance, must pass through.
TEST(VulkanDxgiFifoScopingTest, FinalPresentRewriteRequiresArmedForceAndRegisteredInstance) {
    EXPECT_FALSE(ShouldRewriteFinalPresent(/*forceFifo=*/false, /*presentedSwapchainRegistered=*/true));
    EXPECT_FALSE(ShouldRewriteFinalPresent(/*forceFifo=*/true, /*presentedSwapchainRegistered=*/false));
    EXPECT_FALSE(ShouldRewriteFinalPresent(false, false));
    EXPECT_TRUE(ShouldRewriteFinalPresent(true, true));
}

TEST(VulkanDxgiFifoScopingTest, ObservedSwapchainRegistryScopesMembershipToRegisteredInstances) {
    ObservedSwapchainRegistry registry;
    alignas(16) static std::array<std::byte, 4> storage{};

    const size_t slotA = registry.Register(storage.data());
    const size_t slotB = registry.Register(storage.data() + 1);
    ASSERT_NE(slotA, kInvalidSlot);
    ASSERT_NE(slotB, kInvalidSlot);
    EXPECT_NE(slotA, slotB);

    // Membership is exact: a pointer that was never registered is foreign.
    EXPECT_EQ(registry.Find(storage.data() + 2), kInvalidSlot);

    // Re-registration of a live instance is a refresh, not a duplicate.
    EXPECT_EQ(registry.Register(storage.data()), slotA);
    EXPECT_EQ(registry.Find(storage.data()), slotA);

    // The logged identity comes back from the slot it was registered under.
    EXPECT_EQ(registry.PointerAt(slotA), storage.data());
    EXPECT_EQ(registry.PointerAt(slotB), storage.data() + 1);

    // Per-identity cadence counters advance independently.
    EXPECT_EQ(registry.NextPresentationOccurrence(slotA), 1u);
    EXPECT_EQ(registry.NextPresentationOccurrence(slotA), 2u);
    EXPECT_EQ(registry.NextPresentationOccurrence(slotB), 1u);
}

// Fail-closed capacity: once the bounded table is full, an unknown instance
// stays unregistered and therefore untouched, while every earlier identity
// remains findable.
TEST(VulkanDxgiFifoScopingTest, ObservedSwapchainRegistryFailsClosedWhenFull) {
    ObservedSwapchainRegistry registry;
    alignas(16) static std::array<std::byte, ce::vulkan_dxgi_fifo_registry::kCapacity + 1> storage{};

    for (size_t i = 0; i < ce::vulkan_dxgi_fifo_registry::kCapacity; ++i) {
        EXPECT_NE(registry.Register(storage.data() + i), kInvalidSlot);
    }
    EXPECT_EQ(registry.Register(storage.data() + ce::vulkan_dxgi_fifo_registry::kCapacity), kInvalidSlot);

    for (size_t i = 0; i < ce::vulkan_dxgi_fifo_registry::kCapacity; ++i) {
        EXPECT_NE(registry.Find(storage.data() + i), kInvalidSlot);
    }
    EXPECT_EQ(registry.Find(storage.data() + ce::vulkan_dxgi_fifo_registry::kCapacity), kInvalidSlot);
}

// True WSI scoping (F2): the live-surface HWND table is the authorization
// source for swapchain registration. Only windows with a live Vulkan surface
// behind them may authorize; a destroyed surface's window stops being live
// immediately, which is the explicit vkDestroySurfaceKHR retirement contract.
TEST(VulkanDxgiFifoScopingTest, LiveSurfaceHwndTableTracksOnlyLiveVulkanWindows) {
    LiveSurfaceHwndTable table;

    // A window with no live surface is never live.
    EXPECT_FALSE(table.IsLive(hwnd(0x1000)));

    // Registration makes the window live; an unregistered window stays foreign.
    EXPECT_TRUE(table.Register(hwnd(0x1000)));
    EXPECT_TRUE(table.IsLive(hwnd(0x1000)));
    EXPECT_FALSE(table.IsLive(hwnd(0x2000)));

    // Two surfaces may share one window; the window stays live until the last
    // surface retires (refcounted unregister).
    EXPECT_TRUE(table.Register(hwnd(0x1000)));
    EXPECT_EQ(table.LiveWindowCount(), 1u);
    EXPECT_TRUE(table.Unregister(hwnd(0x1000)));
    EXPECT_TRUE(table.IsLive(hwnd(0x1000)));
    EXPECT_TRUE(table.Unregister(hwnd(0x1000)));
    EXPECT_FALSE(table.IsLive(hwnd(0x1000)));
    EXPECT_EQ(table.LiveWindowCount(), 0u);

    // Multiple windows track independently.
    EXPECT_TRUE(table.Register(hwnd(0x3000)));
    EXPECT_TRUE(table.Register(hwnd(0x4000)));
    EXPECT_TRUE(table.IsLive(hwnd(0x3000)));
    EXPECT_TRUE(table.IsLive(hwnd(0x4000)));
    EXPECT_TRUE(table.Unregister(hwnd(0x3000)));
    EXPECT_FALSE(table.IsLive(hwnd(0x3000)));
    EXPECT_TRUE(table.IsLive(hwnd(0x4000)));

    // A null window can never be a Win32 WSI target.
    EXPECT_FALSE(table.Register(nullptr));
    EXPECT_FALSE(table.IsLive(nullptr));
    EXPECT_FALSE(table.Unregister(nullptr));

    // Retiring an unknown window is a no-op, not an error.
    EXPECT_FALSE(table.Unregister(hwnd(0x5000)));
}

// Fail-closed capacity for the HWND table: when full, a new surface is not
// tracked (so its swapchains are never authorized) and every tracked window
// stays live.
TEST(VulkanDxgiFifoScopingTest, LiveSurfaceHwndTableFailsClosedWhenFull) {
    LiveSurfaceHwndTable table;

    for (uintptr_t i = 1; i <= kMaxLiveSurfaceHwnds; ++i) {
        EXPECT_TRUE(table.Register(hwnd(i)));
    }
    EXPECT_EQ(table.LiveWindowCount(), kMaxLiveSurfaceHwnds);
    EXPECT_FALSE(table.Register(hwnd(kMaxLiveSurfaceHwnds + 1)));
    EXPECT_FALSE(table.IsLive(hwnd(kMaxLiveSurfaceHwnds + 1)));

    for (uintptr_t i = 1; i <= kMaxLiveSurfaceHwnds; ++i) {
        EXPECT_TRUE(table.IsLive(hwnd(i)));
    }

    // Freeing one slot makes room again.
    EXPECT_TRUE(table.Unregister(hwnd(1)));
    EXPECT_TRUE(table.Register(hwnd(kMaxLiveSurfaceHwnds + 1)));
    EXPECT_TRUE(table.IsLive(hwnd(kMaxLiveSurfaceHwnds + 1)));
}

// Implicit retirement at vkDestroyInstance: the sweep selects exactly the
// surfaces owned by the instance being destroyed - one window per surface,
// owned entries erased, foreign surfaces untouched. Without it, an
// application that destroys its instance without destroying its surfaces
// first would leave its HWNDs published as live Vulkan targets forever.
TEST(VulkanDxgiFifoScopingTest, InstanceDestroySweepSelectsExactlyTheOwnedSurfaces) {
    SurfaceMap surfaces;
    const void* kSurfaceA = reinterpret_cast<const void*>(0xA000);
    const void* kSurfaceB = reinterpret_cast<const void*>(0xB000);
    const void* kSurfaceC = reinterpret_cast<const void*>(0xC000);
    const void* kInstance1 = reinterpret_cast<const void*>(0x11000);
    const void* kInstance2 = reinterpret_cast<const void*>(0x22000);

    surfaces[kSurfaceA] = Record{hwnd(0x1000), kInstance1};
    surfaces[kSurfaceB] = Record{hwnd(0x2000), kInstance2};
    surfaces[kSurfaceC] = Record{hwnd(0x3000), kInstance1};

    std::vector<HWND> retired;
    const size_t selected = SelectWindowsToRetireOnInstanceDestroy(surfaces, kInstance1, std::back_inserter(retired));

    EXPECT_EQ(selected, 2u);
    ASSERT_EQ(retired.size(), 2u);
    EXPECT_EQ(retired[0], hwnd(0x1000));
    EXPECT_EQ(retired[1], hwnd(0x3000));

    // The swept entries are gone; the foreign instance's surface survives.
    EXPECT_EQ(surfaces.size(), 1u);
    EXPECT_EQ(surfaces.count(kSurfaceB), 1u);

    // Sweeping an instance that owns nothing retires nothing.
    std::vector<HWND> none;
    EXPECT_EQ(SelectWindowsToRetireOnInstanceDestroy(surfaces, kInstance1, std::back_inserter(none)), 0u);
    EXPECT_TRUE(none.empty());
    EXPECT_EQ(surfaces.size(), 1u);
}

// Refcount correctness when windows are shared: each surface contributes
// exactly one retirement, so two surfaces of one instance on the same window
// retire it twice (which LiveSurfaceHwndTable's per-HWND refcount requires to
// reach zero), while a surface of another instance on that same window keeps
// its own retirement - and its ownership - intact.
TEST(VulkanDxgiFifoScopingTest, InstanceDestroySweepRetiresSharedWindowsOncePerSurface) {
    SurfaceMap surfaces;
    const void* kSurfaceA = reinterpret_cast<const void*>(0xA000);
    const void* kSurfaceB = reinterpret_cast<const void*>(0xB000);
    const void* kSurfaceC = reinterpret_cast<const void*>(0xC000);
    const void* kInstance1 = reinterpret_cast<const void*>(0x11000);
    const void* kInstance2 = reinterpret_cast<const void*>(0x22000);

    // kSurfaceA and kSurfaceB (instance1) share one window; kSurfaceC belongs
    // to instance2 but targets the same window.
    surfaces[kSurfaceA] = Record{hwnd(0x1000), kInstance1};
    surfaces[kSurfaceB] = Record{hwnd(0x1000), kInstance1};
    surfaces[kSurfaceC] = Record{hwnd(0x1000), kInstance2};

    std::vector<HWND> retired;
    const size_t selected = SelectWindowsToRetireOnInstanceDestroy(surfaces, kInstance1, std::back_inserter(retired));

    EXPECT_EQ(selected, 2u);
    ASSERT_EQ(retired.size(), 2u);
    EXPECT_EQ(retired[0], hwnd(0x1000));
    EXPECT_EQ(retired[1], hwnd(0x1000));

    // Only the destroyed instance's surfaces are erased.
    EXPECT_EQ(surfaces.size(), 1u);
    EXPECT_EQ(surfaces.count(kSurfaceC), 1u);
    EXPECT_EQ(surfaces[kSurfaceC].window, hwnd(0x1000));
}
