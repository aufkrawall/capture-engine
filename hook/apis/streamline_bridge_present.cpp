// Streamline is a 64-bit runtime; see streamline_bridge_translate.cpp.
#if defined(_M_X64) || defined(__x86_64__)

#include "streamline_bridge_present.h"

#include <dxgi1_2.h>

#include <atomic>
#include <mutex>

#include "../common/hook_common.h"
#include "../wrappers/inline_hook.h"
#include "streamline_bridge_dlssg.h"
#include "streamline_bridge_dlssg_gate.h"

namespace ce::streamline_bridge {
namespace {

// sl.common's present hooks as 2.x's interposer calls them before every proxy present
// (`commonInterface.cpp`: both run presentCommon, which counts the present).
using SlGetPluginFunctionFn = void*(const char* functionName);
using SlHookPresentFn = HRESULT(IDXGISwapChain* swapChain, UINT syncInterval, UINT flags, bool& skip);
using SlHookPresent1Fn = HRESULT(IDXGISwapChain* swapChain, UINT syncInterval, UINT flags,
                                 DXGI_PRESENT_PARAMETERS* params, bool& skip);

void* volatile g_originalHookPresent = nullptr;
void* volatile g_originalHookPresent1 = nullptr;

std::mutex g_ledgerMutex;
PresentMarkerLedger g_ledger;  // guarded by g_ledgerMutex

// The outcome and time of the previous present the guard saw. A title
// that re-presents after a failed or deferred present is the first explanation to rule out.
std::atomic<HRESULT> g_previousPresentResult{S_OK};
std::atomic<bool> g_previousPresentSkipped{false};
std::atomic<int64_t> g_previousPresentQpc{0};

int64_t QpcNow() {
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    return now.QuadPart;
}

int64_t QpcToUs(int64_t ticks) {
    static const int64_t frequency = [] {
        LARGE_INTEGER value{};
        QueryPerformanceFrequency(&value);
        return value.QuadPart > 0 ? value.QuadPart : 1;
    }();
    return ticks * 1'000'000 / frequency;
}

void PublishTrampoline(void* trampoline, void* context) {
    InterlockedExchangePointer(static_cast<void* volatile*>(context), trampoline);
}

void LogUnmarkedPresent(uint32_t n, bool remarked, uint32_t frameIndex, IDXGISwapChain* swapChain, UINT syncInterval,
                        UINT flags, const TitlePresentActivity& now, const TitlePresentActivity& before) {
    if (n >= 32 && (n % 256) != 0) {
        return;
    }
    const int64_t nowQpc = QpcNow();
    const int64_t previousQpc = g_previousPresentQpc.load(std::memory_order_relaxed);
    const long long sinceUs = previousQpc ? static_cast<long long>(QpcToUs(nowQpc - previousQpc)) : -1;
    const auto previousResult = static_cast<unsigned long>(g_previousPresentResult.load(std::memory_order_relaxed));
    const int previousSkipped = g_previousPresentSkipped.load(std::memory_order_relaxed) ? 1 : 0;
    HookLogImportant(
        "Streamline bridge: title presented without a Reflex PRESENT_START marker - %s frame %u so 2.x DLSS-G "
        "does not fail its Reflex check and skip this present (1.x had no such check) #%u",
        remarked ? "re-marked" : "could NOT re-mark (no frame token)", frameIndex, n + 1);
    // No constants, tags or upscaler evaluate since the previous present means a re-present of
    // frame N; constants for a newer frame mean the title rendered a frame it did not mark.
    HookLogImportant(
        "Streamline bridge: unmarked present #%u - swapchain=%p sync=%u flags=0x%X sinceLastPresentUs=%lld "
        "lastPresent(hr=0x%08lX skip=%d) | since that present: constants=%u(frame %u) tags=%u evaluates=%u(frame %u) "
        "markers=%u(last %u frame %u) | before it: constants=%u(frame %u) tags=%u evaluates=%u(frame %u) "
        "markers=%u(last %u frame %u)",
        n + 1, swapChain, syncInterval, flags, sinceUs, previousResult, previousSkipped, now.constants,
        now.lastConstantsFrame, now.tags, now.evaluates, now.lastEvaluateFrame, now.markers, now.lastMarker, now.lastMarkerFrame, before.constants,
        before.lastConstantsFrame, before.tags, before.evaluates, before.lastEvaluateFrame, before.markers,
        before.lastMarker, before.lastMarkerFrame);
}

// Runs on the title's present thread, ahead of the present 2.x is about to count.
void BeforeCountedPresent(IDXGISwapChain* swapChain, UINT syncInterval, UINT flags) {
    const bool dlssgEnabled = DlssgEnabledOnAnyViewport();
    uint32_t frameIndex = 0;
    bool needsMarker = false;
    TitlePresentActivity activity;
    TitlePresentActivity previousActivity;
    {
        std::lock_guard<std::mutex> lock(g_ledgerMutex);
        needsMarker = g_ledger.PresentNeedsMarker(flags, dlssgEnabled, &frameIndex);
        if (needsMarker) {
            activity = g_ledger.LastPresentActivity();
            previousActivity = g_ledger.PreviousPresentActivity();
        }
    }
    if (!needsMarker) {
        return;
    }
    const bool remarked = SynthesizePresentMarkersFor(frameIndex);
    static std::atomic<uint32_t> count{0};
    const uint32_t n = count.fetch_add(1, std::memory_order_relaxed);
    LogUnmarkedPresent(n, remarked, frameIndex, swapChain, syncInterval, flags, activity, previousActivity);
}

void AfterCountedPresent(UINT flags, HRESULT result, bool skip) {
    if ((flags & kDxgiPresentTest) != 0) {
        return;
    }
    g_previousPresentResult.store(result, std::memory_order_relaxed);
    g_previousPresentSkipped.store(skip, std::memory_order_relaxed);
    g_previousPresentQpc.store(QpcNow(), std::memory_order_relaxed);
}

HRESULT HookedSlHookPresent(IDXGISwapChain* swapChain, UINT syncInterval, UINT flags, bool& skip) {
    BeforeCountedPresent(swapChain, syncInterval, flags);
    auto* original = reinterpret_cast<SlHookPresentFn*>(
        InterlockedCompareExchangePointer(&g_originalHookPresent, nullptr, nullptr));
    const HRESULT result = original ? original(swapChain, syncInterval, flags, skip) : S_OK;
    AfterCountedPresent(flags, result, skip);
    return result;
}

HRESULT HookedSlHookPresent1(IDXGISwapChain* swapChain, UINT syncInterval, UINT flags,
                             DXGI_PRESENT_PARAMETERS* params, bool& skip) {
    BeforeCountedPresent(swapChain, syncInterval, flags);
    auto* original = reinterpret_cast<SlHookPresent1Fn*>(
        InterlockedCompareExchangePointer(&g_originalHookPresent1, nullptr, nullptr));
    const HRESULT result = original ? original(swapChain, syncInterval, flags, params, skip) : S_OK;
    AfterCountedPresent(flags, result, skip);
    return result;
}

bool IsInModule(void* address, HMODULE module) {
    HMODULE owner = nullptr;
    return address &&
           GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                              static_cast<LPCWSTR>(address), &owner) &&
           owner == module;
}

// The plugin publishes its hooks only through slGetPluginFunction, never as exports.
void* InstallOne(SlGetPluginFunctionFn* getPluginFunction, HMODULE v2Common, const char* name, void* detour,
                 void* volatile* original) {
    void* target = getPluginFunction(name);
    if (!IsInModule(target, v2Common)) {
        HookLogImportant("Streamline bridge: 2.x sl.common answered %s with %p, outside its image - not hooking it",
                         name, target);
        return nullptr;
    }
    void* trampoline = nullptr;
    if (!InlineHook::InstallPublished(target, detour, &trampoline, PublishTrampoline,
                                      const_cast<void**>(original))) {
        HookLogImportant("Streamline bridge: could not hook 2.x sl.common %s at %p", name, target);
        return nullptr;
    }
    return target;
}

}  // namespace

