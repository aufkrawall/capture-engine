#include "dx12_device_trace.h"

#include "dx12_hook_internal.h"
#include "execute_dispatch_registry.h"
#include "common/logging/log_meter.h"

#include <utility>

namespace ce::dx12_device_trace {
namespace {

HRESULT STDMETHODCALLTYPE DetourTraceCreateCommandQueue(ID3D12Device*, const D3D12_COMMAND_QUEUE_DESC*, REFIID, void**);
HRESULT STDMETHODCALLTYPE DetourTraceCreateDescriptorHeap(ID3D12Device*, const D3D12_DESCRIPTOR_HEAP_DESC*, REFIID,
                                                          void**);
HRESULT STDMETHODCALLTYPE DetourCreateCommittedResource(ID3D12Device*, const D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS,
                                                        const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES,
                                                        const D3D12_CLEAR_VALUE*, REFIID, void**);

using CreateQueue = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_COMMAND_QUEUE_DESC*, REFIID, void**);
using CreateHeap = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_DESCRIPTOR_HEAP_DESC*, REFIID, void**);
using CreateResource = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS,
                                                   const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES,
                                                   const D3D12_CLEAR_VALUE*, REFIID, void**);

template <typename Target>
class TraceMethod {
public:
    TraceMethod(size_t slot, Target detour, const char* name) : slot_(slot), detour_(detour), name_(name) {}

    void Capture(ID3D12Device* device) {
        void** vtable = VTable(device);
        if (!vtable)
            return;
        const auto captured = registry_.Install(vtable, ReadSlot(vtable), detour_, [&](Target* original) {
            return VTableHook::Create(static_cast<void*>(&vtable[slot_]), reinterpret_cast<void*>(detour_),
                                      reinterpret_cast<void**>(original)) == VTableHook::Success;
        });
        using Result = typename ce::dx12::ExecuteDispatchRegistry<Target>::InstallResult;
        if (captured.result == Result::kCaptured) {
            HookLogImportant("DX12 TRACE: captured %s for device=%p vtable=%p original=%p", name_, device, vtable,
                             reinterpret_cast<void*>(captured.original));
        } else if (captured.result == Result::kFollower || captured.result == Result::kFailed) {
            const auto verdict = installation_.Observe(ce::log_meter::FieldKey(vtable, captured.result));
            if (verdict) {
                HookLogImportant("DX12 TRACE: capture=%d method=%s device=%p vtable=%p%s",
                                 static_cast<int>(captured.result), name_, device, vtable,
                                 ce::log_meter::SuppressedNote(verdict.suppressed).c_str());
            }
        }
    }

    template <typename... Args>
    HRESULT Invoke(ID3D12Device* device, Args&&... args) {
        void** vtable = VTable(device);
        Target target = nullptr;
        if (vtable) {
            target = registry_.ResolveInterception(
                vtable, detour_,
                [&] {
                    void* original = nullptr;
                    VTableHook::GetOriginal(static_cast<void*>(&vtable[slot_]), reinterpret_cast<void*>(detour_), &original);
                    return reinterpret_cast<Target>(original);
                },
                [&] { return ReadSlot(vtable); });
        }
        if (target)
            return target(device, std::forward<Args>(args)...);
        const auto verdict = missing_.Observe(ce::log_meter::FieldKey(device, vtable));
        if (verdict) {
            HookLogImportant("DX12 TRACE: no exact predecessor method=%s device=%p vtable=%p%s", name_, device, vtable,
                             ce::log_meter::SuppressedNote(verdict.suppressed).c_str());
        }
        return E_FAIL;
    }

    void Reset() {
        registry_.Reset();
    }

private:
    static void** VTable(ID3D12Device* device) {
        return device ? *reinterpret_cast<void***>(device) : nullptr;
    }
    Target ReadSlot(void** vtable) const {
        return reinterpret_cast<Target>(std::atomic_ref<void*>(vtable[slot_]).load(std::memory_order_acquire));
    }
    const size_t slot_;
    const Target detour_;
    const char* const name_;
    ce::dx12::ExecuteDispatchRegistry<Target> registry_;
    ce::log_meter::ChangeGate installation_;
    ce::log_meter::ChangeGate missing_;
};

// NOLINTNEXTLINE(bugprone-throwing-static-initialization) - process-lifetime synchronization must initialize before hooks are callable.
TraceMethod<CreateQueue> queue{8, &DetourTraceCreateCommandQueue, "CreateCommandQueue"};
// NOLINTNEXTLINE(bugprone-throwing-static-initialization) - process-lifetime synchronization must initialize before hooks are callable.
TraceMethod<CreateHeap> heap{14, &DetourTraceCreateDescriptorHeap, "CreateDescriptorHeap"};
// NOLINTNEXTLINE(bugprone-throwing-static-initialization) - process-lifetime synchronization must initialize before hooks are callable.
TraceMethod<CreateResource> resource{27, &DetourCreateCommittedResource, "CreateCommittedResource"};
HRESULT STDMETHODCALLTYPE DetourCreateCommittedResource(ID3D12Device* device,
                                                        const D3D12_HEAP_PROPERTIES* pHeapProperties,
                                                        D3D12_HEAP_FLAGS HeapFlags, const D3D12_RESOURCE_DESC* pDesc,
                                                        D3D12_RESOURCE_STATES InitialResourceState,
                                                        const D3D12_CLEAR_VALUE* pOptimizedClearValue,
                                                        REFIID riidResource, void** ppvResource) {
    HRESULT hr =
        ce::dx12_device_trace::CreateCommittedResource(device, pHeapProperties, HeapFlags, pDesc, InitialResourceState,
                                                       pOptimizedClearValue, riidResource, ppvResource);
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
    HRESULT hr = ce::dx12_device_trace::CreateCommandQueue(device, pDesc, riid, ppQueue);
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
    HRESULT hr = ce::dx12_device_trace::CreateDescriptorHeap(device, pDesc, riid, ppHeap);
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

}  // namespace

void HookDevice(ID3D12Device* device) {
    queue.Capture(device);
    heap.Capture(device);
    resource.Capture(device);
}

HRESULT CreateCommandQueue(ID3D12Device* device, const D3D12_COMMAND_QUEUE_DESC* desc, REFIID id, void** out) {
    return queue.Invoke(device, desc, id, out);
}
HRESULT CreateDescriptorHeap(ID3D12Device* device, const D3D12_DESCRIPTOR_HEAP_DESC* desc, REFIID id, void** out) {
    return heap.Invoke(device, desc, id, out);
}
HRESULT CreateCommittedResource(ID3D12Device* device, const D3D12_HEAP_PROPERTIES* properties, D3D12_HEAP_FLAGS flags,
                                const D3D12_RESOURCE_DESC* desc, D3D12_RESOURCE_STATES state,
                                const D3D12_CLEAR_VALUE* clearValue, REFIID id, void** out) {
    return resource.Invoke(device, properties, flags, desc, state, clearValue, id, out);
}
void Reset() {
    queue.Reset();
    heap.Reset();
    resource.Reset();
}

}  // namespace ce::dx12_device_trace
