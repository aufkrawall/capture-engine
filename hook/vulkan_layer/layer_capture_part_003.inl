    if (mem) {
        mem->SetIsHDR(ce::presentation_color::IsHDR(presentationEncoding));
    }
    if (presentationEncoding == ce::presentation_color::Encoding::Unsupported) {
        LayerLog("Vulkan Layer: [Error] Unsupported capture presentation contract format=%d colorSpace=%d", format,
                 colorSpace);
        return;
    }
    LayerLog("Vulkan Layer: Capture presentation contract format=%d colorSpace=%d encoding=%s", format, colorSpace,
             ce::presentation_color::Describe(presentationEncoding));

    // Get LUID for GPU identification
    LUID luid = {};
    if (!GetLUIDFromPhysicalDevice(disp->physicalDevice, &luid)) {
        LayerLog("Vulkan Layer: [Error] Failed to get LUID from physical device");
        return;
    }
    LayerIPC_SetLUID(static_cast<int32_t>(luid.LowPart), static_cast<int32_t>(luid.HighPart));

    uint64_t luidKey = MakeLuidKey(luid);

    SharedMemoryLayout* generationSharedMem = mem;
    const bool retiredLeasesDrained =
        !generationSharedMem || generationSharedMem->frameRing.readIndex.load(std::memory_order_acquire) ==
                                    generationSharedMem->frameRing.writeIndex.load(std::memory_order_acquire);
    for (const auto& retired : g_RetiredCaptureStates) {
        if (retired.device == device && (!retiredLeasesDrained || !CaptureStateCopiesComplete(retired, disp))) {
            return;
        }
    }

    // Check for existing state
    auto it = g_CaptureStates.find(device);
    if (it != g_CaptureStates.end()) {
        const uint32_t normalizedFormat = NormalizeVkFormat(format);
        const size_t expectedSubmissionCount = std::max<size_t>(imageCount, SHARED_TEXTURE_SLOT_COUNT);
        if (it->second.initialized && it->second.luidKey == luidKey && it->second.swapchain == swapchain &&
            it->second.captureWidth == extent.width && it->second.captureHeight == extent.height &&
            it->second.captureFormat == normalizedFormat && it->second.colorSpace == colorSpace &&
            it->second.copyFences.size() == expectedSubmissionCount &&
            it->second.signalSemaphores.size() == imageCount) {
            return;
        }

        SharedMemoryLayout* sharedMem = g_IPCClient.GetSharedMem();
        const bool leasesDrained = !sharedMem || sharedMem->frameRing.readIndex.load(std::memory_order_acquire) ==
                                                     sharedMem->frameRing.writeIndex.load(std::memory_order_acquire);
        if (!leasesDrained || !CaptureStateCopiesComplete(it->second, disp)) {
            static std::atomic<int> s_generationDrainLogCount{0};
            if (s_generationDrainLogCount.fetch_add(1, std::memory_order_relaxed) < 12) {
                LayerLog(
                    "Vulkan Layer: Deferring capture generation switch %p -> %p until old leases/GPU copies "
                    "drain",
                    it->second.swapchain, swapchain);
            }
            return;
        }
        LayerLog("Vulkan Layer: Retiring capture state for replaced swapchain %p -> %p (%ux%u/%u -> %ux%u/%u)",
                 it->second.swapchain, swapchain, it->second.captureWidth, it->second.captureHeight,
                 it->second.captureFormat, extent.width, extent.height, normalizedFormat);
        g_RetiredCaptureStates.emplace_back(std::move(it->second));
        g_CaptureStates.erase(it);
    }

    // Once old leases and GPU copies are complete, no capture state can still
    // reference texture generations for obsolete dimensions/formats. Reclaim
    // them instead of accumulating VRAM across repeated resizes.
    {
        const uint32_t normalizedFormat = NormalizeVkFormat(format);
        std::lock_guard<std::mutex> textureLock(g_InteropMutex);
        for (auto textureIt = g_TextureCache.begin(); textureIt != g_TextureCache.end();) {
            if (textureIt->vkDevice == device &&
                (textureIt->width != extent.width || textureIt->height != extent.height ||
                 textureIt->vkFormat != normalizedFormat)) {
                DestroySharedTextureEntryResources(*textureIt, disp);
                textureIt = g_TextureCache.erase(textureIt);
            } else {
                ++textureIt;
            }
        }
    }

    // Get shared textures (creates if needed)
    // For DXVK, we'll try encoder-created KMT textures first (imported into Vulkan)
    SharedTextureEntry* sharedTextures = nullptr;
    bool usingEncoderTextures = false;
    const VulkanCaptureInteropMode interopMode = DetectVulkanInteropMode();
    const bool allowDxvkEncoderTextures = (interopMode == VulkanCaptureInteropMode::kDxvkD3D11);
    LayerLog("Vulkan Layer: Capture interop mode = %s", VulkanInteropModeToString(interopMode));
    if (interopMode == VulkanCaptureInteropMode::kNative) {
        LayerLog("Vulkan Layer: Native Vulkan detected - encoder KMT adoption disabled");
    }

    if (allowDxvkEncoderTextures && mem) {
        // DXVK D3D11 zero-copy path: import encoder's KMT handles into Vulkan.

        // Publish resolution FIRST so the encoder can create textures
        mem->SetWidth(extent.width);
        mem->SetHeight(extent.height);
        mem->SetFormat(VkFormatToDXGI(format));

        // If existing KMT textures have a different format, clear kmtReady to
        // force the encoder to recreate them with the correct format.
        uint32_t ourDxgiFormat = VkFormatToDXGI(format);
        if (mem->encoderTextures.kmtReady.load(std::memory_order_acquire)) {
            uint32_t existingFormat = mem->encoderTextures.GetFormat();
            if (existingFormat != 0 && existingFormat != ourDxgiFormat) {
                LayerLog("Vulkan Layer: KMT format mismatch (existing=%d, need=%d) - requesting recreation",
                         existingFormat, ourDxgiFormat);
                mem->encoderTextures.kmtReady.store(false, std::memory_order_release);
            }
        }

        LayerLog(
            "Vulkan Layer: DXVK InitCapture - VkFmt=%d NormVkFmt=%d DXGIFmt=%d - "
            "published %dx%d, checking encoder KMT textures",
            format, NormalizeVkFormat((VkFormat)format), VkFormatToDXGI(format), extent.width, extent.height);

        // Never block swapchain creation waiting for the media process. The
        // capture hot path already retries encoder KMT adoption once media has
        // published the textures, so an immediate interop fallback is lossless.
        if (mem->encoderTextures.kmtReady.load(std::memory_order_acquire)) {
            SharedTextureEntry encoderEntry;
            HANDLE kmtHandles[ENCODER_TEXTURE_SLOT_COUNT] = {};
            if (ImportEncoderKmtTextures(device, disp, luidKey, extent.width, extent.height,
                                         NormalizeVkFormat((VkFormat)format), mem, &encoderEntry, kmtHandles)) {
                std::lock_guard<std::mutex> texLock(g_InteropMutex);
                g_TextureCache.push_back(std::move(encoderEntry));
                sharedTextures = &g_TextureCache.back();

                // Publish encoder KMT handles to ring buffer
                LayerIPC_SetTextures(kmtHandles, ENCODER_TEXTURE_SLOT_COUNT, extent.width, extent.height,
                                     VkFormatToDXGI(format));
                // Tell encoder to use its own textures directly
                mem->useEncoderTextures.store(true, std::memory_order_release);
                usingEncoderTextures = true;
                LayerLog("Vulkan Layer: DXVK d3d11 zero-copy: imported %d encoder KMT textures into Vulkan",
                         ENCODER_TEXTURE_SLOT_COUNT);
            } else {
                LayerLog("Vulkan Layer: [Warn] Failed to import encoder KMT textures, falling back to interop");
            }
        }

        if (!usingEncoderTextures) {
            mem->useEncoderTextures.store(false, std::memory_order_release);
            LayerLog(
                "Vulkan Layer: Encoder KMT textures not ready at swapchain creation; using immediate D3D11 "
                "interop fallback and retrying adoption asynchronously");
        }
    }

    // Fallback: D3D11 interop textures (non-DXVK or when encoder textures unavailable)
    if (!sharedTextures) {
        sharedTextures = GetOrCreateSharedTextures(device, disp, disp->physicalDevice, luid, extent.width,
                                                   extent.height, NormalizeVkFormat((VkFormat)format));
    }
    if (!sharedTextures || !sharedTextures->valid) {
        LayerLog("Vulkan Layer: [Error] Failed to get shared textures");
        return;
    }

    if (!usingEncoderTextures) {
        if (!allowDxvkEncoderTextures && mem) {
            LayerLog("Vulkan Layer: Native Vulkan mode - keeping IPC relay path (encoder KMT adoption disabled)");
        }

        // Fallback to our own textures if encoder textures not available
        if (!usingEncoderTextures) {
            if (sharedTextures->hasIpcRelay) {
                LayerIPC_SetTextures(sharedTextures->ipcHandles.data(), (uint32_t)sharedTextures->ipcHandles.size(),
                                     extent.width, extent.height, VkFormatToDXGI(format));
                LayerLog("Vulkan Layer: Publishing IPC relay %s handles to encoder",
                         sharedTextures->ipcHandlesAreNt ? "NT" : "KMT");
            } else {
                LayerIPC_SetTextures(sharedTextures->textureHandles.data(),
                                     (uint32_t)sharedTextures->textureHandles.size(), extent.width, extent.height,
                                     VkFormatToDXGI(format));
                LayerLog("Vulkan Layer: Publishing %s handles to encoder",
                         sharedTextures->textureHandlesAreNt ? "NT" : "KMT");
            }
        }
    }

    VulkanCaptureState state = {};
    state.device = device;
    state.swapchain = swapchain;
    state.luidKey = luidKey;
    state.captureWidth = extent.width;
    state.captureHeight = extent.height;
    state.captureFormat = NormalizeVkFormat((VkFormat)format);
    state.colorSpace = colorSpace;
    state.interopMode = interopMode;
    state.initialized = false;

    const uint32_t captureSubmissionCount = std::max<uint32_t>(imageCount, SHARED_TEXTURE_SLOT_COUNT);
    state.copyFences.resize(captureSubmissionCount);
    VkFenceCreateInfo fenceInfo = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, nullptr, VK_FENCE_CREATE_SIGNALED_BIT};
    for (uint32_t i = 0; i < captureSubmissionCount; i++) {
        const VkResult fenceResult = disp->fp_vkCreateFence(device, &fenceInfo, nullptr, &state.copyFences[i]);
        if (fenceResult != VK_SUCCESS || state.copyFences[i] == VK_NULL_HANDLE) {
            LayerLog("Vulkan Layer: [Error] Failed to create capture fence %u/%u (result=%d)", i,
                     captureSubmissionCount, fenceResult);
            DestroyCaptureStateResources(state, disp);
            return;
        }
    }

    state.signalSemaphores.resize(imageCount);
    state.presentedImages.assign(imageCount, false);
    VkSemaphoreCreateInfo semInfo = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, nullptr, 0};
    for (uint32_t i = 0; i < imageCount; i++) {
        const VkResult signalSemaphoreResult =
            disp->fp_vkCreateSemaphore(device, &semInfo, nullptr, &state.signalSemaphores[i]);
        if (signalSemaphoreResult != VK_SUCCESS || state.signalSemaphores[i] == VK_NULL_HANDLE) {
            LayerLog("Vulkan Layer: [Error] Failed to create capture signal semaphore %u/%u (result=%d)", i, imageCount,
                     signalSemaphoreResult);
            DestroyCaptureStateResources(state, disp);
            return;
        }
    }

    // D3D12_FENCE produces a standard fence handle that D3D11 OpenSharedFence
    // can consume in both the relay and media processes. OPAQUE_WIN32 is not a
    // D3D fence and must never be published as though it were one.
    constexpr VkExternalSemaphoreHandleTypeFlagBits semHandleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT;

    VkSemaphoreTypeCreateInfo timelineInfo = {VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
    timelineInfo.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    timelineInfo.initialValue = 0;

    // Explicit security attributes for cross-process fence access.
    // Without this, the D3D12_FENCE handle may lack sufficient access rights
    // and OpenSharedFence fails in the encoder process.
    SECURITY_ATTRIBUTES secAttr = {};
    secAttr.nLength = sizeof(SECURITY_ATTRIBUTES);
    secAttr.bInheritHandle = FALSE;
    secAttr.lpSecurityDescriptor = nullptr;  // Default security descriptor (full access)

    VkExportSemaphoreWin32HandleInfoKHR exportWin32Info = {VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_WIN32_HANDLE_INFO_KHR};
    exportWin32Info.pAttributes = &secAttr;
    exportWin32Info.dwAccess = GENERIC_ALL;
    exportWin32Info.name = nullptr;
    exportWin32Info.pNext = &timelineInfo;

    VkExportSemaphoreCreateInfo exportInfo = {VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO};
    exportInfo.handleTypes = semHandleType;
    exportInfo.pNext = &exportWin32Info;

    VkSemaphoreCreateInfo timelineSemInfo = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    timelineSemInfo.pNext = &exportInfo;

    VkResult semResult = disp->fp_vkCreateSemaphore(device, &timelineSemInfo, nullptr, &state.timelineSemaphore);

    if (semResult == VK_SUCCESS) {
        if (disp->fp_vkGetSemaphoreWin32HandleKHR) {
            // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization) - zero-initialized placeholder; enum fields are assigned before use
            VkSemaphoreGetWin32HandleInfoKHR getHandleInfo = {VK_STRUCTURE_TYPE_SEMAPHORE_GET_WIN32_HANDLE_INFO_KHR};
            getHandleInfo.semaphore = state.timelineSemaphore;
            getHandleInfo.handleType = semHandleType;

            HANDLE hFence = nullptr;
            VkResult fenceRes = disp->fp_vkGetSemaphoreWin32HandleKHR(device, &getHandleInfo, &hFence);

            if (fenceRes == VK_SUCCESS && hFence) {
                state.sharedFenceHandle = hFence;
                LayerLog("Vulkan Layer: Created D3D12 shared fence %p", hFence);
            } else {
                LayerLog("Vulkan Layer: [Error] Failed to get fence handle (vkResult=%d)", fenceRes);
            }
        }
    } else {
        LayerLog("Vulkan Layer: [Error] Failed to create timeline semaphore (vkResult=%d)", semResult);
    }
    if (semResult != VK_SUCCESS || state.timelineSemaphore == VK_NULL_HANDLE || !state.sharedFenceHandle) {
        LayerLog(
            "Vulkan Layer: [Error] Capture initialization requires an exportable timeline fence; disabling "
            "capture for this swapchain");
        DestroyCaptureStateResources(state, disp);
        return;
    }

    // Set up D3D11 relay for IPC if needed (KMT path with relay textures)
    if (sharedTextures->hasIpcRelay && state.sharedFenceHandle) {
        std::lock_guard<std::mutex> interopLock(g_InteropMutex);
        D3D11InteropDevice* interopDev = nullptr;
        for (auto& dev : g_D3D11Devices) {
            if (dev.luidKey == luidKey && dev.valid) {
                interopDev = &dev;
                break;
            }
        }
        if (interopDev) {
            // Get ID3D11Device5 for OpenSharedFence
            ComPtr<ID3D11Device5> device5;
            HRESULT hr = interopDev->device->QueryInterface(IID_PPV_ARGS(&device5));
            if (SUCCEEDED(hr) && device5) {
                hr = device5->OpenSharedFence(state.sharedFenceHandle, IID_PPV_ARGS(&state.d3d11Fence));
                if (SUCCEEDED(hr) && state.d3d11Fence) {
                    LayerLog("Vulkan Layer: Opened D3D11 fence for IPC relay");
                } else {
                    LayerLog("Vulkan Layer: [Error] OpenSharedFence failed (hr=0x%08X)", hr);
                }
            } else {
                LayerLog("Vulkan Layer: [Error] ID3D11Device5 unavailable (hr=0x%08X)", hr);
            }

            // Get ID3D11DeviceContext4 for Wait/Signal
            hr = interopDev->context->QueryInterface(IID_PPV_ARGS(&state.d3d11Context4));
            if (SUCCEEDED(hr) && state.d3d11Context4) {
                LayerLog("Vulkan Layer: IPC relay D3D11 context ready");
            } else {
                LayerLog("Vulkan Layer: [Error] ID3D11DeviceContext4 unavailable (hr=0x%08X)", hr);
                if (state.d3d11Fence) {
                    state.d3d11Fence->Release();
                    state.d3d11Fence = nullptr;
                }
            }

            // Create a D3D11-native shared IPC fence whenever relay is active and D3D11 context is available.
            // Even when Vulkan exports D3D12_FENCE, some driver/runtime combinations can fail OpenSharedFence
            // in the encoder process. A D3D11-created shared fence is generally more interoperable.
            if (state.d3d11Fence && state.d3d11Context4 && device5) {
                hr = device5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&state.d3d11IpcFence));
                if (SUCCEEDED(hr) && state.d3d11IpcFence) {
                    SECURITY_ATTRIBUTES ipcFenceSecAttr = {};
                    ipcFenceSecAttr.nLength = sizeof(ipcFenceSecAttr);
                    ipcFenceSecAttr.bInheritHandle = FALSE;
                    ipcFenceSecAttr.lpSecurityDescriptor = nullptr;

                    state.ipcFenceHandle = nullptr;
                    hr = state.d3d11IpcFence->CreateSharedHandle(&ipcFenceSecAttr, GENERIC_ALL, nullptr,
                                                                 &state.ipcFenceHandle);
                    if (SUCCEEDED(hr) && state.ipcFenceHandle) {
                        LayerLog("Vulkan Layer: Created D3D11 IPC fence, handle=%p", state.ipcFenceHandle);
                    } else {
                        LayerLog("Vulkan Layer: [Error] IPC fence CreateSharedHandle failed (hr=0x%08X, handle=%p)", hr,
                                 state.ipcFenceHandle);
                        if (state.ipcFenceHandle) {
                            CloseHandle(state.ipcFenceHandle);
                            state.ipcFenceHandle = nullptr;
                        }
                        state.d3d11IpcFence->Release();
                        state.d3d11IpcFence = nullptr;
                    }
                } else {
                    LayerLog("Vulkan Layer: [Error] CreateFence for IPC failed (hr=0x%08X)", hr);
                }
            }
        }
    }
    if (sharedTextures->hasIpcRelay && (!state.d3d11Fence || !state.d3d11Context4 || sharedTextures->vkImages.empty() ||
                                        sharedTextures->ipcTextures.size() < sharedTextures->vkImages.size() ||
                                        sharedTextures->textures.size() < sharedTextures->vkImages.size())) {
        LayerLog(
            "Vulkan Layer: [Error] IPC relay synchronization/resources are incomplete; capture will not "
            "publish unsynchronized relay textures");
        DestroyCaptureStateResources(state, disp);
        return;
    }

    // Publish the appropriate fence handle for the encoder.
    // When relay uses the shared Vulkan fence as its signal target, publish that
    // handle so the encoder can still wait on the post-relay fence value.
    HANDLE fenceToPublish = nullptr;
    if (state.ipcFenceHandle) {
        fenceToPublish = state.ipcFenceHandle;
    } else if (state.sharedFenceHandle) {
        fenceToPublish = state.sharedFenceHandle;
        if (sharedTextures->hasIpcRelay) {
            LayerLog("Vulkan Layer: [Warn] IPC relay fence export unavailable; falling back to shared Vulkan fence");
        }
    }

    mem = g_IPCClient.GetSharedMem();
    if (fenceToPublish) {
        if (mem) {
            if (mem->encoderTextures.ready.load(std::memory_order_acquire)) {
                mem->encoderTextures.SetFenceHandle((uint64_t)fenceToPublish);
                LayerLog("Vulkan Layer: Published capture fence handle %p to encoderTextures", fenceToPublish);
            } else {
                LayerIPC_SetFence(fenceToPublish);
                LayerLog("Vulkan Layer: Published capture fence handle %p", fenceToPublish);
            }
        }
    } else if (mem) {
        if (mem->encoderTextures.ready.load(std::memory_order_acquire)) {
            mem->encoderTextures.SetFenceHandle(0);
        } else {
            LayerIPC_SetFence(nullptr);
        }
    }

    state.initialized = true;
    LayerLog("Vulkan Layer: Zero-Copy Capture Initialized (%dx%d)", extent.width, extent.height);
    g_CaptureStates[device] = std::move(state);
}