void NoteTitlePresentStart(uint32_t frameIndex) {
    std::lock_guard<std::mutex> lock(g_ledgerMutex);
    g_ledger.NoteTitlePresentStart(frameIndex);
}

void NoteTitleActivity(TitleActivity kind, uint32_t frameIndex, uint32_t marker) {
    std::lock_guard<std::mutex> lock(g_ledgerMutex);
    g_ledger.NoteTitleActivity(kind, frameIndex, marker);
}

bool InstallPresentMarkerGuard(HMODULE v2Common) {
    static std::atomic<bool> attempted{false};
    if (attempted.exchange(true, std::memory_order_acq_rel)) {
        return true;  // one attempt per process: a second would stack a hook on the first
    }
    auto* getPluginFunction =
        v2Common ? reinterpret_cast<SlGetPluginFunctionFn*>(GetProcAddress(v2Common, "slGetPluginFunction")) : nullptr;
    if (!IsInModule(reinterpret_cast<void*>(getPluginFunction), v2Common)) {
        HookLogImportant(
            "Streamline bridge: no 2.x sl.common slGetPluginFunction (module=%p) - presents the title leaves without a "
            "Reflex PRESENT_START marker will fail DLSS-G's Reflex check",
            v2Common);
        return false;
    }
    void* present = InstallOne(getPluginFunction, v2Common, "slHookPresent",
                               reinterpret_cast<void*>(&HookedSlHookPresent), &g_originalHookPresent);
    void* present1 = InstallOne(getPluginFunction, v2Common, "slHookPresent1",
                                reinterpret_cast<void*>(&HookedSlHookPresent1), &g_originalHookPresent1);
    HookLogImportant(
        "Streamline bridge: present-marker guard on 2.x sl.common - slHookPresent=%p slHookPresent1=%p (a title "
        "present without its own Reflex PRESENT_START re-marks the last presented frame while DLSS-G generates)",
        present, present1);
    return present != nullptr;
}

}  // namespace ce::streamline_bridge

#endif  // x64
