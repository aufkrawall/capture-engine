#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>

#include "../hook/vulkan_layer/vulkan_loader_data.h"
#include "source_fragment_reader.h"

namespace {

using ce::vulkan_instance_registry::DispatchKey;
using ce::vulkan_loader_data::InitializationOutcome;
using ce::vulkan_loader_data::InitializeDeviceObject;

struct FakeDispatchable {
    const void* dispatchTable = nullptr;
    int tag = 0;
};

VkDevice AsDevice(FakeDispatchable& object) {
    return reinterpret_cast<VkDevice>(&object);
}

void* AsObject(FakeDispatchable& object) {
    return static_cast<void*>(&object);
}

VKAPI_ATTR VkResult VKAPI_CALL InstallParentDispatch(VkDevice device, void* object) {
    const void* dispatch = DispatchKey(device);
    std::memcpy(object, static_cast<const void*>(&dispatch), sizeof(dispatch));
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL RejectLoaderData(VkDevice, void*) {
    return VK_ERROR_INITIALIZATION_FAILED;
}

VKAPI_ATTR VkResult VKAPI_CALL IgnoreLoaderData(VkDevice, void*) {
    return VK_SUCCESS;
}

std::string ReadProjectSource(const char* relativePath) {
    const std::filesystem::path source = std::filesystem::current_path() / relativePath;
    if (!std::filesystem::exists(source))
        return {};
    return ce::test_source::ReadFile(source);
}

}  // namespace

TEST(VulkanLoaderDataTest, LoaderCallbackInitializesALayerCreatedQueue) {
    int loaderDispatch = 0;
    FakeDispatchable device{&loaderDispatch, 1};
    FakeDispatchable queue{reinterpret_cast<const void*>(static_cast<uintptr_t>(0x01CDC0DE)), 2};

    const auto result = InitializeDeviceObject(InstallParentDispatch, AsDevice(device), AsObject(queue));

    EXPECT_TRUE(result.initialized());
    EXPECT_EQ(result.outcome, InitializationOutcome::kLoaderCallback);
    EXPECT_EQ(result.callbackResult, VK_SUCCESS);
    EXPECT_EQ(DispatchKey(AsObject(queue)), &loaderDispatch);
}

TEST(VulkanLoaderDataTest, OlderLoaderFallsBackToTheParentDispatchPointer) {
    int loaderDispatch = 0;
    FakeDispatchable device{&loaderDispatch, 1};
    FakeDispatchable queue{reinterpret_cast<const void*>(static_cast<uintptr_t>(0x01CDC0DE)), 2};

    const auto result = InitializeDeviceObject(nullptr, AsDevice(device), AsObject(queue));

    EXPECT_TRUE(result.initialized());
    EXPECT_EQ(result.outcome, InitializationOutcome::kParentDispatchFallback);
    EXPECT_EQ(DispatchKey(AsObject(queue)), &loaderDispatch);
}

TEST(VulkanLoaderDataTest, RejectedCallbackLeavesTheQueueUnavailable) {
    int loaderDispatch = 0;
    FakeDispatchable device{&loaderDispatch, 1};
    FakeDispatchable queue{reinterpret_cast<const void*>(static_cast<uintptr_t>(0x01CDC0DE)), 2};

    const auto result = InitializeDeviceObject(RejectLoaderData, AsDevice(device), AsObject(queue));

    EXPECT_FALSE(result.initialized());
    EXPECT_EQ(result.outcome, InitializationOutcome::kCallbackRejected);
    EXPECT_EQ(result.callbackResult, VK_ERROR_INITIALIZATION_FAILED);
    EXPECT_NE(DispatchKey(AsObject(queue)), &loaderDispatch);
}

TEST(VulkanLoaderDataTest, SuccessWithoutDispatchInitializationFailsClosed) {
    int loaderDispatch = 0;
    FakeDispatchable device{&loaderDispatch, 1};
    FakeDispatchable queue{reinterpret_cast<const void*>(static_cast<uintptr_t>(0x01CDC0DE)), 2};

    const auto result = InitializeDeviceObject(IgnoreLoaderData, AsDevice(device), AsObject(queue));

    EXPECT_FALSE(result.initialized());
    EXPECT_EQ(result.outcome, InitializationOutcome::kCallbackDidNotInitialize);
    EXPECT_NE(DispatchKey(AsObject(queue)), &loaderDispatch);
}

TEST(VulkanLoaderDataTest, NullAndUninitializedObjectsAreRejected) {
    FakeDispatchable device{};
    FakeDispatchable queue{};

    EXPECT_EQ(InitializeDeviceObject(nullptr, AsDevice(device), nullptr).outcome,
              InitializationOutcome::kNullObject);
    EXPECT_EQ(InitializeDeviceObject(nullptr, AsDevice(device), AsObject(queue)).outcome,
              InitializationOutcome::kMissingParentDispatch);
}

// Portal RTX session 20260901_071629 crashed in SteamOverlayVulkanLayer64's
// vkQueueSubmit because CE fetched its private queue below the loader trampoline
// and registered it while offset zero still held ICD_LOADER_MAGIC (0x01CDC0DE).
TEST(VulkanLoaderDataSourceTest, ReservedQueueIsInitializedBeforeItIsRegistered) {
    const std::string hooks = ReadProjectSource("hook/vulkan_layer/vulkan_layer_hooks.cpp");
    const std::string queues = ReadProjectSource("hook/vulkan_layer/layer_overlay_queue.cpp");
    ASSERT_FALSE(hooks.empty());
    ASSERT_FALSE(queues.empty());

    const size_t createDevice = hooks.find("VKAPI_ATTR VkResult VKAPI_CALL Capture_vkCreateDevice(");
    ASSERT_NE(createDevice, std::string::npos);
    const size_t findCallback = hooks.find("FindDeviceLoaderDataCallback(*pCreateInfo)", createDevice);
    const size_t advanceLink =
        hooks.find("chain_info->u.pLayerInfo = chain_info->u.pLayerInfo->pNext", createDevice);
    ASSERT_NE(findCallback, std::string::npos);
    ASSERT_NE(advanceLink, std::string::npos);
    EXPECT_LT(findCallback, advanceLink);
    EXPECT_NE(hooks.find("InitializeReservedOverlayQueue"), std::string::npos);
    EXPECT_NE(queues.find("VK_LOADER_DATA_CALLBACK"), std::string::npos);

    const size_t initialize = queues.find("InitializeDeviceObject(setDeviceLoaderData, device, queue)");
    const size_t registerQueue = queues.find("RegisterQueue(queue, device");
    ASSERT_NE(initialize, std::string::npos);
    ASSERT_NE(registerQueue, std::string::npos);
    EXPECT_LT(initialize, registerQueue);
}
