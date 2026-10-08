#include "gamma_external_submission.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <vector>
#include <wrl/client.h>

#include "common/logging/log_meter.h"
#include "hook/runtime/hook_common.h"
#include "sharpen_d3d12.h"
#include "sharpen_gpu_timeline.h"

namespace ce::sharpen {
namespace {
struct Slot {
    std::unique_ptr<D3D12Pass> pass;
    Microsoft::WRL::ComPtr<ID3D12Fence> fence;
    Microsoft::WRL::ComPtr<ID3D12CommandList> pendingList;
    uint64_t value = 0;
    bool unprovable = false;
    bool leased = false;
    bool ownsSubmission = false;

    bool Retired() const {
        return !leased && !pendingList && !unprovable &&
               (ownsSubmission ? (!pass || pass->SubmissionsRetired())
                               : (!fence || CompletedPast(fence->GetCompletedValue(), value)));
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

// Borrowed lists may be recorded but never submitted. They cannot be reclaimed
// on injector shutdown: their runtime can still submit them. Retain unproven
// slots for process lifetime; proven completed slots are explicitly trimmed.
Pool* GetPool() {
    static Pool* pool = CreatePool();
    return pool;
}
std::atomic<uint32_t> g_pending{0};

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
                              D3D12_RESOURCE_STATES state, const Request& request, TargetEncoding encoding) {
    if (!device || !list || !output || !Requested(request))
        return false;
    Pool* pool = GetPool();
    if (!pool)
        return false;
    Slot* selected = nullptr;
    try {
        std::lock_guard<std::mutex> lock(pool->mutex);
        for (auto& item : pool->slots) {
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
                HookLogImportant("PostProcess: runtime command pool grew to %zu slots%s", pool->slots.size(),
                                 ce::log_meter::SuppressedNote(verdict.suppressed).c_str());
        }
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
        Microsoft::WRL::ComPtr<ID3D12Device> fenceDevice;
        if (slot.fence)
            slot.fence->GetDevice(IID_PPV_ARGS(&fenceDevice));
        if (fenceDevice.Get() != device) {
            slot.fence.Reset();
            slot.value = 0;
            if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&slot.fence))))
                return false;
        }
        if (!slot.pass->RecordExternal(device, list, output, state, request, encoding))
            return false;
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
        // Signal AFTER the observed ECL, on its actual submitting queue. Each
        // slot has its own fence, so unrelated queues cannot retire one another.
        slot.unprovable = FAILED(queue->Signal(slot.fence.Get(), slot.value));
        if (slot.unprovable)
            HookLogImportant("PostProcess: runtime command completion signal failed; resources retained");
    }
}

void CollectRuntimePostProcess(bool trim) {
    if (!trim)
        return;
    Pool* pool = g_createdPool.load(std::memory_order_acquire);
    if (!pool)
        return;
    for (;;) {
        Slot* selected = nullptr;
        {
            std::lock_guard<std::mutex> lock(pool->mutex);
            for (auto& item : pool->slots) {
                if (item->Retired() && item->pass) {
                    selected = item.get();
                    selected->leased = true;
                    break;
                }
            }
        }
        if (!selected)
            return;
        auto& slot = *selected;
        Lease lease{*pool, slot};
        slot.pass.reset();
        slot.fence.Reset();
        slot.value = 0;
    }
}

}  // namespace ce::sharpen
