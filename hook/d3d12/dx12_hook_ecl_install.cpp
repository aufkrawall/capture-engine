#include "dx12_hook_internal.h"
#include "common/logging/log_meter.h"
#include "dx12_hook_ecl_shared.h"


__attribute__((noinline)) void DX12_HookQueueVTable(ID3D12CommandQueue* queue) {
    if (!queue)
        return;

    // Safety: freed COM objects have null vtable — skip
    void** vtbl = *reinterpret_cast<void***>(queue);
    if (!vtbl)
        return;

    // Never hook our own overlay queue to avoid re-entry in ECL
    if (queue == dx12_hook_g_State.overlayQueue)
        return;

    if (ShouldQuiesceCESideEffectsForProtectedOfficialFFXStartup()) {
        static std::atomic<int> s_protectedOfficialFFXQueueHookSkipLogCount{0};
        static ce::log_meter::ChangeGate s_protectedOfficialFFXQueueHookSkipGate;
        const int logCount = s_protectedOfficialFFXQueueHookSkipLogCount.fetch_add(1, std::memory_order_relaxed);
        const auto verdict = s_protectedOfficialFFXQueueHookSkipGate.ObserveOrEvery(
            ce::log_meter::FieldKey(queue), static_cast<uint32_t>(logCount) + 1, 1024);
        if (verdict) {
            HookLogImportant(
                "DX12: Protected official FFX startup pending - skipping ExecuteCommandLists vtable hook refresh "
                "(queue=%p count=%d)%s",
                queue, logCount + 1, ce::log_meter::SuppressedNote(verdict.suppressed).c_str());
        }
        return;
    }

    char vtableModulePath[MAX_PATH] = {};
    char executeModulePath[MAX_PATH] = {};
    const bool vtableModuleResolved =
        ce::overlay_compat::TryGetModulePathFromCodeAddress(reinterpret_cast<const void*>(vtbl), vtableModulePath, sizeof(vtableModulePath));
    const bool executeModuleResolved = vtbl[10] && ce::overlay_compat::TryGetModulePathFromCodeAddress(
                                                       vtbl[10], executeModulePath, sizeof(executeModulePath));
    const bool vtableFromStreamline =
        vtableModuleResolved && ce::overlay_compat::IsStreamlineFrameGenerationModulePath(vtableModulePath);
    const bool executeFromStreamline =
        executeModuleResolved && ce::overlay_compat::IsStreamlineFrameGenerationModulePath(executeModulePath);
    const bool vtableFromFFX =
        vtableModuleResolved && ce::overlay_compat::IsFFXFrameGenerationModulePath(vtableModulePath);
    const bool executeFromFFX =
        executeModuleResolved && ce::overlay_compat::IsFFXFrameGenerationModulePath(executeModulePath);
    if (ce::dx12_overlay_policy::ShouldSkipCommandQueueVTableHookForFrameGenerationRuntimeModule(
            vtableFromStreamline, executeFromStreamline, vtableFromFFX, executeFromFFX)) {
        static std::atomic<int> s_fgRuntimeQueueVTableSkipLogCount{0};
        const int logCount = s_fgRuntimeQueueVTableSkipLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logCount < 20 || (logCount % 256) == 0) {
            HookLogImportant(
                "DX12: Skipping ExecuteCommandLists vtable hook for FG-runtime queue %p "
                "(vtbl=%p vtblModule=%s ecl=%p eclModule=%s origGame=%p scQueue=%p count=%d)",
                queue, vtbl, vtableModulePath[0] ? vtableModulePath : "unknown", vtbl[10],
                executeModulePath[0] ? executeModulePath : "unknown", dx12_hook_g_OriginalGameQueue, dx12_hook_g_SwapchainQueue,
                logCount + 1);
        }
        return;
    }

    // Skip vtable hooking on SL wrapper queues during Streamline startup.
    // During pure-DLSS cold start, Streamline creates COM wrapper queues that
    // inherit the shared vtable.  Hooking vtable[10] on these wrappers and then
    // intercepting their ECL calls during Streamline's critical initialization
    // phase can crash Streamline (null pointer call at RIP=0).  The non-origGame
    // check covers these transient wrapper queues without affecting the game queue
    // or the swapchain queue that we need for overlay/heartbeat.
    if (queue != dx12_hook_g_OriginalGameQueue && queue != dx12_hook_g_SwapchainQueue &&
        DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire) &&
        DXGIShared::IsStreamlineStartupTransitionWindowActive()) {
        static int s_skipSLWrapperVTableHookLog = 0;
        if (s_skipSLWrapperVTableHookLog < 10) {
            HookLogImportant("DX12: Skipping vtable hook for non-origGame queue %p during SL startup window", queue);
        }
        s_skipSLWrapperVTableHookLog++;
        return;
    }

    // We ALWAYS hook the queue for freeze detection heartbeat
    // The overlay rendering is skipped separately in ProcessFrameExternal if
    // needed This ensures freeze watchdog works even with DLSS/FSR FG

    void* unwrapped = nullptr;
    if (SUCCEEDED(queue->QueryInterface(IID_CWrapD3D12CommandQueue, &unwrapped))) {
        ((IUnknown*)unwrapped)->Release();
        return;
    }
    static std::recursive_mutex s_HookMutex;
    std::lock_guard<std::recursive_mutex> lock(s_HookMutex);
    vtbl = *reinterpret_cast<void***>(queue);

    const auto capture = ce::dx12_queue_dispatch::CaptureVTable(vtbl);
    if (capture.result == ce::dx12_queue_dispatch::CaptureResult::kKnown ||
        capture.result == ce::dx12_queue_dispatch::CaptureResult::kFollower) {
        if (capture.result == ce::dx12_queue_dispatch::CaptureResult::kFollower) {
            static ce::log_meter::ChangeGate follower;
            const auto verdict = follower.Observe(ce::log_meter::FieldKey(vtbl, vtbl[10]));
            if (verdict) {
                HookLogImportant("DX12 ECL: preserving follower chain vtable=%p current=%p%s", vtbl, vtbl[10],
                                 ce::log_meter::SuppressedNote(verdict.suppressed).c_str());
            }
        }
        return;
    }
    if (capture.result == ce::dx12_queue_dispatch::CaptureResult::kCaptured &&
        !dx12_hook_g_RealD3D12ECL.load(std::memory_order_acquire)) {
        TryPublishRealD3D12ECLCandidate(capture.original, "fresh queue vtable hook");
    }

    // DX12 trace: hook CommandQueue::Signal (slot 14) to observe per-frame fence usage. The queue
    // vtable is shared by all queues from the device, so this also catches any co-resident module's
    // own queue. Only installed when tracing is enabled (Dx12TraceEnabled).
    if (Dx12TraceEnabled())
        DX12_HookQueueSignalVTable(queue);
}

