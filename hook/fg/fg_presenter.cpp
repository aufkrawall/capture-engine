#include "fg_presenter.h"

#include <atomic>
#include <mutex>
#include <unordered_map>

#include "common/logging/log_meter.h"
#include "hook/runtime/hook_common.h"

namespace ce::fg_presenter {
namespace {

struct Record {
    Presenter presenter = Presenter::kApplication;
    HWND window = nullptr;
    ID3D12CommandQueue* queue = nullptr;  // identity only; the presenter owns the queue
    uintptr_t owner = 0;
    uint64_t lifetime = 0;
};

struct OpenScope {
    Presenter presenter = Presenter::kApplication;
    HWND window = nullptr;
    uintptr_t owner = 0;
    IDXGISwapChain* created = nullptr;
};

struct State {
    std::mutex mutex;
    std::unordered_map<IDXGISwapChain*, Record> swapchains;
    OpenScope scope;
    uint64_t nextLifetime = 1;
};

State& Registry() {
    static State state;
    return state;
}

std::atomic<size_t> g_runtimeOwnedCount{0};
std::atomic<size_t> g_outputPresenterCount{0};
std::atomic<uint64_t> g_presents{0};
std::atomic<uint64_t> g_lastStreamlinePresent{0};
ce::present_interposer::CadenceTracker g_runtimeCadence;
std::atomic<bool> g_runtimeInputBoundary{false};
std::atomic<uint8_t> g_dlssgApiProvider{static_cast<uint8_t>(DLSSGApiProvider::kUnknown)};

void PublishCountLocked(State& state) {
    size_t count = 0;
    size_t outputPresenters = 0;
    for (const auto& entry : state.swapchains) {
        if (entry.second.presenter != Presenter::kApplication) {
            ++count;
        }
        if (IsRuntimeOwnedOutputPresenter(entry.second.presenter)) {
            ++outputPresenters;
        }
    }
    g_runtimeOwnedCount.store(count, std::memory_order_release);
    g_outputPresenterCount.store(outputPresenters, std::memory_order_release);
}

const char* ProviderName(DLSSGApiProvider provider) {
    switch (provider) {
        case DLSSGApiProvider::kStreamline:
            return "streamline";
        case DLSSGApiProvider::kCaptureEngine:
            return "captureengine";
        case DLSSGApiProvider::kForeign:
            return "foreign";
        case DLSSGApiProvider::kUnknown:
        default:
            return "unknown";
    }
}

}  // namespace

const char* PresenterName(Presenter presenter) {
    switch (presenter) {
        case Presenter::kApplication:
            return "application";
        case Presenter::kStreamlineDLSSG:
            return "streamline-dlssg";
        case Presenter::kFFXSwapchain:
            return "ffx-swapchain";
        case Presenter::kXeFGSwapchain:
            return "xefg-swapchain";
        case Presenter::kPresentInterposer:
            return "present-interposer";
        case Presenter::kUnknownExternal:
        default:
            return "unknown-external";
    }
}

ScopedRuntimeSwapchainCreate::ScopedRuntimeSwapchainCreate(Presenter presenter, HWND window, uintptr_t owner) {
    State& state = Registry();
    std::lock_guard<std::mutex> lock(state.mutex);
    previousPresenter_ = state.scope.presenter;
    previousWindow_ = state.scope.window;
    previousOwner_ = state.scope.owner;
    previousCreated_ = state.scope.created;
    state.scope = {presenter, window, owner, nullptr};
}

ScopedRuntimeSwapchainCreate::~ScopedRuntimeSwapchainCreate() {
    State& state = Registry();
    std::lock_guard<std::mutex> lock(state.mutex);
    state.scope = {previousPresenter_, previousWindow_, previousOwner_, previousCreated_};
}

IDXGISwapChain* ScopedRuntimeSwapchainCreate::CreatedSwapchain() const {
    State& state = Registry();
    std::lock_guard<std::mutex> lock(state.mutex);
    return state.scope.created;
}

Presenter OpenRuntimeCreateScopeFor(HWND window) {
    State& state = Registry();
    std::lock_guard<std::mutex> lock(state.mutex);
    if (state.scope.presenter == Presenter::kApplication) {
        return Presenter::kApplication;
    }
    return (!state.scope.window || !window || state.scope.window == window) ? state.scope.presenter
                                                                              : Presenter::kApplication;
}

Presenter NoteSwapchainCreated(IDXGISwapChain* swapchain, HWND window, ID3D12CommandQueue* queue,
                               bool xefgModuleOnCreateStack, const char* context, bool ffxRuntimeCreator) {
    if (!swapchain) {
        return Presenter::kApplication;
    }
    State& state = Registry();
    Presenter presenter = Presenter::kApplication;
    uint64_t lifetime = 0;
    size_t retired = 0;
    uintptr_t owner = 0;
    bool scopeMatches = false;
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        scopeMatches = state.scope.presenter != Presenter::kApplication &&
                       (!state.scope.window || !window || state.scope.window == window);
        presenter = ResolveCreatedSwapchainPresenter(scopeMatches ? state.scope.presenter : Presenter::kApplication,
                                                     xefgModuleOnCreateStack, ffxRuntimeCreator);
        if (scopeMatches) {
            state.scope.created = swapchain;
            owner = state.scope.owner;
        }
        // DXGI allows one flip swapchain per window: a successful create proves every other one CE registered
        // for that window gone, whatever address it had (the new one may reuse it).
        for (auto it = state.swapchains.begin(); it != state.swapchains.end();) {
            if (it->first == swapchain || (window && it->second.window == window)) {
                if (it->second.presenter != Presenter::kApplication) {
                    ++retired;
                }
                it = state.swapchains.erase(it);
            } else {
                ++it;
            }
        }
        if (presenter != Presenter::kApplication) {
            lifetime = state.nextLifetime++;
            state.swapchains[swapchain] = {presenter, window, queue, owner, lifetime};
        }
        PublishCountLocked(state);
    }
    if (presenter != Presenter::kApplication || retired != 0) {
        HookLogImportant(
            "[FG PRESENTER] %s: swapchain %p for HWND=%p is presented by %s (queue=%p evidence=%s lifetime=%llu "
            "owner=%p retiredForWindow=%zu)",
            context ? context : "create", swapchain, window, PresenterName(presenter), queue,
            scopeMatches                ? "runtime-create-scope"
            : xefgModuleOnCreateStack ? "module-on-create-stack"
            : ffxRuntimeCreator       ? "ffx-runtime-creator"
                                      : "none",
            static_cast<unsigned long long>(lifetime), reinterpret_cast<void*>(owner), retired);
    }
    return presenter;
}

