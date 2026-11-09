#include "gamma_external_submission.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <vector>
#include <wrl/client.h>

#include "common/logging/log_meter.h"
#include "hook/capture/inline_capture_completion.h"
#include "hook/runtime/hook_common.h"
#include "sharpen_d3d12.h"
#include "sharpen_gpu_timeline.h"

namespace ce::sharpen {
namespace {
Microsoft::WRL::ComPtr<IUnknown> QueryComIdentity(IUnknown* object) {
    Microsoft::WRL::ComPtr<IUnknown> identity;
    if (object) {
        object->QueryInterface(IID_PPV_ARGS(&identity));
    }
    return identity;
}

struct Slot {
    std::unique_ptr<D3D12Pass> pass;
    Microsoft::WRL::ComPtr<ID3D12Fence> fence;
    Microsoft::WRL::ComPtr<IUnknown> completionDeviceIdentity;
    ID3D12Device* completionDevice = nullptr;
    Microsoft::WRL::ComPtr<ID3D12CommandList> pendingList;
    std::unique_ptr<InlineCaptureCompletion<1>> inlineMarker;
    uint64_t value = 0;
    uint64_t inlineValue = 0;
    bool unprovable = false;
    bool leased = false;
    bool ownsSubmission = false;
    bool inlineCompletion = false;

    bool Retired() const {
        if (leased || unprovable)
            return false;
        if (ownsSubmission)
            return !pass || pass->SubmissionsRetired();
        if (inlineCompletion)
            return inlineMarker && inlineMarker->Reached(0, inlineValue);
        return !pendingList && (!fence || CompletedPast(fence->GetCompletedValue(), value));
    }
};

struct Pool {
    std::mutex mutex;
    std::vector<std::unique_ptr<Slot>> slots;
    ce::log_meter::ChangeGate growthGate;
};

std::atomic<Pool*> g_createdPool{nullptr};

Pool* CreatePool() noexcept {
    try {
        auto* pool = new Pool;
        g_createdPool.store(pool, std::memory_order_release);
        return pool;
    } catch (...) {
        HookLogImportant("PostProcess: runtime resource pool allocation failed");
        return nullptr;
    }
}

// Borrowed lists can be submitted after injector shutdown. Keep recordings
// until the GPU completes them or the list's owner explicitly abandons them.
Pool* GetPool() {
    static Pool* pool = CreatePool();
    return pool;
}
std::atomic<uint32_t> g_pending{0};

// Move out the list under the registry lock, then release it outside the lock.
Microsoft::WRL::ComPtr<ID3D12CommandList> TakePendingList(Slot& slot) {
    if (slot.pendingList)
        g_pending.fetch_sub(1, std::memory_order_release);
    return std::move(slot.pendingList);
}

struct Lease {
    Pool& pool;
    Slot& slot;
    ~Lease() {
        std::lock_guard<std::mutex> lock(pool.mutex);
        slot.leased = false;
    }
};
}

bool RenderPresentedPostProcess(ID3D12Device* device, ID3D12CommandQueue* queue, ID3D12Resource* output,
                                 DXGI_FORMAT viewFormat, const Request& request, Route route, TargetEncoding encoding) {
    if (!device || !queue || !output || !Requested(request))
        return false;
    Pool* pool = GetPool();
    if (!pool)
        return false;
    Slot* selected = nullptr;
    Microsoft::WRL::ComPtr<ID3D12CommandList> retiredList;
    try {
        std::lock_guard<std::mutex> lock(pool->mutex);
        for (auto& item : pool->slots) {
            if ((!item->leased && item->ownsSubmission) || item->Retired()) {
                selected = item.get();
                break;
            }
        }
        if (!selected) {
            pool->slots.push_back(std::make_unique<Slot>());
            selected = pool->slots.back().get();
        }
        retiredList = TakePendingList(*selected);
        selected->leased = true;
        selected->ownsSubmission = true;
    } catch (...) {
        return false;
    }
    Lease lease{*pool, *selected};
    try {
        if (!selected->pass)
            selected->pass = std::make_unique<D3D12Pass>();
        return selected->pass->Render(device, queue, output, viewFormat, D3D12_RESOURCE_STATE_PRESENT,
                                      request, route, encoding);
    } catch (...) {
        return false;
    }
}

bool RecordRuntimePostProcess(ID3D12Device* device, ID3D12GraphicsCommandList* list, ID3D12Resource* output,
                              D3D12_RESOURCE_STATES state, const Request& request, TargetEncoding encoding,
                              bool inlineCompletion) {
    if (!device || !list || !output || !Requested(request))
        return false;
    Pool* pool = GetPool();
    if (!pool)
        return false;
    Slot* selected = nullptr;
    Microsoft::WRL::ComPtr<ID3D12CommandList> retiredList;
    try {
        std::lock_guard<std::mutex> lock(pool->mutex);
        for (auto& item : pool->slots) {
            // Appending to a borrowed list does not abandon its earlier commands.
            // Each recording owns its resources until its completion point.
            if (item->Retired()) {
                selected = item.get();
                break;
            }
        }
        if (!selected) {
            pool->slots.push_back(std::make_unique<Slot>());
            selected = pool->slots.back().get();
            const auto verdict = pool->growthGate.ObserveOrEvery(ce::log_meter::FieldKey("runtime-command-backlog"),
                                                                static_cast<uint32_t>(pool->slots.size()), 64);
            if (verdict)
                HookLogImportant("PostProcess: runtime command pool grew to %zu slots (completion=%s)%s",
                                 pool->slots.size(), inlineCompletion ? "GPU marker" : "submission fence",
                                 ce::log_meter::SuppressedNote(verdict.suppressed).c_str());
        }
        retiredList = TakePendingList(*selected);
        selected->inlineCompletion = false;
        selected->leased = true;
        selected->ownsSubmission = false;
    } catch (...) {
        return false;
    }
    auto& slot = *selected;
    Lease lease{*pool, slot};
    // Driver calls run outside the registry lock. Queue/PSO wrappers can reenter
    // ECL observation; they must never encounter this lock held by their caller.
    try {
        if (!slot.pass)
            slot.pass = std::make_unique<D3D12Pass>();
        if (slot.completionDevice != device) {
            auto deviceIdentity = QueryComIdentity(device);
            if (!deviceIdentity || deviceIdentity.Get() != slot.completionDeviceIdentity.Get()) {
                slot.fence.Reset();
                slot.inlineMarker.reset();
                slot.inlineValue = 0;
                slot.value = 0;
                slot.completionDeviceIdentity = std::move(deviceIdentity);
            }
            slot.completionDevice = slot.completionDeviceIdentity ? device : nullptr;
        }
        if (inlineCompletion) {
            Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList2> list2;
            if (FAILED(list->QueryInterface(IID_PPV_ARGS(&list2))))
                return false;
            if (!slot.inlineMarker) {
                auto marker = std::make_unique<InlineCaptureCompletion<1>>();
                if (!marker->Initialize(device)) {
                    HookLogImportant("PostProcess: inline completion marker allocation failed");
                    return false;
                }
                slot.inlineMarker = std::move(marker);
            }
        } else if (!slot.fence &&
                   FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&slot.fence)))) {
            return false;
        }
        if (!slot.pass->RecordExternal(device, list, output, state, request, encoding))
            return false;
        slot.inlineCompletion = inlineCompletion;
        if (inlineCompletion) {
            if (static_cast<uint32_t>(++slot.inlineValue) == 0)
                ++slot.inlineValue;
            // MARKER_OUT follows the commands using this slot's resources. The
            // marker itself proves completion, even through an unhooked submission.
            slot.unprovable = !slot.inlineMarker->Record(list, 0, slot.inlineValue);
            if (slot.unprovable)
                HookLogImportant("PostProcess: inline completion recording failed; resources retained");
        }
    } catch (...) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(pool->mutex);
        slot.pendingList = list;
        g_pending.fetch_add(1, std::memory_order_release);
    }
    return true;
}

