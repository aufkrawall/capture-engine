#pragma once

#include <cstddef>
#include <cstring>
#include <iterator>
#include <unordered_map>

// Loader-object identity for CE's Vulkan layer.
//
// Inside vkCreateDevice a layer must answer "which VkInstance owns this
// VkPhysicalDevice?", because the next link's vkCreateDevice can only be
// fetched as gipa(instance, "vkCreateDevice"). Remembering the handles that
// vkEnumeratePhysicalDevices produced is not a sufficient answer: an
// application may obtain its physical devices from
// vkEnumeratePhysicalDeviceGroups instead, and a multi-GPU-aware Vulkan 1.1
// engine does exactly that. Red Dead Redemption 2 enumerates only groups, so
// the handle map stayed empty, CE's vkCreateDevice returned
// VK_ERROR_INITIALIZATION_FAILED, and the game asserted during startup - for
// every launch on the machine, because the layer is implicit and therefore
// loads even while CaptureEngine is not running.
//
// The loader's dispatchable-object contract makes the ownership question
// answerable without hooking any enumeration entry point at all: every
// dispatchable handle starts with a pointer to the dispatch table its owner
// installed, and the loader installs the *same* instance dispatch table
// pointer on a VkInstance and on every VkPhysicalDevice belonging to it. That
// pointer - the dispatch key - is what the Khronos layer framework itself uses
// to find instance layer data from a VkPhysicalDevice.
//
// Resolution therefore runs most-exact-first and reports which step answered,
// so a fallback is visible in the log instead of silently becoming load-
// bearing. The one thing it must never do is fail: this layer is loaded into
// every Vulkan process on the system, so an unanswered lookup has to degrade
// into pass-through, never into an error the application would not otherwise
// have seen.

namespace ce::vulkan_instance_registry {

// The dispatch table pointer stored at offset 0 of a dispatchable handle.
// Reads exactly one pointer and never dereferences a null handle.
inline const void* DispatchKey(const void* dispatchableHandle) {
    if (dispatchableHandle == nullptr)
        return nullptr;
    const void* key = nullptr;
    std::memcpy(static_cast<void*>(&key), dispatchableHandle, sizeof(key));
    return key;
}

// Which step answered a lookup. Anything past kExactHandle means CE's own
// bookkeeping missed the object and is worth logging once.
enum class Resolution {
    kNone = 0,
    kExactHandle,
    kDispatchKey,
    kSoleInstance,
};

inline const char* ToString(Resolution resolution) {
    switch (resolution) {
        case Resolution::kExactHandle:
            return "exact";
        case Resolution::kDispatchKey:
            return "dispatch-key";
        case Resolution::kSoleInstance:
            return "sole-instance";
        case Resolution::kNone:
        default:
            return "none";
    }
}

struct InstanceLookup {
    void* instance = nullptr;
    Resolution resolution = Resolution::kNone;
};

// Handle bookkeeping only - the dispatch tables themselves stay in
// VulkanLayerState. Callers hold that class's lock; the registry adds none.
class Registry {
public:
    void AddInstance(void* instance, const void* dispatchKey) {
        if (instance == nullptr)
            return;
        m_InstanceKeys[instance] = dispatchKey;
        if (dispatchKey != nullptr)
            m_KeyToInstance[dispatchKey] = instance;
    }

    // Drops the instance and every physical device recorded against it. A
    // VkPhysicalDevice dies with its instance and the loader reuses the
    // allocation, so a surviving entry could answer for a later instance.
    void RemoveInstance(void* instance) {
        if (instance == nullptr)
            return;
        auto keyIt = m_InstanceKeys.find(instance);
        if (keyIt != m_InstanceKeys.end()) {
            auto byKey = m_KeyToInstance.find(keyIt->second);
            if (byKey != m_KeyToInstance.end() && byKey->second == instance)
                m_KeyToInstance.erase(byKey);
            m_InstanceKeys.erase(keyIt);
        }
        // NOLINTNEXTLINE(bugprone-nondeterministic-pointer-iteration-order) - erase-by-value, order-independent
        for (auto it = m_PhysicalDevices.begin(); it != m_PhysicalDevices.end();) {
            it = (it->second == instance) ? m_PhysicalDevices.erase(it) : std::next(it);
        }
    }

    void AddPhysicalDevice(void* physicalDevice, void* instance) {
        if (physicalDevice == nullptr || instance == nullptr)
            return;
        m_PhysicalDevices[physicalDevice] = instance;
    }

    InstanceLookup ResolveByPhysicalDevice(void* physicalDevice) const {
        if (physicalDevice == nullptr)
            return {};
        auto it = m_PhysicalDevices.find(physicalDevice);
        if (it != m_PhysicalDevices.end() && m_InstanceKeys.count(it->second) != 0)
            return {it->second, Resolution::kExactHandle};
        return ResolveByKeyOrSoleInstance(DispatchKey(physicalDevice));
    }

    // Same three steps for a VkInstance handle, so a lookup for an instance CE
    // did not create still reaches the dispatch table of the one it did.
    InstanceLookup ResolveInstance(void* instance) const {
        if (instance == nullptr)
            return {};
        if (m_InstanceKeys.count(instance) != 0)
            return {instance, Resolution::kExactHandle};
        return ResolveByKeyOrSoleInstance(DispatchKey(instance));
    }

    size_t InstanceCount() const {
        return m_InstanceKeys.size();
    }

    size_t PhysicalDeviceCount() const {
        return m_PhysicalDevices.size();
    }

private:
    InstanceLookup ResolveByKeyOrSoleInstance(const void* dispatchKey) const {
        if (dispatchKey != nullptr) {
            auto it = m_KeyToInstance.find(dispatchKey);
            if (it != m_KeyToInstance.end())
                return {it->second, Resolution::kDispatchKey};
        }
        // A process with exactly one live instance leaves no ambiguity, and
        // pass-through through that instance's chain beats failing the call.
        if (m_InstanceKeys.size() == 1)
            return {m_InstanceKeys.begin()->first, Resolution::kSoleInstance};
        return {};
    }

    std::unordered_map<void*, const void*> m_InstanceKeys;
    std::unordered_map<const void*, void*> m_KeyToInstance;
    std::unordered_map<void*, void*> m_PhysicalDevices;
};

}  // namespace ce::vulkan_instance_registry
