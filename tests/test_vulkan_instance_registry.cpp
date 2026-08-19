#include <gtest/gtest.h>

#include <filesystem>
#include <string>

#include "../hook/vulkan_layer/vulkan_instance_registry.h"
#include "source_fragment_reader.h"

namespace {

using ce::vulkan_instance_registry::DispatchKey;
using ce::vulkan_instance_registry::InstanceLookup;
using ce::vulkan_instance_registry::Registry;
using ce::vulkan_instance_registry::Resolution;

// A stand-in for a loader dispatchable object: the dispatch table pointer at
// offset 0 is the whole contract the registry relies on.
struct FakeDispatchable {
    const void* dispatchTable = nullptr;
    int tag = 0;
};

void* AsHandle(FakeDispatchable& object) {
    return static_cast<void*>(&object);
}

std::string ReadLayerSource(const char* fileName) {
    return ce::test_source::ReadLogicalSource(std::filesystem::current_path() / "hook" / "vulkan_layer" / fileName);
}

}  // namespace

TEST(VulkanInstanceRegistryTest, DispatchKeyReadsTheTablePointerAndToleratesNull) {
    int table = 0;
    FakeDispatchable object{&table, 1};

    EXPECT_EQ(DispatchKey(AsHandle(object)), &table);
    EXPECT_EQ(DispatchKey(nullptr), nullptr);
}

TEST(VulkanInstanceRegistryTest, ResolvesAnEnumeratedPhysicalDeviceExactly) {
    int table = 0;
    FakeDispatchable instance{&table, 1};
    FakeDispatchable physicalDevice{&table, 2};

    Registry registry;
    registry.AddInstance(AsHandle(instance), DispatchKey(AsHandle(instance)));
    registry.AddPhysicalDevice(AsHandle(physicalDevice), AsHandle(instance));

    const InstanceLookup lookup = registry.ResolveByPhysicalDevice(AsHandle(physicalDevice));
    EXPECT_EQ(lookup.instance, AsHandle(instance));
    EXPECT_EQ(lookup.resolution, Resolution::kExactHandle);
    EXPECT_EQ(registry.PhysicalDeviceCount(), 1u);
}

// The Red Dead Redemption 2 regression. The game takes its physical devices
// from vkEnumeratePhysicalDeviceGroups only, so before this fix nothing ever
// entered the handle map and vkCreateDevice failed the game's own call - on
// every launch, with CaptureEngine not even running, because the layer is
// implicit. The loader stamps those handles with the instance's dispatch table
// pointer, which is what makes them resolvable without any enumeration hook.
TEST(VulkanInstanceRegistryTest, ResolvesAPhysicalDeviceNoEnumerationHookEverSaw) {
    int table = 0;
    FakeDispatchable instance{&table, 1};
    FakeDispatchable groupPhysicalDevice{&table, 2};

    Registry registry;
    registry.AddInstance(AsHandle(instance), DispatchKey(AsHandle(instance)));

    const InstanceLookup lookup = registry.ResolveByPhysicalDevice(AsHandle(groupPhysicalDevice));
    EXPECT_EQ(lookup.instance, AsHandle(instance));
    EXPECT_EQ(lookup.resolution, Resolution::kDispatchKey);
    EXPECT_EQ(registry.PhysicalDeviceCount(), 0u);
}

TEST(VulkanInstanceRegistryTest, TheDispatchKeyPicksTheRightOneOfTwoInstances) {
    int firstTable = 0;
    int secondTable = 0;
    FakeDispatchable firstInstance{&firstTable, 1};
    FakeDispatchable secondInstance{&secondTable, 2};
    FakeDispatchable secondPhysicalDevice{&secondTable, 3};

    Registry registry;
    registry.AddInstance(AsHandle(firstInstance), DispatchKey(AsHandle(firstInstance)));
    registry.AddInstance(AsHandle(secondInstance), DispatchKey(AsHandle(secondInstance)));

    const InstanceLookup lookup = registry.ResolveByPhysicalDevice(AsHandle(secondPhysicalDevice));
    EXPECT_EQ(lookup.instance, AsHandle(secondInstance));
    EXPECT_EQ(lookup.resolution, Resolution::kDispatchKey);
}

