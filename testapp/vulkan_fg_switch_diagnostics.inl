// Included by vulkan_fg_switch_test.cpp; shares the global AppState.

namespace testapp::vkfg {

const char* VkResultName(VkResult result) {
    switch (result) {
        case VK_SUCCESS:
            return "VK_SUCCESS";
        case VK_NOT_READY:
            return "VK_NOT_READY";
        case VK_TIMEOUT:
            return "VK_TIMEOUT";
        case VK_EVENT_SET:
            return "VK_EVENT_SET";
        case VK_EVENT_RESET:
            return "VK_EVENT_RESET";
        case VK_INCOMPLETE:
            return "VK_INCOMPLETE";
        case VK_ERROR_OUT_OF_HOST_MEMORY:
            return "VK_ERROR_OUT_OF_HOST_MEMORY";
        case VK_ERROR_OUT_OF_DEVICE_MEMORY:
            return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
        case VK_ERROR_INITIALIZATION_FAILED:
            return "VK_ERROR_INITIALIZATION_FAILED";
        case VK_ERROR_DEVICE_LOST:
            return "VK_ERROR_DEVICE_LOST";
        case VK_ERROR_MEMORY_MAP_FAILED:
            return "VK_ERROR_MEMORY_MAP_FAILED";
        case VK_ERROR_LAYER_NOT_PRESENT:
            return "VK_ERROR_LAYER_NOT_PRESENT";
        case VK_ERROR_EXTENSION_NOT_PRESENT:
            return "VK_ERROR_EXTENSION_NOT_PRESENT";
        case VK_ERROR_FEATURE_NOT_PRESENT:
            return "VK_ERROR_FEATURE_NOT_PRESENT";
        case VK_ERROR_INCOMPATIBLE_DRIVER:
            return "VK_ERROR_INCOMPATIBLE_DRIVER";
        case VK_ERROR_TOO_MANY_OBJECTS:
            return "VK_ERROR_TOO_MANY_OBJECTS";
        case VK_ERROR_FORMAT_NOT_SUPPORTED:
            return "VK_ERROR_FORMAT_NOT_SUPPORTED";
        case VK_ERROR_FRAGMENTED_POOL:
            return "VK_ERROR_FRAGMENTED_POOL";
        case VK_ERROR_SURFACE_LOST_KHR:
            return "VK_ERROR_SURFACE_LOST_KHR";
        case VK_ERROR_NATIVE_WINDOW_IN_USE_KHR:
            return "VK_ERROR_NATIVE_WINDOW_IN_USE_KHR";
        case VK_SUBOPTIMAL_KHR:
            return "VK_SUBOPTIMAL_KHR";
        case VK_ERROR_OUT_OF_DATE_KHR:
            return "VK_ERROR_OUT_OF_DATE_KHR";
        case VK_ERROR_VALIDATION_FAILED_EXT:
            return "VK_ERROR_VALIDATION_FAILED_EXT";
#ifdef VK_ERROR_INVALID_OPAQUE_CAPTURE_ADDRESS
        case VK_ERROR_INVALID_OPAQUE_CAPTURE_ADDRESS:
            return "VK_ERROR_INVALID_OPAQUE_CAPTURE_ADDRESS";
#endif
        default:
            return "VK_RESULT_UNKNOWN";
    }
}

const char* SlResultName(sl::Result result) {
    switch (result) {
        case sl::Result::eOk:
            return "eOk";
        case sl::Result::eErrorIO:
            return "eErrorIO";
        case sl::Result::eErrorDriverOutOfDate:
            return "eErrorDriverOutOfDate";
        case sl::Result::eErrorOSOutOfDate:
            return "eErrorOSOutOfDate";
        case sl::Result::eErrorOSDisabledHWS:
            return "eErrorOSDisabledHWS";
        case sl::Result::eErrorDeviceNotCreated:
            return "eErrorDeviceNotCreated";
        case sl::Result::eErrorAdapterNotSupported:
            return "eErrorAdapterNotSupported";
        case sl::Result::eErrorNoPlugins:
            return "eErrorNoPlugins";
        case sl::Result::eErrorVulkanAPI:
            return "eErrorVulkanAPI";
        case sl::Result::eErrorFeatureNotSupported:
            return "eErrorFeatureNotSupported";
        case sl::Result::eErrorMissingInputParameter:
            return "eErrorMissingInputParameter";
        case sl::Result::eErrorNotInitialized:
            return "eErrorNotInitialized";
        case sl::Result::eErrorInvalidParameter:
            return "eErrorInvalidParameter";
        default:
            return "sl::Result(unknown)";
    }
}

const char* FfxResultName(ffxReturnCode_t result) {
    switch (result) {
        case FFX_API_RETURN_OK:
            return "FFX_API_RETURN_OK";
        case FFX_API_RETURN_ERROR:
            return "FFX_API_RETURN_ERROR";
        case FFX_API_RETURN_ERROR_UNKNOWN_DESCTYPE:
            return "FFX_API_RETURN_ERROR_UNKNOWN_DESCTYPE";
        case FFX_API_RETURN_ERROR_RUNTIME_ERROR:
            return "FFX_API_RETURN_ERROR_RUNTIME_ERROR";
        case FFX_API_RETURN_ERROR_PARAMETER:
            return "FFX_API_RETURN_ERROR_PARAMETER";
        case FFX_API_RETURN_ERROR_MEMORY:
            return "FFX_API_RETURN_ERROR_MEMORY";
        case FFX_API_RETURN_NO_PROVIDER:
            return "FFX_API_RETURN_NO_PROVIDER";
        default:
            return "FFX_API_RETURN_UNKNOWN";
    }
}

const char* ModeName(FgMode mode) {
    switch (mode) {
        case FgMode::Off:
            return "OFF";
        case FgMode::Dlss:
            return "DLSS FG";
        case FgMode::Fsr:
            return "FSR FG";
        default:
            return "UNKNOWN";
    }
}

const char* TransitionStageName(TransitionStage stage) {
    switch (stage) {
        case TransitionStage::Idle:
            return "idle";
        case TransitionStage::OldPassthroughPending:
            return "old-passthrough-pending";
        case TransitionStage::PreparingReplacement:
            return "preparing-replacement";
        case TransitionStage::ReplacingSwapchain:
            return "replacing-swapchain";
        case TransitionStage::ReplacementPresentPending:
            return "replacement-present-pending";
        case TransitionStage::Activating:
            return "activating";
        case TransitionStage::Rollback:
            return "rollback";
        case TransitionStage::DeviceLost:
            return "device-lost";
        default:
            return "unknown";
    }
}

void LogTransition(const char* event, bool flush) {
    const FgTransitionState& transition = g_App.transition;
    testapp::Log(
        "[FG-TRANSITION] event=%s epoch=%llu stage=%s current=%s target=%s owner=%s targetOwner=%s "
        "swapchain=%p route=%s suspended=%d oldDisabled=%d replacementCommitted=%d frameID=%llu\n",
        event ? event : "unknown", static_cast<unsigned long long>(transition.epoch),
        TransitionStageName(transition.stage), ModeName(transition.currentMode), ModeName(transition.targetMode),
        OwnerName(g_App.swapchain.owner), OwnerName(transition.targetOwner),
        reinterpret_cast<void*>(g_App.swapchain.handle), WsiRouteName(g_App.swapchain.wsi.route),
        transition.suspended ? 1 : 0, transition.oldFgDisabled ? 1 : 0,
        transition.replacementCommitted ? 1 : 0, static_cast<unsigned long long>(g_App.frameId));
    if (flush) {
        testapp::LogFlush();
    }
}

void LogDeviceFault(const char* reason) {
    testapp::Log("[FG-DIAG] DEVICE_LOST reason=%s frameID=%llu owner=%s swapchain=%p\n",
                 reason ? reason : "unknown", static_cast<unsigned long long>(g_App.frameId),
                 OwnerName(g_App.swapchain.owner), reinterpret_cast<void*>(g_App.swapchain.handle));
    if (!g_App.vk.deviceFaultEnabled || g_App.vk.device == VK_NULL_HANDLE) {
        testapp::Log("[FG-DIAG] VK_EXT_device_fault evidence unavailable (extension not enabled)\n");
        testapp::LogFlush();
        return;
    }
    auto getFaultInfo = reinterpret_cast<PFN_vkGetDeviceFaultInfoEXT>(
        vkGetDeviceProcAddr(g_App.vk.device, "vkGetDeviceFaultInfoEXT"));
    if (!getFaultInfo) {
        testapp::Log("[FG-DIAG] vkGetDeviceFaultInfoEXT unresolved\n");
        testapp::LogFlush();
        return;
    }
    VkDeviceFaultCountsEXT counts = {VK_STRUCTURE_TYPE_DEVICE_FAULT_COUNTS_EXT};
    VkResult result = getFaultInfo(g_App.vk.device, &counts, nullptr);
    testapp::Log("[FG-DIAG] vkGetDeviceFaultInfoEXT(counts)=%s(%d) addresses=%u vendors=%u binary=%llu\n",
                 VkResultName(result), static_cast<int>(result), counts.addressInfoCount,
                 counts.vendorInfoCount, static_cast<unsigned long long>(counts.vendorBinarySize));
    if (result == VK_SUCCESS) {
        std::vector<VkDeviceFaultAddressInfoEXT> addresses(counts.addressInfoCount);
        std::vector<VkDeviceFaultVendorInfoEXT> vendors(counts.vendorInfoCount);
        std::vector<uint8_t> binary(static_cast<size_t>(counts.vendorBinarySize));
        VkDeviceFaultInfoEXT info = {VK_STRUCTURE_TYPE_DEVICE_FAULT_INFO_EXT};
        info.pAddressInfos = addresses.data();
        info.pVendorInfos = vendors.data();
        info.pVendorBinaryData = binary.data();
        result = getFaultInfo(g_App.vk.device, &counts, &info);
        testapp::Log("[FG-DIAG] deviceFaultInfo result=%s description='%s'\n", VkResultName(result),
                     info.description);
        for (uint32_t i = 0; i < counts.addressInfoCount; ++i) {
            testapp::Log("[FG-DIAG] faultAddress[%u] type=%u address=0x%llx precision=%llu\n", i,
                         static_cast<unsigned>(addresses[i].addressType),
                         static_cast<unsigned long long>(addresses[i].reportedAddress),
                         static_cast<unsigned long long>(addresses[i].addressPrecision));
        }
        for (uint32_t i = 0; i < counts.vendorInfoCount; ++i) {
            testapp::Log("[FG-DIAG] faultVendor[%u] code=%llu data=%llu description='%s'\n", i,
                         static_cast<unsigned long long>(vendors[i].vendorFaultCode),
                         static_cast<unsigned long long>(vendors[i].vendorFaultData), vendors[i].description);
        }
    }
    testapp::LogFlush();
}

}  // namespace testapp::vkfg
