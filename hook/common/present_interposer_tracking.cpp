#include "dxgi_shared_internal.h"

#include <unordered_map>

// A present interposer (NVIDIA Smooth Motion's NvPresent64) creates a private real DXGI swapchain,
// on its own command queue, for the frames it actually puts on screen, and hands the application a
// proxy. CE composites on that output chain — it is below every overlay that patches the dxgi entry
// (so CE draws last and is topmost) and after the driver has generated the frame (so CE's overlay is
// not interpolated). What CE must never do is submit that overlay on the application's queue:
// cross-queue backbuffer access removes the device with DXGI_ERROR_ACCESS_DENIED (0x887A002B), the
// same failure ffx_routing.h documents for the DLSS-G render queue. This unit is therefore the
// swapchain -> owning-queue association for those chains.
//
// It also measures the two present streams against each other, which is the only Smooth Motion
// evidence available in DX12 (present_interposer_cadence.h).
namespace DXGIShared {
namespace {
struct InterposerOutputChain {
    ID3D12CommandQueue* queue = nullptr;
};

std::mutex& InterposerMutex() {
    static std::mutex mutex;
    return mutex;
}

std::unordered_map<IDXGISwapChain*, InterposerOutputChain>& InterposerChains() {
    static std::unordered_map<IDXGISwapChain*, InterposerOutputChain> chains;
    return chains;
}

// Lock-free gates so the present path pays two atomic loads when no interposer is in the chain.
std::atomic<size_t> s_interposerChainCount{0};
std::atomic<size_t> s_compositableInterposerChainCount{0};

void PublishInterposerCounts() {
    size_t total = 0;
    size_t compositable = 0;
    for (const auto& entry : InterposerChains()) {
        ++total;
        if (entry.second.queue) {
            ++compositable;
        }
    }
    s_interposerChainCount.store(total, std::memory_order_release);
    s_compositableInterposerChainCount.store(compositable, std::memory_order_release);
}

ce::present_interposer::CadenceTracker& InterposerCadence() {
    static ce::present_interposer::CadenceTracker tracker;
    return tracker;
}

// The interposer presents its private chain on its own thread while the
// application presents the proxy on the render thread, so this classification
// is per-Present and per-thread, never a process-wide latch.
thread_local bool t_presentInterposerPrivateOutputChainScope = false;
// The per-present source verdict, resolved once at the top of the present path.
// Both the application frame-rate metric and the DX11 overlay path read it, and
// resolving it twice would consume the submission counter twice.
thread_local bool t_presentInterposerSourceClassified = false;
thread_local bool t_presentInterposerApplicationSourced = false;
}  // namespace

void SetPresentInterposerPrivateOutputChainScope(bool active) {
    t_presentInterposerPrivateOutputChainScope = active;
    t_presentInterposerSourceClassified = false;
    t_presentInterposerApplicationSourced = false;
}

bool IsPresentOnPresentInterposerPrivateOutputChain() {
    return t_presentInterposerPrivateOutputChainScope;
}

// Resolve this present's stream before anything measures a frame rate from it.
//
// The application frame-rate metric is advanced once per Present, and on an
// interposer's private output chain that is once per OUTPUT frame, not once per
// game frame - the overlay then reads the interposer's submission rate as the
// game's. DX12 does not have this problem: its metric is fed by the app-facing
// swapchain wrapper, which is 1x by construction. Witcher 3 session
// 20260919_180154, driver vsync forced to 144: `application=144.0 fps
// output=288.0 fps`, and the overlay showed 288.
void ClassifyPresentInterposerPresentSource() {
    if (!t_presentInterposerPrivateOutputChainScope || t_presentInterposerSourceClassified) {
        return;
    }
    t_presentInterposerApplicationSourced = g_FGCompat.RecordPresentForNvidiaSmoothMotion();
    t_presentInterposerSourceClassified = true;
    if (t_presentInterposerApplicationSourced) {
        NoteApplicationPresentUnderPresentInterposer();
    }
}

bool HasPresentInterposerPresentSourceClassification() {
    return t_presentInterposerSourceClassified;
}

bool IsPresentInterposerPresentApplicationSourced() {
    return t_presentInterposerApplicationSourced;
}

void DX12_RegisterPresentInterposerPrivateSwapchain(IDXGISwapChain* pSwapChain, ID3D12CommandQueue* pOutputQueue) {
    if (!pSwapChain) {
        return;
    }
    std::lock_guard<std::mutex> lock(InterposerMutex());
    InterposerChains()[pSwapChain].queue = pOutputQueue;
    PublishInterposerCounts();
    // From here the DX11/DX10 present path needs to tell an application frame
    // from an interposer-generated one, and the application's own submissions
    // are the evidence. Nothing counts them until an interposer actually exists.
    g_FGCompat.SetApplicationSubmissionCountingEnabled(true);
}

void DX12_UnregisterPresentInterposerPrivateSwapchain(IDXGISwapChain* pSwapChain) {
    if (!pSwapChain) {
        return;
    }
    std::lock_guard<std::mutex> lock(InterposerMutex());
    InterposerChains().erase(pSwapChain);
    PublishInterposerCounts();
}

bool HasPresentInterposerPrivateSwapchains() {
    return s_interposerChainCount.load(std::memory_order_acquire) != 0;
}

bool HasCompositablePresentInterposerOutputChain() {
    return s_compositableInterposerChainCount.load(std::memory_order_acquire) != 0;
}

bool DX12_IsPresentInterposerPrivateSwapchain(IDXGISwapChain* pSwapChain) {
    if (!pSwapChain || !HasPresentInterposerPrivateSwapchains()) {
        return false;
    }
    std::lock_guard<std::mutex> lock(InterposerMutex());
    return InterposerChains().find(pSwapChain) != InterposerChains().end();
}

ID3D12CommandQueue* DX12_GetPresentInterposerOutputQueue(IDXGISwapChain* pSwapChain) {
    if (!pSwapChain || !HasCompositablePresentInterposerOutputChain()) {
        return nullptr;
    }
    std::lock_guard<std::mutex> lock(InterposerMutex());
    const auto it = InterposerChains().find(pSwapChain);
    return it == InterposerChains().end() ? nullptr : it->second.queue;
}

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