// A layer below CE is free to hand up an object whose dispatch pointer CE has
// never seen. With a single live instance the owner is still unambiguous, and
// pass-through through its chain is the only answer that keeps the application
// working.
TEST(VulkanInstanceRegistryTest, FallsBackToTheSoleInstanceForAnUnknownDispatchKey) {
    int table = 0;
    int foreignTable = 0;
    FakeDispatchable instance{&table, 1};
    FakeDispatchable foreignPhysicalDevice{&foreignTable, 2};

    Registry registry;
    registry.AddInstance(AsHandle(instance), DispatchKey(AsHandle(instance)));

    const InstanceLookup lookup = registry.ResolveByPhysicalDevice(AsHandle(foreignPhysicalDevice));
    EXPECT_EQ(lookup.instance, AsHandle(instance));
    EXPECT_EQ(lookup.resolution, Resolution::kSoleInstance);
}

TEST(VulkanInstanceRegistryTest, ReportsNoOwnerRatherThanGuessingBetweenInstances) {
    int firstTable = 0;
    int secondTable = 0;
    int foreignTable = 0;
    FakeDispatchable firstInstance{&firstTable, 1};
    FakeDispatchable secondInstance{&secondTable, 2};
    FakeDispatchable foreignPhysicalDevice{&foreignTable, 3};

    Registry registry;
    registry.AddInstance(AsHandle(firstInstance), DispatchKey(AsHandle(firstInstance)));
    registry.AddInstance(AsHandle(secondInstance), DispatchKey(AsHandle(secondInstance)));

    const InstanceLookup lookup = registry.ResolveByPhysicalDevice(AsHandle(foreignPhysicalDevice));
    EXPECT_EQ(lookup.instance, nullptr);
    EXPECT_EQ(lookup.resolution, Resolution::kNone);
}

TEST(VulkanInstanceRegistryTest, ResolvesAnInstanceHandleThroughItsDispatchKey) {
    int table = 0;
    FakeDispatchable instance{&table, 1};
    FakeDispatchable sameChainInstanceHandle{&table, 2};

    Registry registry;
    registry.AddInstance(AsHandle(instance), DispatchKey(AsHandle(instance)));

    const InstanceLookup exact = registry.ResolveInstance(AsHandle(instance));
    EXPECT_EQ(exact.instance, AsHandle(instance));
    EXPECT_EQ(exact.resolution, Resolution::kExactHandle);

    const InstanceLookup byKey = registry.ResolveInstance(AsHandle(sameChainInstanceHandle));
    EXPECT_EQ(byKey.instance, AsHandle(instance));
    EXPECT_EQ(byKey.resolution, Resolution::kDispatchKey);
}

// A destroyed instance takes its physical devices and its dispatch key with it.
// The loader reuses both allocations, so a surviving entry would answer a later
// lookup with a dangling instance.
TEST(VulkanInstanceRegistryTest, DestroyingAnInstanceDropsItsPhysicalDevicesAndKey) {
    int table = 0;
    FakeDispatchable instance{&table, 1};
    FakeDispatchable physicalDevice{&table, 2};

    Registry registry;
    registry.AddInstance(AsHandle(instance), DispatchKey(AsHandle(instance)));
    registry.AddPhysicalDevice(AsHandle(physicalDevice), AsHandle(instance));
    registry.RemoveInstance(AsHandle(instance));

    EXPECT_EQ(registry.InstanceCount(), 0u);
    EXPECT_EQ(registry.PhysicalDeviceCount(), 0u);

    const InstanceLookup lookup = registry.ResolveByPhysicalDevice(AsHandle(physicalDevice));
    EXPECT_EQ(lookup.instance, nullptr);
    EXPECT_EQ(lookup.resolution, Resolution::kNone);
}

