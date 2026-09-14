#include "dxgi_shared_internal.h"

// Measures the two present streams a present interposer creates (see present_interposer_cadence.h)
// and publishes the verdict as CE's Smooth Motion FG state. This is the only Smooth Motion evidence
// available in DX12: the application's present stream stays 1x, so the command-list population and
// present-gap heuristics in fg_detection never fire there.
namespace DXGIShared {
namespace {
ce::present_interposer::CadenceTracker& InterposerCadence() {
    static ce::present_interposer::CadenceTracker tracker;
    return tracker;
}
}  // namespace

void NotePresentInterposerOutputPresent(IDXGISwapChain* pSwapChain) {
    if (!HasPresentInterposerPrivateSwapchains() || !DX12_IsPresentInterposerPrivateSwapchain(pSwapChain)) {
        return;
    }
    InterposerCadence().NoteOutputPresent();
}

void NoteApplicationPresentUnderPresentInterposer() {
    if (!HasPresentInterposerPrivateSwapchains()) {
        return;
    }
    ce::present_interposer::CadenceVerdict verdict;
    if (!InterposerCadence().NoteApplicationPresent(PerfLogger::GetQpcUs(), &verdict)) {
        return;
    }
    g_FGCompat.NotePresentInterposerCadence(verdict);

    static std::atomic<int> s_windowLogCount{0};
    const int windowNum = s_windowLogCount.fetch_add(1, std::memory_order_relaxed) + 1;
    if (windowNum <= 5 || (windowNum % 60) == 0) {
        HookLogImportant(
            "DetourPresent: Present interposer output cadence window #%d — application=%.1f fps output=%.1f fps "
            "generating=%d multiplier=%d",
            windowNum, verdict.applicationFps, verdict.outputFps, verdict.generating ? 1 : 0, verdict.multiplier);
    }
}

void ResetPresentInterposerCadence() {
    InterposerCadence().Reset();
}
}  // namespace DXGIShared