void DX12_HookQueueSignalVTable(ID3D12CommandQueue* queue) {
    void** vtbl = queue ? *reinterpret_cast<void***>(queue) : nullptr;
    const auto capture = ce::dx12_queue_dispatch::CaptureSignalVTable(vtbl);
    if (capture.result == ce::dx12_queue_dispatch::CaptureResult::kCaptured) {
        TryPublishRealD3D12SignalCandidate(capture.original, "fresh queue vtable hook");
        HookLogImportant("DX12 TRACE: hooked CommandQueue::Signal for queue %p (vtbl=%p)", (void*)queue, (void*)vtbl);
    } else if (capture.result == ce::dx12_queue_dispatch::CaptureResult::kFollower ||
               capture.result == ce::dx12_queue_dispatch::CaptureResult::kFailed) {
        static ce::log_meter::ChangeGate installation;
        const auto verdict = installation.Observe(ce::log_meter::FieldKey(vtbl, capture.result));
        if (verdict) {
            HookLogImportant("DX12 Signal: capture=%d queue=%p vtable=%p%s", static_cast<int>(capture.result), queue, vtbl,
                             ce::log_meter::SuppressedNote(verdict.suppressed).c_str());
        }
    }
}

void DX12_HookDeviceVTable(ID3D12Device* device) {
    if (!device)
        return;

    // Don't hook wrapped devices
    void* unwrapped = nullptr;
    if (SUCCEEDED(device->QueryInterface(IID_CWrapD3D12Device, &unwrapped))) {
        ((IUnknown*)unwrapped)->Release();
        return;  // Already wrapped, skip vtable hook
    }

    static std::recursive_mutex s_DeviceHookMutex;
    std::lock_guard<std::recursive_mutex> lock(s_DeviceHookMutex);

    void** vtbl = *reinterpret_cast<void***>(device);
    if (!vtbl)
        return;

    // Skip vtable hooking on sl_interposer / SL wrapper devices.
    // SL wrapper vtables may have fewer than the 23 entries required for
    // CreateSampler (slot 22). Reading or writing vtbl[22] past the end
    // of the wrapper's vtable corrupts adjacent memory, which causes a
    // deterministic RIP=0 crash when another COM object's vtable pointer
    // gets overwritten with the trampoline pool address.
    {
        HMODULE hMod = nullptr;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCSTR)vtbl, &hMod) &&
            hMod) {
            char modPath[MAX_PATH] = {};
            if (GetModuleFileNameA(hMod, modPath, MAX_PATH)) {
                const char* modName = strrchr(modPath, '\\');
                modName = modName ? modName + 1 : modPath;
                if (strstr(modName, "sl_interposer") || strstr(modName, "sl.common")) {
                    HookLog("DX12: Skipping CreateSampler vtable hook for SL wrapper device %p (vtbl in %s)", device,
                            modName);
                    return;
                }
            }
        }
    }

    // Install the sampler/root-signature pair together. The dedicated subsystem
    // validates both slots, retains originals per vtable, and covers precompiled
    // root-signature blobs without expanding this already-large overlay module.
    ce::dx12_sampler_hooks::HookDevice(device);

    // DX12 trace: hook device creation calls to inspect queue/resource architecture (own command
    // queue? what resources/heaps?). CreateCommandQueue=8, CreateDescriptorHeap=14,
    // CreateCommittedResource=27. Only installed when tracing is enabled (Dx12TraceEnabled).
    if (Dx12TraceEnabled()) {
        if (vtbl[8] && vtbl[8] != (void*)DetourTraceCreateCommandQueue) {
            CreateCommandQueuePtr o = nullptr;
            if (VTableHook::Create(reinterpret_cast<void*>(&vtbl[8]), (LPVOID)DetourTraceCreateCommandQueue, (LPVOID*)&o) ==
                    VTableHook::Success &&
                o && !oTraceCreateCommandQueue) {
                oTraceCreateCommandQueue = o;
            }
            HookLogImportant("DX12 TRACE: hooked CreateCommandQueue for device %p", (void*)device);
        }
        if (vtbl[14] && vtbl[14] != (void*)DetourTraceCreateDescriptorHeap) {
            CreateDescriptorHeapPtr o = nullptr;
            if (VTableHook::Create(reinterpret_cast<void*>(&vtbl[14]), (LPVOID)DetourTraceCreateDescriptorHeap, (LPVOID*)&o) ==
                    VTableHook::Success &&
                o && !oTraceCreateDescriptorHeap) {
                oTraceCreateDescriptorHeap = o;
            }
            HookLogImportant("DX12 TRACE: hooked CreateDescriptorHeap for device %p", (void*)device);
        }
        if (vtbl[27] && vtbl[27] != (void*)DetourCreateCommittedResource) {
            CreateCommittedResourcePtr o = nullptr;
            if (VTableHook::Create(reinterpret_cast<void*>(&vtbl[27]), (LPVOID)DetourCreateCommittedResource, (LPVOID*)&o) ==
                    VTableHook::Success &&
                o && !oCreateCommittedResource) {
                oCreateCommittedResource = o;
            }
            HookLogImportant("DX12 TRACE: hooked CreateCommittedResource for device %p", (void*)device);
        }
    }
}

