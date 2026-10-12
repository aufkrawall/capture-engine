#pragma once

#include <windows.h>
#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

namespace ce::dx12 {
// Adopts existing references. GPU completion, rather than a wait timeout,
// decides when an old resource generation can be destroyed.
template<class Resource, class Fence>
class OverlayResourceRetirement {
    struct Work { Fence* fence; uint64_t value; bool unprovable; };
    struct Entry { std::vector<Resource*> resources; std::vector<HANDLE> events; std::vector<Work> work; };
public:
    OverlayResourceRetirement() = default;
    // Pending COM references deliberately survive global loader-lock destruction.
    // Normal cleanup releases each generation through Collect after GPU proof.
    OverlayResourceRetirement(const OverlayResourceRetirement&) = delete;
    OverlayResourceRetirement& operator=(const OverlayResourceRetirement&) = delete;
    void RememberViewWork(Fence* fence, uint64_t value, bool unprovable) {
        if (!Complete({fence, value, unprovable})) AddWork(views_, fence, value, unprovable);
    }
    void Retire(std::vector<Resource*> resources, std::vector<HANDLE> events, Fence* fence,
                uint64_t value, bool unprovable, bool includesViews,
                Fence* completionFence = nullptr, uint64_t completionValue = 0) {
        Entry entry{std::move(resources), std::move(events), {}};
        if (includesViews) entry.work = std::move(views_);
        AddWork(entry.work, fence, value, unprovable);
        if (completionValue) AddWork(entry.work, completionFence, completionValue, unprovable);
        if (Complete(entry)) Release(entry);
        else entries_.push_back(std::move(entry));
    }
    void Collect() {
        for (auto it = entries_.begin(); it != entries_.end();) {
            if (Complete(*it)) { Release(*it); it = entries_.erase(it); }
            else ++it;
        }
        for (auto it = views_.begin(); it != views_.end();) {
            if (Complete(*it)) { if (it->fence) it->fence->Release(); it = views_.erase(it); }
            else ++it;
        }
    }
    bool Pending() const { return !entries_.empty() || !views_.empty(); }
private:
    static bool Complete(const Work& work) {
        if (!work.fence) return !work.unprovable;
        const uint64_t completed = work.fence->GetCompletedValue();
        return completed == UINT64_MAX || (!work.unprovable && completed >= work.value);
    }
    static bool Complete(const Entry& entry) {
        return std::all_of(entry.work.begin(), entry.work.end(), [](const auto& work) { return Complete(work); });
    }
    static void AddWork(std::vector<Work>& work, Fence* fence, uint64_t value, bool unprovable) {
        if (!fence && !unprovable) return;
        for (auto& prior : work) {
            if (prior.fence == fence) {
                prior.value = std::max(prior.value, value);
                prior.unprovable |= unprovable;
                return;
            }
        }
        work.push_back({fence, value, unprovable});
        if (fence) fence->AddRef();
    }
    static void Release(Entry& entry) {
        for (auto* resource : entry.resources) if (resource) resource->Release();
        for (HANDLE event : entry.events) if (event) CloseHandle(event);
        for (const auto& work : entry.work) if (work.fence) work.fence->Release();
    }
    std::vector<Entry> entries_;
    std::vector<Work> views_;
};
}  // namespace ce::dx12