void NoteSwapchainPresentQueue(IDXGISwapChain* swapchain, ID3D12CommandQueue* queue, const char* context) {
    if (!swapchain || !queue || !HasRuntimeOwnedSwapchains()) {
        return;
    }
    State& state = Registry();
    ID3D12CommandQueue* previous = nullptr;
    Presenter presenter = Presenter::kApplication;
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        auto it = state.swapchains.find(swapchain);
        if (it == state.swapchains.end() || it->second.queue == queue) {
            return;
        }
        previous = it->second.queue;
        it->second.queue = queue;
        presenter = it->second.presenter;
    }
    HookLogImportant("[FG PRESENTER] %s: %s swapchain %p now presents on queue %p (was %p)",
                     context ? context : "resize", PresenterName(presenter), swapchain, queue, previous);
}

void RetireOwner(uintptr_t owner, const char* reason) {
    if (!owner) {
        return;
    }
    State& state = Registry();
    size_t retired = 0;
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        for (auto it = state.swapchains.begin(); it != state.swapchains.end();) {
            if (it->second.owner == owner) {
                ++retired;
                it = state.swapchains.erase(it);
            } else {
                ++it;
            }
        }
        PublishCountLocked(state);
    }
    HookLogImportant("[FG PRESENTER] owner %p retired (%s): %zu swapchain(s) no longer runtime-presented",
                     reinterpret_cast<void*>(owner), reason ? reason : "destroyed", retired);
}

bool HasRuntimeOwnedSwapchains() {
    return g_runtimeOwnedCount.load(std::memory_order_acquire) != 0;
}

Presenter PresenterFor(IDXGISwapChain* swapchain) {
    if (!swapchain || !HasRuntimeOwnedSwapchains()) {
        return Presenter::kApplication;
    }
    State& state = Registry();
    std::lock_guard<std::mutex> lock(state.mutex);
    const auto it = state.swapchains.find(swapchain);
    return it == state.swapchains.end() ? Presenter::kApplication : it->second.presenter;
}