// An exact entry left behind by an instance that has since been destroyed must
// not outrank the live instance's dispatch key.
TEST(VulkanInstanceRegistryTest, NeverResolvesToAnInstanceThatIsNoLongerRegistered) {
    int firstTable = 0;
    int secondTable = 0;
    FakeDispatchable firstInstance{&firstTable, 1};
    FakeDispatchable secondInstance{&secondTable, 2};
    FakeDispatchable physicalDevice{&secondTable, 3};

    Registry registry;
    registry.AddInstance(AsHandle(firstInstance), DispatchKey(AsHandle(firstInstance)));
    registry.AddInstance(AsHandle(secondInstance), DispatchKey(AsHandle(secondInstance)));
    registry.AddPhysicalDevice(AsHandle(physicalDevice), AsHandle(firstInstance));
    // Only the owning-instance link is dropped, mimicking a stale entry.
    registry.RemoveInstance(AsHandle(firstInstance));
    registry.AddPhysicalDevice(AsHandle(physicalDevice), AsHandle(firstInstance));

    const InstanceLookup lookup = registry.ResolveByPhysicalDevice(AsHandle(physicalDevice));
    EXPECT_EQ(lookup.instance, AsHandle(secondInstance));
    EXPECT_EQ(lookup.resolution, Resolution::kDispatchKey);
}

TEST(VulkanInstanceRegistryTest, NullHandlesResolveToNothingAndAreNotStored) {
    int table = 0;
    FakeDispatchable instance{&table, 1};

    Registry registry;
    registry.AddInstance(nullptr, nullptr);
    registry.AddInstance(AsHandle(instance), DispatchKey(AsHandle(instance)));
    registry.AddPhysicalDevice(nullptr, AsHandle(instance));
    registry.RemoveInstance(nullptr);

    EXPECT_EQ(registry.InstanceCount(), 1u);
    EXPECT_EQ(registry.PhysicalDeviceCount(), 0u);
    EXPECT_EQ(registry.ResolveByPhysicalDevice(nullptr).instance, nullptr);
    EXPECT_EQ(registry.ResolveInstance(nullptr).instance, nullptr);
}

// The layer is an implicit layer: it loads into every Vulkan process on the
// machine, whether or not CaptureEngine runs. Both device-group entry points
// therefore have to stay wired into vkGetInstanceProcAddr, or a title that
// enumerates only groups goes back to creating devices CE cannot attribute.
TEST(VulkanInstanceRegistrySourceTest, GetInstanceProcAddrExposesBothDeviceGroupEnumerations) {
    const std::string source = ReadLayerSource("layer_main.cpp");
    ASSERT_FALSE(source.empty());

    EXPECT_NE(source.find("\"vkEnumeratePhysicalDeviceGroups\""), std::string::npos);
    EXPECT_NE(source.find("Capture_vkEnumeratePhysicalDeviceGroups;"), std::string::npos);
    EXPECT_NE(source.find("\"vkEnumeratePhysicalDeviceGroupsKHR\""), std::string::npos);
    EXPECT_NE(source.find("Capture_vkEnumeratePhysicalDeviceGroupsKHR;"), std::string::npos);
}

// vkCreateDevice must never turn CE's own bookkeeping miss into a failure of
// the application's call. Red Dead Redemption 2 asserted and died on exactly
// that VK_ERROR_INITIALIZATION_FAILED.
TEST(VulkanInstanceRegistrySourceTest, CreateDeviceFallsBackToPassthroughInsteadOfFailing) {
    const std::string source = ReadLayerSource("vulkan_layer_hooks.cpp");
    ASSERT_FALSE(source.empty());

    const size_t createDevice = source.find("Capture_vkCreateDevice(VkPhysicalDevice physicalDevice");
    ASSERT_NE(createDevice, std::string::npos);
    const size_t createDeviceEnd = source.find("Capture_vkDestroyDevice", createDevice);
    ASSERT_NE(createDeviceEnd, std::string::npos);
    const std::string body = source.substr(createDevice, createDeviceEnd - createDevice);

    EXPECT_NE(body.find("passthroughOnly"), std::string::npos);
    EXPECT_EQ(body.find("Could not find instance for physical device"), std::string::npos);
}
