// Exported FidelityFX entry points, included by ffx_framegeneration.cpp.

using namespace ce::flow::fake;

extern "C" __declspec(dllexport) uint64_t CEFlow_FFXUiRegistrations(bool enabled) {
    std::lock_guard<std::mutex> lock(g_configMutex);
    return enabled ? g_uiRegistrationsEnabled : g_uiRegistrationsDisabled;
}

extern "C" FFX_API_ENTRY ffxReturnCode_t ffxCreateContext(ffxContext* context, ffxCreateContextDescHeader* desc,
                                                          const ffxAllocationCallbacks*) {
    if (!context || !desc)
        return FFX_API_RETURN_ERROR_PARAMETER;
    if (const auto* forHwnd = FindHeader(desc, FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_FOR_HWND_DX12))
        return CreateSwapchainContext(
            context, *reinterpret_cast<const ffxCreateContextDescFrameGenerationSwapChainForHwndDX12*>(forHwnd));
    if (FindHeader(desc, FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION)) {
        *context = new FrameGenerationContext();
        Log(kModule, "ffxCreateContext(frame generation) -> %p", *context);
        return FFX_API_RETURN_OK;
    }
    return FFX_API_RETURN_ERROR_UNKNOWN_DESCTYPE;
}

extern "C" FFX_API_ENTRY ffxReturnCode_t ffxDestroyContext(ffxContext* context, const ffxAllocationCallbacks*) {
    if (!context || !*context)
        return FFX_API_RETURN_ERROR_PARAMETER;
    auto* base = static_cast<Context*>(*context);
    Log(kModule, "ffxDestroyContext(%p kind=%u)", base, static_cast<uint32_t>(base->kind));
    if (base->kind == ContextKind::kSwapchain) {
        std::lock_guard<std::mutex> lock(g_contextMutex);
        if (g_swapchainContext == base) {
            g_swapchainContext = nullptr;
            std::lock_guard<std::mutex> configLock(g_configMutex);
            g_config.ui = {};
            g_config.uiFlags = 0;
            g_config.uiOwner.Reset();
        }
    } else {
        std::lock_guard<std::mutex> lock(g_configMutex);
        // Destroying an effect context stops generation, and any callback associated with the
        // destroyed effect context must not be invoked on subsequent presents.
        g_config.enabled = false;
        g_config.frameGenerationCallback = nullptr;
        g_config.frameGenerationCallbackUserContext = nullptr;
        g_config.presentCallback = nullptr;
        g_config.presentCallbackUserContext = nullptr;
    }
    delete base;
    *context = nullptr;
    return FFX_API_RETURN_OK;
}

extern "C" FFX_API_ENTRY ffxReturnCode_t ffxConfigure(ffxContext* context, const ffxConfigureDescHeader* desc) {
    if (!context || !*context || !desc)
        return FFX_API_RETURN_ERROR_PARAMETER;
    if (desc->type == FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION) {
        const auto& configure = *reinterpret_cast<const ffxConfigureDescFrameGeneration*>(desc);
        if (configure.frameID == UINT64_MAX)
            return FFX_API_RETURN_ERROR_PARAMETER;
        std::lock_guard<std::mutex> lock(g_configMutex);
        if (configure.frameGenerationEnabled != g_config.enabled ||
            configure.presentCallback != g_config.presentCallback)
            Log(kModule, "ffxConfigure(frame generation) enabled=%d presentCallback=%p (was enabled=%d) frameID=%llu",
                configure.frameGenerationEnabled ? 1 : 0, reinterpret_cast<void*>(configure.presentCallback),
                g_config.enabled ? 1 : 0, static_cast<unsigned long long>(configure.frameID));
        g_config.enabled = configure.frameGenerationEnabled;
        g_config.presentCallback = configure.presentCallback;
        g_config.presentCallbackUserContext = configure.presentCallbackUserContext;
        g_config.frameGenerationCallback = configure.frameGenerationCallback;
        g_config.frameGenerationCallbackUserContext = configure.frameGenerationCallbackUserContext;
        return FFX_API_RETURN_OK;
    }
    if (desc->type == FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_REGISTERUIRESOURCE_DX12) {
        const auto& ui = *reinterpret_cast<const ffxConfigureDescFrameGenerationSwapChainRegisterUiResourceDX12*>(desc);
        std::lock_guard<std::mutex> lock(g_configMutex);
        ++(g_config.enabled ? g_uiRegistrationsEnabled : g_uiRegistrationsDisabled);
        if (ui.uiResource.resource != g_config.ui.resource)
            Log(kModule, "ffxConfigure(register UI resource) %p (%ux%u state=0x%X) was %p", ui.uiResource.resource,
                ui.uiResource.description.width, ui.uiResource.description.height, ui.uiResource.state,
                g_config.ui.resource);
        g_config.ui = ui.uiResource;
        g_config.uiOwner = static_cast<ID3D12Resource*>(ui.uiResource.resource);
        g_config.uiFlags = ui.flags;
        return FFX_API_RETURN_OK;
    }
    return FFX_API_RETURN_ERROR_UNKNOWN_DESCTYPE;
}

extern "C" FFX_API_ENTRY ffxReturnCode_t ffxQuery(ffxContext*, ffxQueryDescHeader*) {
    return FFX_API_RETURN_OK;
}

extern "C" FFX_API_ENTRY ffxReturnCode_t ffxDispatch(ffxContext* context, const ffxDispatchDescHeader* desc) {
    if (!context || !*context || !desc)
        return FFX_API_RETURN_ERROR_PARAMETER;
    if (desc->type == FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION) {
        std::lock_guard<std::mutex> lock(g_contextMutex);
        if (!g_swapchainContext || !g_swapchainContext->proxy)
            return FFX_API_RETURN_ERROR_RUNTIME_ERROR;
        return g_swapchainContext->proxy->RecordGeneration(
            *reinterpret_cast<const ffxDispatchDescFrameGeneration*>(desc));
    }
    if (desc->type == FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_WAIT_FOR_PRESENTS_DX12) {
        auto* base = static_cast<Context*>(*context);
        if (base->kind != ContextKind::kSwapchain)
            return FFX_API_RETURN_ERROR_PARAMETER;
        static_cast<SwapchainContext*>(base)->proxy->WaitForPresents();
    }
    // PREPARE(_V2) and the rest need no work here.
    return FFX_API_RETURN_OK;
}