ID3D12CommandQueue* OutputQueue(IDXGISwapChain* swapchain) {
    if (!swapchain || !HasRuntimeOwnedSwapchains()) {
        return nullptr;
    }
    State& state = Registry();
    std::lock_guard<std::mutex> lock(state.mutex);
    const auto it = state.swapchains.find(swapchain);
    return it == state.swapchains.end() ? nullptr : it->second.queue;
}

uint64_t OutputPresenterLifetime(IDXGISwapChain* swapchain) {
    if (!swapchain || !HasRuntimeOwnedOutputPresenter()) {
        return 0;
    }
    State& state = Registry();
    std::lock_guard<std::mutex> lock(state.mutex);
    const auto it = state.swapchains.find(swapchain);
    return it == state.swapchains.end() || !IsRuntimeOwnedOutputPresenter(it->second.presenter)
               ? 0
               : it->second.lifetime;
}

void NotePresentOriginator(bool streamline) {
    const uint64_t present = g_presents.fetch_add(1, std::memory_order_acq_rel) + 1;
    if (streamline) {
        g_lastStreamlinePresent.store(present, std::memory_order_release);
    }
}

bool IsStreamlinePresentingNow() {
    return IsStreamlinePresenting(g_presents.load(std::memory_order_acquire),
                                  g_lastStreamlinePresent.load(std::memory_order_acquire));
}

void NoteRuntimeOutputPresent() {
    g_runtimeCadence.NoteOutputPresent();
}

bool NoteRuntimeApplicationFrame(int64_t nowUs, ce::present_interposer::CadenceVerdict* verdict) {
    g_runtimeInputBoundary.store(true, std::memory_order_release);
    return g_runtimeCadence.NoteApplicationPresent(nowUs, verdict);
}

void ResetRuntimeCadence() {
    g_runtimeCadence.Reset();
    g_runtimeInputBoundary.store(false, std::memory_order_release);
}

bool HasRuntimeInputBoundary() {
    return g_runtimeInputBoundary.load(std::memory_order_acquire);
}

void NoteDLSSGApiProvider(DLSSGApiProvider provider, const void* target, const char* modulePath) {
    if (provider == DLSSGApiProvider::kUnknown || provider == DLSSGApiProvider::kCaptureEngine) {
        return;  // CE's own wrappers say nothing about who serves the API underneath
    }
    const auto previous = static_cast<DLSSGApiProvider>(
        g_dlssgApiProvider.exchange(static_cast<uint8_t>(provider), std::memory_order_acq_rel));
    if (previous != provider) {
        HookLogImportant(
            "[FG PRESENTER] DLSS-G API (slDLSSGSetOptions/GetState) is served by %s (target=%p module=%s)%s",
            ProviderName(provider), target, modulePath && modulePath[0] ? modulePath : "unknown",
            provider == DLSSGApiProvider::kForeign
                ? " - its Streamline FG signal is a front while Streamline presents no frames"
                : "");
    }
}

bool IsDLSSGApiServedOutsideStreamline() {
    return static_cast<DLSSGApiProvider>(g_dlssgApiProvider.load(std::memory_order_acquire)) ==
           DLSSGApiProvider::kForeign;
}

bool HasRuntimeOwnedOutputPresenter() {
    return g_outputPresenterCount.load(std::memory_order_acquire) != 0;
}

bool IsStreamlineFGSignalFrontNow() {
    return IsStreamlineFGSignalFront(IsDLSSGApiServedOutsideStreamline(), IsStreamlinePresentingNow(),
                                     HasRuntimeOwnedOutputPresenter());
}

void ResetForTesting() {
    State& state = Registry();
    std::lock_guard<std::mutex> lock(state.mutex);
    state.swapchains.clear();
    state.scope = {};
    state.nextLifetime = 1;
    g_runtimeOwnedCount.store(0, std::memory_order_release);
    g_outputPresenterCount.store(0, std::memory_order_release);
    g_presents.store(0, std::memory_order_release);
    g_lastStreamlinePresent.store(0, std::memory_order_release);
    g_runtimeInputBoundary.store(false, std::memory_order_release);
    g_runtimeCadence.Reset();
    g_dlssgApiProvider.store(static_cast<uint8_t>(DLSSGApiProvider::kUnknown), std::memory_order_release);
}

}  // namespace ce::fg_presenter