void AbandonRuntimePostProcessCommandList(ID3D12CommandList* list) {
    if (!list || g_pending.load(std::memory_order_acquire) == 0)
        return;
    Pool* pool = g_createdPool.load(std::memory_order_acquire);
    if (!pool)
        return;
    std::lock_guard<std::mutex> lock(pool->mutex);
    for (auto& item : pool->slots) {
        if (!item->leased && item->pendingList.Get() == list) {
            item->pendingList.Reset();
            item->inlineCompletion = false;
            item->unprovable = false;
            g_pending.fetch_sub(1, std::memory_order_release);
        }
    }
}

void NotifyRuntimePostProcessSubmitted(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists) {
    if (!queue || !lists || g_pending.load(std::memory_order_acquire) == 0)
        return;
    Pool* pool = GetPool();
    if (!pool)
        return;
    for (;;) {
        Slot* selected = nullptr;
        {
            std::lock_guard<std::mutex> lock(pool->mutex);
            for (auto& item : pool->slots) {
                auto& slot = *item;
                if (slot.leased || !slot.pendingList)
                    continue;
                for (UINT index = 0; index < count; ++index) {
                    if (lists[index] == slot.pendingList.Get()) {
                        slot.leased = true;
                        if (!slot.inlineCompletion)
                            ++slot.value;
                        slot.pendingList.Reset();
                        g_pending.fetch_sub(1, std::memory_order_release);
                        selected = &slot;
                        break;
                    }
                }
                if (selected)
                    break;
            }
        }
        if (!selected)
            return;
        auto& slot = *selected;
        Lease lease{*pool, slot};
        if (slot.inlineCompletion)
            continue;
        // Signal AFTER the observed ECL, on its actual submitting queue. Each
        // slot has its own fence, so unrelated queues cannot retire one another.
        slot.unprovable = FAILED(queue->Signal(slot.fence.Get(), slot.value));
        if (slot.unprovable)
            HookLogImportant("PostProcess: runtime command completion signal failed; resources retained");
    }
}

size_t CollectRuntimePostProcess(bool trim) {
    if (!trim)
        return 0;
    Pool* pool = g_createdPool.load(std::memory_order_acquire);
    if (!pool)
        return 0;
    size_t collected = 0;
    for (;;) {
        Slot* selected = nullptr;
        Microsoft::WRL::ComPtr<ID3D12CommandList> retiredList;
        {
            std::lock_guard<std::mutex> lock(pool->mutex);
            for (auto& item : pool->slots) {
                if (item->Retired() && item->pass) {
                    selected = item.get();
                    retiredList = TakePendingList(*selected);
                    selected->leased = true;
                    break;
                }
            }
        }
        if (!selected)
            return collected;
        auto& slot = *selected;
        Lease lease{*pool, slot};
        slot.pass.reset();
        slot.fence.Reset();
        slot.completionDeviceIdentity.Reset();
        slot.completionDevice = nullptr;
        slot.inlineMarker.reset();
        slot.inlineCompletion = false;
        slot.value = 0;
        ++collected;
    }
}

}  // namespace ce::sharpen