void NoteCaptureSwapchainImagePresented(VkDevice device, VkSwapchainKHR swapchain, uint32_t imageIndex) {
    std::lock_guard<std::mutex> lock(g_CaptureMutex);
    auto current = g_CaptureStates.find(device);
    if (current == g_CaptureStates.end() || current->second.swapchain != swapchain ||
        imageIndex >= current->second.presentedImages.size()) {
        return;
    }

    const bool imageWasPresented = current->second.presentedImages[imageIndex];
    current->second.presentedImages[imageIndex] = true;
    if (!imageWasPresented)
        return;

    // Reacquiring and presenting the same image proves its preceding present
    // wait was consumed. At that point capture semaphores from older swapchains
    // can no longer be referenced by the presentation engine.
    DeviceDispatch* disp = VulkanLayerState::Get().GetDeviceDispatch(device);
    for (auto retired = g_RetiredCaptureStates.begin(); retired != g_RetiredCaptureStates.end();) {
        if (retired->device == device && CaptureStateCopiesComplete(*retired, disp)) {
            DestroyCaptureStateResources(*retired, disp);
            retired = g_RetiredCaptureStates.erase(retired);
        } else {
            ++retired;
        }
    }
}

void RetireCaptureSwapchain(VkDevice device, VkSwapchainKHR swapchain) {
    if (device == VK_NULL_HANDLE || swapchain == VK_NULL_HANDLE)
        return;
    std::lock_guard<std::mutex> lock(g_CaptureMutex);
    auto it = g_CaptureStates.find(device);
    if (it == g_CaptureStates.end() || it->second.swapchain != swapchain)
        return;
    it->second.initialized = false;
    g_RetiredCaptureStates.emplace_back(std::move(it->second));
    g_CaptureStates.erase(it);
    LayerLog("Vulkan Layer: Retired capture state for destroyed swapchain %p", swapchain);
}

