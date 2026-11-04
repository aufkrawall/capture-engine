#pragma once

#include <cstddef>

namespace ce::present_probe {

// Which swapchain object and method table this thread has already proved readable, so a Present does not ask the
// kernel again for the pointer it presents through every frame (DXGIShared::PresentSwapchainProbesOk). The object
// and the table have to match: a new chain at a dead one's address normally brings its own table, and a table seen
// for the first time is probed. Either way the detour has called through this object before it gets here.
class SwapchainProbeMemo {
public:
    bool HoldsObject(const void* swapchain) const { return swapchain != nullptr && swapchain == swapchain_; }
    bool Holds(const void* swapchain, const void* table) const {
        return HoldsObject(swapchain) && table != nullptr && table == table_;
    }
    void Remember(const void* swapchain, const void* table) {
        swapchain_ = swapchain;
        table_ = table;
    }

private:
    const void* swapchain_ = nullptr;
    const void* table_ = nullptr;
};

// The method table of `swapchain`, or null if it or its first `tableBytes` are not readable. `isReadable(pointer, size)`
// asks the kernel, so it runs only for what `memo` does not hold yet. Its cost depends on the memory region around the
// pointer: a Present route that asked every frame cost 131 us in one run and 9 us in another (20261104_210842,
// 20261104_154909; the cause is inferred).
template <typename IsReadable>
void** ProbeSwapchainTable(const void* swapchain, size_t tableBytes, SwapchainProbeMemo& memo, IsReadable&& isReadable) {
    if (!swapchain || (!memo.HoldsObject(swapchain) && !isReadable(swapchain, sizeof(void*)))) {
        return nullptr;
    }
    void** const table = *static_cast<void** const*>(swapchain);
    if (!memo.Holds(swapchain, table)) {
        if (!table || !isReadable(static_cast<const void*>(table), tableBytes)) {
            return nullptr;
        }
        memo.Remember(swapchain, table);
    }
    return table;
}

}  // namespace ce::present_probe