HRESULT STDMETHODCALLTYPE DetourCreateCommittedResource(ID3D12Device* device,
                                                        const D3D12_HEAP_PROPERTIES* pHeapProperties,
                                                        D3D12_HEAP_FLAGS HeapFlags, const D3D12_RESOURCE_DESC* pDesc,
                                                        D3D12_RESOURCE_STATES InitialResourceState,
                                                        const D3D12_CLEAR_VALUE* pOptimizedClearValue,
                                                        REFIID riidResource, void** ppvResource) {
    HRESULT hr = oCreateCommittedResource
                     ? oCreateCommittedResource(device, pHeapProperties, HeapFlags, pDesc, InitialResourceState,
                                                pOptimizedClearValue, riidResource, ppvResource)
                     : E_FAIL;
    if (Dx12TraceEnabled()) {
        static std::atomic<int> s_n{0};
        const int sn = s_n.fetch_add(1, std::memory_order_relaxed);
        if (sn < 300 || (sn % 200) == 0) {
            char d[256];
            _snprintf_s(d, sizeof(d), _TRUNCATE,
                        "heapType=%d dim=%d w=%llu h=%u fmt=%d resFlags=0x%X state=0x%X -> res=%p hr=0x%08X seq=%d",
                        pHeapProperties ? (int)pHeapProperties->Type : -1, pDesc ? (int)pDesc->Dimension : -1,
                        pDesc ? (unsigned long long)pDesc->Width : 0ull, pDesc ? pDesc->Height : 0,
                        pDesc ? (int)pDesc->Format : -1, pDesc ? (unsigned)pDesc->Flags : 0u,
                        (unsigned)InitialResourceState, (SUCCEEDED(hr) && ppvResource) ? *ppvResource : nullptr,
                        (unsigned)hr, sn);
            Dx12TraceLog("CreateCommittedResource", d);
        }
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE DetourTraceCreateCommandQueue(ID3D12Device* device, const D3D12_COMMAND_QUEUE_DESC* pDesc,
                                                        REFIID riid, void** ppQueue) {
    HRESULT hr = oTraceCreateCommandQueue ? oTraceCreateCommandQueue(device, pDesc, riid, ppQueue) : E_FAIL;
    if (Dx12TraceEnabled()) {
        static std::atomic<int> s_n{0};
        const int sn = s_n.fetch_add(1, std::memory_order_relaxed);
        if (sn < 200) {
            char d[256];
            _snprintf_s(d, sizeof(d), _TRUNCATE,
                        "type=%d prio=%d flags=0x%X node=%u dev=%p -> queue=%p hr=0x%08X seq=%d",
                        pDesc ? (int)pDesc->Type : -1, pDesc ? (int)pDesc->Priority : 0,
                        pDesc ? (unsigned)pDesc->Flags : 0u, pDesc ? pDesc->NodeMask : 0u, (void*)device,
                        (SUCCEEDED(hr) && ppQueue) ? *ppQueue : nullptr, (unsigned)hr, sn);
            Dx12TraceLog("CreateCommandQueue", d);
        }
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE DetourTraceCreateDescriptorHeap(ID3D12Device* device, const D3D12_DESCRIPTOR_HEAP_DESC* pDesc,
                                                          REFIID riid, void** ppHeap) {
    HRESULT hr = oTraceCreateDescriptorHeap ? oTraceCreateDescriptorHeap(device, pDesc, riid, ppHeap) : E_FAIL;
    if (Dx12TraceEnabled()) {
        static std::atomic<int> s_n{0};
        const int sn = s_n.fetch_add(1, std::memory_order_relaxed);
        if (sn < 200) {
            char d[256];
            _snprintf_s(d, sizeof(d), _TRUNCATE, "type=%d num=%u flags=0x%X node=%u -> heap=%p hr=0x%08X seq=%d",
                        pDesc ? (int)pDesc->Type : -1, pDesc ? pDesc->NumDescriptors : 0u,
                        pDesc ? (unsigned)pDesc->Flags : 0u, pDesc ? pDesc->NodeMask : 0u,
                        (SUCCEEDED(hr) && ppHeap) ? *ppHeap : nullptr, (unsigned)hr, sn);
            Dx12TraceLog("CreateDescriptorHeap", d);
        }
    }
    return hr;
}

SignalPtr GetOriginalCommandQueueSignal(ID3D12CommandQueue* queue) {
    return ce::dx12_queue_dispatch::ResolveSignal(queue);
}

HRESULT STDMETHODCALLTYPE DetourTraceCommandQueueSignal(ID3D12CommandQueue* queue, ID3D12Fence* fence, UINT64 value) {
    const SignalPtr original = GetOriginalCommandQueueSignal(queue);
    HRESULT hr = original ? original(queue, fence, value) : E_FAIL;
    if (Dx12TraceEnabled()) {
        static std::atomic<int> s_n{0};
        const int sn = s_n.fetch_add(1, std::memory_order_relaxed);
        if (sn < 80 || (sn % 300) == 0) {
            char d[160];
            _snprintf_s(d, sizeof(d), _TRUNCATE, "queue=%p fence=%p value=%llu hr=0x%08X seq=%d", (void*)queue,
                        (void*)fence, (unsigned long long)value, (unsigned)hr, sn);
            Dx12TraceLog("Signal", d);
        }
    }
    return hr;
}