void CleanupCapture(VkDevice device) {
    std::lock_guard<std::mutex> lock(g_CaptureMutex);
    DeviceDispatch* disp = VulkanLayerState::Get().GetDeviceDispatch(device);
    uint64_t luidKey = 0;

    auto it = g_CaptureStates.find(device);
    if (it != g_CaptureStates.end()) {
        luidKey = it->second.luidKey;
        DestroyCaptureStateResources(it->second, disp);
        g_CaptureStates.erase(it);
    }
    for (auto retired = g_RetiredCaptureStates.begin(); retired != g_RetiredCaptureStates.end();) {
        if (retired->device == device) {
            if (luidKey == 0)
                luidKey = retired->luidKey;
            DestroyCaptureStateResources(*retired, disp);
            retired = g_RetiredCaptureStates.erase(retired);
        } else {
            ++retired;
        }
    }

    if (luidKey != 0 && disp) {
        // CRITICAL FIX: Clean up texture cache entries for this device
        // This prevents memory leaks of D3D11 textures and Vulkan images
        std::lock_guard<std::mutex> interopLock(g_InteropMutex);
        for (auto entry = g_TextureCache.begin(); entry != g_TextureCache.end();) {
            if (entry->vkDevice == device) {
                DestroySharedTextureEntryResources(*entry, disp);
                LayerLog("Vulkan Layer: Cleaned up texture cache entry for LUID %llx", luidKey);
                entry = g_TextureCache.erase(entry);
            } else {
                ++entry;
            }
        }

        // Signal media process to release its preserved encoder textures.
        // The VkImages we exported are gone; the D3D11 textures in the media process
        // are now unused and should be freed to reclaim VRAM.
        auto* sharedMem = g_IPCClient.GetSharedMem();
        if (sharedMem && sharedMem->useEncoderTextures.load(std::memory_order_acquire)) {
            sharedMem->useEncoderTextures.store(false, std::memory_order_release);
            sharedMem->encoderTextures.kmtReady.store(false, std::memory_order_release);
            LayerLog("Vulkan Layer: Cleared useEncoderTextures on vkDestroyDevice");
        }
    }
}

