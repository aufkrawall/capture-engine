#pragma once

#include "vulkan_layer.h"

#include "../common/fps_limiter.h"
#include "../common/system_latency_metrics.h"
#include "layer_main.h"

#include <cstdint>
#include <cstring>
#include <mutex>

// Vulkan-native Reflex pacing through NVIDIA's public Vulkan low-latency
// NvAPI contract. This path works with a VkDevice created by the game and does
// not reinterpret it as the IUnknown required by NvAPI_D3D_SetSleepMode.
class VulkanReflexLimiter {
public:
    void SetDevice(VkDevice device, DeviceDispatch* dispatch) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (device != device_) {
            DestroyLocked();
            device_ = device;
            dispatch_ = dispatch;
            nextInitAttemptTick_ = 0;
            nextGameStatusAttemptTick_ = 0;
        } else {
            dispatch_ = dispatch;
        }
    }

    bool IsAvailable() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (IsModernGameSleepRecentLocked()) {
            return true;
        }
        PrepareLocked();
        return legacyGameOwned_ ||
               (initialized_ && semaphore_ != VK_NULL_HANDLE && dispatch_ && dispatch_->fp_vkWaitSemaphores);
    }

    bool IsGameActive() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (IsModernGameSleepRecentLocked() || legacyGameOwned_) {
            return true;
        }
        if (initialized_ || device_ == VK_NULL_HANDLE || !dispatch_) {
            return false;
        }
        ResolveFunctionsLocked();
        if (!getSleepStatus_ || !setSleepMode_) {
            return false;
        }
        const uint64_t now = GetTickCount64();
        if (now < nextGameStatusAttemptTick_) {
            return false;
        }
        nextGameStatusAttemptTick_ = now + 250;
        return DetectLegacyGameContextLocked();
    }

    bool SetTargetFps(int fps) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (IsModernGameSleepRecentLocked() && modernSwapchain_ != VK_NULL_HANDLE && dispatch_ &&
            dispatch_->fp_vkSetLatencySleepModeNV && fps > 0) {
            const uint32_t intervalUs = TargetIntervalUs(fps);
            if (modernConfiguredIntervalUs_ != intervalUs) {
                VkLatencySleepModeInfoNV params = modernGameSleepMode_;
                params.sType = VK_STRUCTURE_TYPE_LATENCY_SLEEP_MODE_INFO_NV;
                params.lowLatencyMode = VK_TRUE;
                params.minimumIntervalUs = intervalUs;
                const VkResult result =
                    dispatch_->fp_vkSetLatencySleepModeNV(device_, modernSwapchain_, &params);
                if (result != VK_SUCCESS) {
                    LayerLog("Vulkan Reflex: VK_NV_low_latency2 target push failed result=%d target=%d",
                             static_cast<int>(result), fps);
                    return false;
                }
                modernConfiguredIntervalUs_ = intervalUs;
                LayerLog("Vulkan Reflex: game-owned VK_NV_low_latency2 pacing target=%d intervalUs=%u", fps,
                         intervalUs);
            }
            return true;
        }
        PrepareLocked();
        if ((!initialized_ && !legacyGameOwned_) || !setSleepMode_ || fps <= 0) {
            return false;
        }
        const uint32_t intervalUs = TargetIntervalUs(fps);
        if (configuredIntervalUs_ == intervalUs) {
            return true;
        }

        NvVulkanSetSleepModeParams params{};
        params.version = MakeVersion(sizeof(params), 1);
        params.lowLatencyMode = 1;
        params.lowLatencyBoost = 0;
        params.minimumIntervalUs = intervalUs;
        const int status = setSleepMode_(AsHandle(device_), &params);
        if (status != kNvApiOk) {
            LayerLog("Vulkan Reflex: NvAPI_Vulkan_SetSleepMode failed status=%d target=%d", status, fps);
            FailLocked();
            return false;
        }
        configuredIntervalUs_ = intervalUs;
        LayerLog("Vulkan Reflex: driver pacing configured target=%d intervalUs=%u", fps, intervalUs);
        return true;
    }

    bool Sleep(int64_t* waitUs) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (waitUs) {
            *waitUs = 0;
        }
        // The application has already called vkLatencySleepNV at the optimal
        // pre-input point and waited on its semaphore. Do not issue a second
        // sleep; our job is only to keep its persistent interval overridden.
        if (IsModernGameSleepRecentLocked() && modernConfiguredIntervalUs_ != 0) {
            return true;
        }
        // Another component already initialized the legacy Vulkan Reflex
        // device. Its own per-frame Sleep remains the owner; SetSleepMode still
        // gives the driver a persistent native cap without CE duplicating or
        // destroying that component's semaphore context.
        if (legacyGameOwned_ && configuredIntervalUs_ != 0) {
            return true;
        }
        if (!initialized_ || configuredIntervalUs_ == 0 || !sleep_ || semaphore_ == VK_NULL_HANDLE || !dispatch_ ||
            !dispatch_->fp_vkWaitSemaphores) {
            return false;
        }

        LARGE_INTEGER frequency{};
        LARGE_INTEGER start{};
        LARGE_INTEGER end{};
        QueryPerformanceFrequency(&frequency);
        QueryPerformanceCounter(&start);

        const uint64_t signalValue = ++signalValue_;
        const int sleepStatus = sleep_(AsHandle(device_), signalValue);
        VkResult waitResult = VK_ERROR_INITIALIZATION_FAILED;
        if (sleepStatus == kNvApiOk) {
            VkSemaphoreWaitInfo waitInfo{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
            waitInfo.semaphoreCount = 1;
            waitInfo.pSemaphores = &semaphore_;
            waitInfo.pValues = &signalValue;
            waitResult = dispatch_->fp_vkWaitSemaphores(device_, &waitInfo, UINT64_MAX);
        }

        QueryPerformanceCounter(&end);
        if (waitUs && frequency.QuadPart > 0) {
            *waitUs = (end.QuadPart - start.QuadPart) * 1000000 / frequency.QuadPart;
        }
        if (sleepStatus != kNvApiOk || waitResult != VK_SUCCESS) {
            LayerLog("Vulkan Reflex: native sleep failed nvapi=%d vulkan=%d signal=%llu", sleepStatus,
                     static_cast<int>(waitResult), static_cast<unsigned long long>(signalValue));
            FailLocked();
            return false;
        }
        return true;
    }

    bool QueryLatencyReport(VkDevice device, ce::system_latency::NativeReport& report);

    void Clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        ClearLocked();
    }

    void ShutdownDevice(VkDevice device) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (device == device_) {
            DestroyLocked();
            device_ = VK_NULL_HANDLE;
            dispatch_ = nullptr;
        }
    }

    VkResult InterceptSetSleepMode(VkDevice device, VkSwapchainKHR swapchain,
                                   const VkLatencySleepModeInfoNV* params,
                                   PFN_vkSetLatencySleepModeNV original) {
        if (!original) {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        if (device != device_) {
            return original(device, swapchain, params);
        }
        modernSwapchain_ = swapchain;
        if (params) {
            modernGameSleepMode_ = *params;
            modernGameSleepMode_.pNext = nullptr;
        }
        VkLatencySleepModeInfoNV overridden{};
        const VkLatencySleepModeInfoNV* forwarded = params;
        if (params && modernConfiguredIntervalUs_ != 0) {
            overridden = *params;
            overridden.lowLatencyMode = VK_TRUE;
            overridden.minimumIntervalUs = modernConfiguredIntervalUs_;
            forwarded = &overridden;
        }
        if (!modernSetObserved_) {
            modernSetObserved_ = true;
            LayerLog("Vulkan Reflex: observed game VK_NV_low_latency2 sleep mode lowLatency=%u boost=%u interval=%u",
                     params ? params->lowLatencyMode : 0, params ? params->lowLatencyBoost : 0,
                     params ? params->minimumIntervalUs : 0);
        }
        return original(device, swapchain, forwarded);
    }

    VkResult InterceptSleep(VkDevice device, VkSwapchainKHR swapchain, const VkLatencySleepInfoNV* params,
                            PFN_vkLatencySleepNV original) {
        if (!original) {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        const VkResult result = original(device, swapchain, params);
        if (result == VK_SUCCESS) {
            std::lock_guard<std::mutex> lock(mutex_);
            if (device == device_) {
                modernSwapchain_ = swapchain;
                modernLastGameSleepTick_ = GetTickCount64();
                if (!modernSleepObserved_) {
                    modernSleepObserved_ = true;
                    LayerLog("Vulkan Reflex: observed game vkLatencySleepNV; using game-owned pre-input pacing");
                }
            }
        }
        return result;
    }

private:
    struct NvVulkanSetSleepModeParams {
        uint32_t version;
        uint8_t lowLatencyMode;
        uint8_t lowLatencyBoost;
        uint32_t minimumIntervalUs;
        uint8_t reserved[32];
    };
    static_assert(sizeof(NvVulkanSetSleepModeParams) == 44, "NvAPI Vulkan sleep-mode ABI changed");

    struct NvVulkanGetSleepStatusParams {
        uint32_t version;
        uint8_t lowLatencyMode;
        uint8_t reserved[128];
    };
    static_assert(sizeof(NvVulkanGetSleepStatusParams) == 136, "NvAPI Vulkan sleep-status ABI changed");

    using NvApiQueryInterface = void*(__cdecl*)(uint32_t id);
    using NvApiVulkanInit = int(__cdecl*)(HANDLE device, HANDLE* semaphore);
    using NvApiVulkanDestroy = int(__cdecl*)(HANDLE device);
    using NvApiVulkanGetSleepStatus = int(__cdecl*)(HANDLE device, NvVulkanGetSleepStatusParams* params);
    using NvApiVulkanSetSleepMode = int(__cdecl*)(HANDLE device, NvVulkanSetSleepModeParams* params);
    using NvApiVulkanSleep = int(__cdecl*)(HANDLE device, uint64_t signalValue);

    static constexpr int kNvApiOk = 0;
    static constexpr uint32_t kInitId = 0x5c1696b6;
    static constexpr uint32_t kDestroyId = 0x11a5932b;
    static constexpr uint32_t kGetSleepStatusId = 0xadf966af;
    static constexpr uint32_t kSetSleepModeId = 0x2acfd162;
    static constexpr uint32_t kSleepId = 0x36732b1e;

    static uint32_t MakeVersion(size_t size, uint32_t version) {
        return static_cast<uint32_t>(size) | (version << 16);
    }

    static uint32_t TargetIntervalUs(int fps) {
        return static_cast<uint32_t>((1000000ULL + static_cast<uint32_t>(fps) / 2) /
                                     static_cast<uint32_t>(fps));
    }

    bool IsModernGameSleepRecentLocked() const {
        if (!modernSleepObserved_ || modernLastGameSleepTick_ == 0 || modernSwapchain_ == VK_NULL_HANDLE ||
            !dispatch_ || !dispatch_->fp_vkSetLatencySleepModeNV) {
            return false;
        }
        const uint64_t now = GetTickCount64();
        return now >= modernLastGameSleepTick_ && now - modernLastGameSleepTick_ <= 500;
    }

    static HANDLE AsHandle(VkDevice device) {
        return reinterpret_cast<HANDLE>(device);
    }

    void ResolveFunctionsLocked() {
        if (init_ && destroy_ && setSleepMode_ && sleep_) {
            return;
        }
#ifdef _WIN64
        HMODULE nvapi = GetModuleHandleW(L"nvapi64.dll");
#else
        HMODULE nvapi = GetModuleHandleW(L"nvapi.dll");
#endif
        if (!nvapi) {
            return;
        }
        auto query = reinterpret_cast<NvApiQueryInterface>(GetProcAddress(nvapi, "nvapi_QueryInterface"));
        if (!query) {
            return;
        }
        init_ = reinterpret_cast<NvApiVulkanInit>(query(kInitId));
        destroy_ = reinterpret_cast<NvApiVulkanDestroy>(query(kDestroyId));
        getSleepStatus_ = reinterpret_cast<NvApiVulkanGetSleepStatus>(query(kGetSleepStatusId));
        setSleepMode_ = reinterpret_cast<NvApiVulkanSetSleepMode>(query(kSetSleepModeId));
        sleep_ = reinterpret_cast<NvApiVulkanSleep>(query(kSleepId));
        if ((!init_ || !destroy_ || !setSleepMode_ || !sleep_) && !loggedIncomplete_) {
            loggedIncomplete_ = true;
            LayerLog("Vulkan Reflex: NvAPI Vulkan entry points incomplete init=%p destroy=%p set=%p sleep=%p",
                     (void*)init_, (void*)destroy_, (void*)setSleepMode_, (void*)sleep_);
        }
    }

    void PrepareLocked() {
        if (initialized_ || legacyGameOwned_ || device_ == VK_NULL_HANDLE || !dispatch_) {
            return;
        }
        ResolveFunctionsLocked();
        if (!init_ || !destroy_ || !setSleepMode_ || !sleep_) {
            return;
        }
        const uint64_t now = GetTickCount64();
        if (now < nextInitAttemptTick_) {
            return;
        }
        nextInitAttemptTick_ = now + 1000;

        if (DetectLegacyGameContextLocked()) {
            return;
        }

        // A game-owned context only needs the persistent SetSleepMode call.
        // CE-created contexts additionally need a timeline-semaphore wait to
        // consume each NvAPI_Vulkan_Sleep signal.
        if (!dispatch_->fp_vkWaitSemaphores) {
            return;
        }

        HANDLE signalSemaphore = nullptr;
        const int status = init_(AsHandle(device_), &signalSemaphore);
        if (status != kNvApiOk || !signalSemaphore) {
            ++initFailureLogCount_;
            if (initFailureLogCount_ <= 3 || (initFailureLogCount_ % 60) == 0) {
                LayerLog("Vulkan Reflex: low-latency device init unavailable status=%d semaphore=%p", status,
                         signalSemaphore);
            }
            return;
        }
        semaphore_ = reinterpret_cast<VkSemaphore>(signalSemaphore);
        initialized_ = true;
        signalValue_ = 0;
        initFailureLogCount_ = 0;
        LayerLog("Vulkan Reflex: native low-latency device ready device=%p semaphore=%p", device_, semaphore_);
    }

    bool DetectLegacyGameContextLocked() {
        if (!getSleepStatus_) {
            return false;
        }
        NvVulkanGetSleepStatusParams statusParams{};
        statusParams.version = MakeVersion(sizeof(statusParams), 1);
        if (getSleepStatus_(AsHandle(device_), &statusParams) != kNvApiOk) {
            return false;
        }
        legacyGameOwned_ = true;
        legacyGameLowLatencyMode_ = statusParams.lowLatencyMode;
        LayerLog("Vulkan Reflex: existing game-owned NvAPI Vulkan context detected lowLatency=%u",
                 statusParams.lowLatencyMode);
        return true;
    }

    void ClearLocked() {
        if (modernConfiguredIntervalUs_ != 0 && modernSwapchain_ != VK_NULL_HANDLE && dispatch_ &&
            dispatch_->fp_vkSetLatencySleepModeNV) {
            VkLatencySleepModeInfoNV params = modernGameSleepMode_;
            params.sType = VK_STRUCTURE_TYPE_LATENCY_SLEEP_MODE_INFO_NV;
            const VkResult result = dispatch_->fp_vkSetLatencySleepModeNV(device_, modernSwapchain_, &params);
            if (result != VK_SUCCESS) {
                LayerLog("Vulkan Reflex: failed to restore game VK_NV_low_latency2 mode result=%d",
                         static_cast<int>(result));
            }
        }
        modernConfiguredIntervalUs_ = 0;
        if ((!initialized_ && !legacyGameOwned_) || configuredIntervalUs_ == 0 || !setSleepMode_) {
            configuredIntervalUs_ = 0;
            return;
        }
        NvVulkanSetSleepModeParams params{};
        params.version = MakeVersion(sizeof(params), 1);
        params.lowLatencyMode = legacyGameOwned_ ? legacyGameLowLatencyMode_ : 0;
        const int status = setSleepMode_(AsHandle(device_), &params);
        if (status != kNvApiOk) {
            LayerLog("Vulkan Reflex: failed to clear driver pacing status=%d", status);
        }
        configuredIntervalUs_ = 0;
    }

    void DestroyLocked() {
        ClearLocked();
        if (initialized_ && destroy_ && device_ != VK_NULL_HANDLE) {
            const int status = destroy_(AsHandle(device_));
            if (status != kNvApiOk) {
                LayerLog("Vulkan Reflex: low-latency device destroy failed status=%d", status);
            }
        }
        initialized_ = false;
        legacyGameOwned_ = false;
        legacyGameLowLatencyMode_ = 0;
        semaphore_ = VK_NULL_HANDLE;
        signalValue_ = 0;
        modernSwapchain_ = VK_NULL_HANDLE;
        modernLastGameSleepTick_ = 0;
        modernSetObserved_ = false;
        modernSleepObserved_ = false;
        nextGameStatusAttemptTick_ = 0;
    }

    void FailLocked() {
        DestroyLocked();
        nextInitAttemptTick_ = GetTickCount64() + 1000;
    }

    std::mutex mutex_;
    VkDevice device_ = VK_NULL_HANDLE;
    DeviceDispatch* dispatch_ = nullptr;
    VkSemaphore semaphore_ = VK_NULL_HANDLE;
    uint64_t signalValue_ = 0;
    uint64_t nextInitAttemptTick_ = 0;
    uint64_t nextGameStatusAttemptTick_ = 0;
    uint32_t configuredIntervalUs_ = 0;
    uint32_t modernConfiguredIntervalUs_ = 0;
    uint32_t initFailureLogCount_ = 0;
    bool initialized_ = false;
    bool legacyGameOwned_ = false;
    uint8_t legacyGameLowLatencyMode_ = 0;
    bool loggedIncomplete_ = false;
    bool modernSetObserved_ = false;
    bool modernSleepObserved_ = false;
    uint64_t modernLastGameSleepTick_ = 0;
    VkSwapchainKHR modernSwapchain_ = VK_NULL_HANDLE;
    VkLatencySleepModeInfoNV modernGameSleepMode_{VK_STRUCTURE_TYPE_LATENCY_SLEEP_MODE_INFO_NV};
    NvApiVulkanInit init_ = nullptr;
    NvApiVulkanDestroy destroy_ = nullptr;
    NvApiVulkanGetSleepStatus getSleepStatus_ = nullptr;
    NvApiVulkanSetSleepMode setSleepMode_ = nullptr;
    NvApiVulkanSleep sleep_ = nullptr;
};

inline VulkanReflexLimiter g_VulkanReflexLimiter;

inline NativeFpsPacingBackend GetVulkanNativeFpsPacingBackend() {
    NativeFpsPacingBackend backend{};
    backend.context = &g_VulkanReflexLimiter;
    backend.isAvailable = [](void* context) { return static_cast<VulkanReflexLimiter*>(context)->IsAvailable(); };
    backend.isGameActive = [](void* context) { return static_cast<VulkanReflexLimiter*>(context)->IsGameActive(); };
    backend.setTargetFps = [](void* context, int fps) {
        return static_cast<VulkanReflexLimiter*>(context)->SetTargetFps(fps);
    };
    backend.sleep = [](void* context, int64_t* waitUs) {
        return static_cast<VulkanReflexLimiter*>(context)->Sleep(waitUs);
    };
    backend.clear = [](void* context) { static_cast<VulkanReflexLimiter*>(context)->Clear(); };
    backend.name = "Vulkan Reflex";
    return backend;
}
