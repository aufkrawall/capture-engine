#pragma once

#include <array>
#include <atomic>
#include <cstddef>

// Whether a DXGI Present on the normal route is one of Streamline's final outputs that the FPS limiter must pace
// as a final-output boundary (output-group admission), and the per-swapchain record that keeps it from doing so
// where the application's own Present is paced too. Policy for DXGIShared::IsStreamlineFinalOutputForLimiter.
namespace ce::streamline_output_site {

// A Present Streamline issued on the real swapchain while DLSS-G runs is one of its final outputs, generated or
// rendered: the game Presents into Streamline's proxy, never into the real swapchain. Talos 20261019_204226
// (pre-FSR DLSS 4x) and GTA V Enhanced both show exactly that topology: owner=0 on every Present, all of them
// from Streamline (the game thread's while DLSS-G is idle, presenter threads while it generates), and the perf
// CSV has exactly four rows per 4x frame. Paced as application Presents they had no output-group admission: four
// evaluations per game frame on the base-rate grid, which only the 2 ms duplicate window thinned out. (A Present
// that reaches the limiter always owns CE's Present path: nested and cross-thread re-entries return before it.)
//
// It is the boundary only where that stream is the only one paced on this swapchain. If the application's own
// Present reaches CE's limiter there too (CE above Streamline's Present hook, or CE's swapchain wrapper), its
// outputs may be partial groups and a second pacer would admit them: that swapchain keeps today's site.
inline bool IsFinalOutputBoundary(bool isD3D12SwapChain, bool streamlineFGRunning, bool callerFromStreamlineModule,
                                  bool applicationPresentPacedOnSwapchain) {
    return isD3D12SwapChain && streamlineFGRunning && callerFromStreamlineModule && !applicationPresentPacedOnSwapchain;
}

// The application's own Present: not issued by Streamline, not an FSR FG output (FFX presenter or a
// callback-proven output). Streamline's own Presents and FSR outputs never count.
inline bool IsApplicationPresent(bool callerFromStreamlineModule, bool callerFromFFXFrameGenerationModule,
                                 bool callbackProvenRuntimeOutput) {
    return !callerFromStreamlineModule && !callerFromFFXFrameGenerationModule && !callbackProvenRuntimeOutput;
}

// Swapchains on which a limiter site paced the application's own Present. Lock-free, bounded, insert-only: a
// released swapchain keeps its entry and a new one at the same address inherits it, and a full table answers
// "paced" for every swapchain. Both fail toward the application-present site, never toward a second pacer.
class ApplicationPacedSwapchains {
public:
    static constexpr size_t kCapacity = 16;

    // True when this call recorded `swapchain` for the first time.
    bool Note(const void* swapchain) {
        if (!swapchain || Contains(swapchain)) {
            return false;
        }
        for (std::atomic<const void*>& slot : slots_) {
            const void* expected = nullptr;
            if (slot.compare_exchange_strong(expected, swapchain, std::memory_order_acq_rel,
                                             std::memory_order_acquire)) {
                return true;
            }
            if (expected == swapchain) {
                return false;  // a concurrent Note of the same swapchain won
            }
        }
        saturated_.store(true, std::memory_order_release);
        return true;
    }

    bool Contains(const void* swapchain) const {
        if (saturated_.load(std::memory_order_acquire)) {
            return true;
        }
        for (const std::atomic<const void*>& slot : slots_) {
            const void* stored = slot.load(std::memory_order_acquire);
            if (stored == swapchain) {
                return swapchain != nullptr;
            }
            if (stored == nullptr) {
                return false;  // slots fill in order and are never cleared
            }
        }
        return false;
    }

private:
    std::array<std::atomic<const void*>, kCapacity> slots_{};
    std::atomic<bool> saturated_{false};
};

}  // namespace ce::streamline_output_site