VkSemaphore GetCaptureSemaphore(VkDevice device, VkSwapchainKHR swapchain, uint32_t imageIndex) {
    std::lock_guard<std::mutex> lock(g_CaptureMutex);
    auto it = g_CaptureStates.find(device);
    if (it != g_CaptureStates.end() && it->second.initialized && it->second.swapchain == swapchain) {
        if (imageIndex < it->second.signalSemaphores.size()) {
            return it->second.signalSemaphores[imageIndex];
        }
    }
    return VK_NULL_HANDLE;
}

bool CaptureFrame(VkDevice device, VkSwapchainKHR swapchain, VkQueue queue, VkImage srcImage,
                  const VkSemaphore* waitSemaphores, uint32_t waitSemaphoreCount, VkSemaphore signalSemaphore) {
    std::lock_guard<std::mutex> lock(g_CaptureMutex);

    auto it = g_CaptureStates.find(device);
    if (it == g_CaptureStates.end() || !it->second.initialized || it->second.swapchain != swapchain)
        return false;

    // Check if we should throttle capture (encoder is falling behind)
    if (g_IPCClient.GetSharedMem()) {
        if (g_IPCClient.GetSharedMem()->throttleCapture.load(std::memory_order_acquire)) {
            return false;
        }
    }

    VulkanCaptureState& state = it->second;
    DeviceDispatch* disp = VulkanLayerState::Get().GetDeviceDispatch(device);
    if (!disp)
        return false;
    if (signalSemaphore == VK_NULL_HANDLE || srcImage == VK_NULL_HANDLE || queue == VK_NULL_HANDLE) {
        return false;
    }

    const uint32_t queueFamilyIndex = VulkanLayerState::Get().GetQueueFamilyIndex(queue);
    if (queueFamilyIndex == VK_QUEUE_FAMILY_IGNORED) {
        static std::atomic<int> s_unknownQueueLogCount{0};
        if (s_unknownQueueLogCount.fetch_add(1, std::memory_order_relaxed) < 5) {
            LayerLog("Vulkan Layer: Capture skipped because present queue family is unknown");
        }
        return false;
    }

    if (!VulkanLayerState::Get().QueueSupportsTransfer(queue)) {
        static std::atomic<int> s_nonTransferQueueLogCount{0};
        if (s_nonTransferQueueLogCount.fetch_add(1, std::memory_order_relaxed) < 5) {
            LayerLog("Vulkan Layer: Capture skipped on queue family %u without transfer support", queueFamilyIndex);
        }
        return false;
    }

    VulkanCaptureState::CommandResources* commandResources =
        EnsureCaptureCommandResources(state, disp, device, queueFamilyIndex);
    if (!commandResources) {
        return false;
    }

    auto* mem = g_IPCClient.GetSharedMem();
    const bool allowDxvkEncoderTextures = (state.interopMode == VulkanCaptureInteropMode::kDxvkD3D11);
    const bool encoderAdoptionRequested = allowDxvkEncoderTextures && mem &&
                                          !mem->useEncoderTextures.load(std::memory_order_acquire) &&
                                          mem->encoderTextures.kmtReady.load(std::memory_order_acquire) &&
                                          state.captureFrameCounter >= state.nextEncoderImportRetryFrame;
    if (encoderAdoptionRequested && mem->frameRing.readIndex.load(std::memory_order_acquire) !=
                                        mem->frameRing.writeIndex.load(std::memory_order_acquire)) {
        // Handles/fence are generation-global in shared memory. Stop producing
        // briefly so every old-generation lease drains before replacing them.
        static std::atomic<int> s_encoderAdoptionDrainLogCount{0};
        if (s_encoderAdoptionDrainLogCount.fetch_add(1, std::memory_order_relaxed) < 8) {
            LayerLog("Vulkan Layer: Deferring encoder-texture adoption until old capture leases drain");
        }
        return false;
    }
    if (encoderAdoptionRequested && !CaptureStateCopiesComplete(state, disp))
        return false;
    if (encoderAdoptionRequested) {
        SharedTextureEntry encoderEntry;
        HANDLE kmtHandles[ENCODER_TEXTURE_SLOT_COUNT] = {};
        if (ImportEncoderKmtTextures(device, disp, state.luidKey, state.captureWidth, state.captureHeight,
                                     state.captureFormat, mem, &encoderEntry, kmtHandles)) {
            {
                std::lock_guard<std::mutex> texLock(g_InteropMutex);
                for (auto textureIt = g_TextureCache.begin(); textureIt != g_TextureCache.end();) {
                    if (textureIt->vkDevice == device && textureIt->luidKey == state.luidKey &&
                        textureIt->width == state.captureWidth && textureIt->height == state.captureHeight &&
                        textureIt->vkFormat == state.captureFormat) {
                        DestroySharedTextureEntryResources(*textureIt, disp);
                        textureIt = g_TextureCache.erase(textureIt);
                    } else {
                        ++textureIt;
                    }
                }
                g_TextureCache.push_back(std::move(encoderEntry));
            }

            LayerIPC_SetTextures(kmtHandles, ENCODER_TEXTURE_SLOT_COUNT, state.captureWidth, state.captureHeight,
                                 VkFormatToDXGI((VkFormat)state.captureFormat));
            mem->useEncoderTextures.store(true, std::memory_order_release);
            state.sharedImageInitialized.fill(false);
            state.relayCompletionValues.fill(0);
            LayerLog("Vulkan Layer: DXVK d3d11 zero-copy: adopted encoder KMT textures after media startup");
        } else {
            state.nextEncoderImportRetryFrame = state.captureFrameCounter + 60;
        }
    }

    // Get shared textures from cache
    SharedTextureEntry* sharedTextures = nullptr;
    for (auto& entry : g_TextureCache) {
        if (entry.vkDevice == device && entry.luidKey == state.luidKey && entry.width == state.captureWidth &&
            entry.height == state.captureHeight && entry.vkFormat == state.captureFormat && entry.valid) {
            sharedTextures = &entry;
            break;
        }
    }
    if (!sharedTextures || !sharedTextures->valid)
        return false;
    if (state.copyFences.empty() || commandResources->buffers.empty() || state.signalSemaphores.empty() ||
        sharedTextures->vkImages.empty()) {
        LayerLog("Vulkan Layer: Capture skipped because synchronization/image resources are incomplete");
        return false;
    }

    // Use a monotonic counter independent of swapchain image patterns, but
    // rotate over the actual shared texture count rather than assuming four.
    const uint32_t sharedTextureCount =
        static_cast<uint32_t>(std::min<size_t>(sharedTextures->vkImages.size(), SHARED_TEXTURE_SLOT_COUNT));
    if (sharedTextureCount == 0)
        return false;
    const bool doRelay = sharedTextures->hasIpcRelay && state.d3d11Fence && state.d3d11Context4;
    ID3D11Fence* relayCompletionFence = state.d3d11IpcFence ? state.d3d11IpcFence : state.d3d11Fence;
    const uint32_t firstSlot = static_cast<uint32_t>(state.captureFrameCounter++ % sharedTextureCount);
    int32_t availableSlot = -1;
    uint32_t cpuBusySlots = 0;
    uint32_t gpuBusySlots = 0;
    for (uint32_t offset = 0; offset < sharedTextureCount; ++offset) {
        const uint32_t candidate = (firstSlot + offset) % sharedTextureCount;
        if (IsCaptureTextureSlotOutstanding(mem, static_cast<int32_t>(candidate))) {
            ++cpuBusySlots;
            continue;
        }
        if (doRelay && state.relayCompletionValues[candidate] != 0) {
            const uint64_t completedRelayValue = relayCompletionFence->GetCompletedValue();
            if (completedRelayValue == UINT64_MAX) {
                LayerLog("Vulkan Layer: IPC relay fence reported device removal; disabling capture state");
                state.initialized = false;
                return false;
            }
            if (completedRelayValue < state.relayCompletionValues[candidate]) {
                ++gpuBusySlots;
                continue;
            }
        }
        const uint32_t candidateFenceIndex = candidate % state.copyFences.size();
        const VkFence candidateFence = state.copyFences[candidateFenceIndex];
        const VkResult fenceStatus = disp->fp_vkWaitForFences(device, 1, &candidateFence, VK_TRUE, 0);
        if (fenceStatus != VK_SUCCESS) {
            if (fenceStatus == VK_ERROR_DEVICE_LOST) {
                state.initialized = false;
                return false;
            }
            ++gpuBusySlots;
            continue;
        }
        availableSlot = static_cast<int32_t>(candidate);
        break;
