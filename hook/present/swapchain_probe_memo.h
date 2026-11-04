#pragma once

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

}  // namespace ce::present_probe
