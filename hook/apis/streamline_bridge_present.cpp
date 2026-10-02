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
#include "streamline_bridge_present_timeline.h"
#include "streamline_bridge_swapchain_serial.h"

namespace ce::streamline_bridge {
namespace {

// sl.common's present hooks as 2.x's interposer calls them before every proxy present
// (`commonInterface.cpp`: both run presentCommon, which counts the present).
using SlGetPluginFunctionFn = void*(const char* functionName);
using SlHookPresentFn = HRESULT(IDXGISwapChain* swapChain, UINT syncInterval, UINT flags, bool& skip);
using SlHookPresent1Fn = HRESULT(IDXGISwapChain* swapChain, UINT syncInterval, UINT flags,
                                 DXGI_PRESENT_PARAMETERS* params, bool& skip);
// sl.common 2.14.1 answers slHookPresent and slHookPresent1 with ONE address (session
// 20261001_144612: both 00007FFF56028FA0): their bodies are identical - presentCommon(Flags,
// swapChain) - so the linker folded them. A detour there serves both signatures, so it may use only
// the arguments they share; the fourth is `bool& skip` for one caller and `params` for the other.
using SlHookPresentSharedFn = HRESULT(IDXGISwapChain* swapChain, UINT syncInterval, UINT flags, void* fourth);
// sl.dlss_g's swapchain before-hooks (2.x sl.api/internal.h PFun*Before).
using SlHookSetFullscreenStatePreFn = HRESULT(IDXGISwapChain* swapChain, BOOL fullscreen, IDXGIOutput* target,
                                              bool& skip);
using SlHookResizeSwapChainPreFn = HRESULT(IDXGISwapChain* swapChain, UINT bufferCount, UINT width, UINT height,
                                           DXGI_FORMAT format, UINT& swapChainFlags, bool& skip);
using SlHookResize1SwapChainPreFn = HRESULT(IDXGISwapChain* swapChain, UINT bufferCount, UINT width, UINT height,
                                            DXGI_FORMAT format, UINT swapChainFlags, const UINT* creationNodeMask,
                                            IUnknown* const* presentQueues, bool& skip);

void* volatile g_originalHookPresent = nullptr;
void* volatile g_originalHookPresent1 = nullptr;
void* volatile g_originalHookPresentShared = nullptr;
// sl.dlss_g's own present hooks. The interposer runs every plugin's before-present hook by
// priority (sl.common 0 first, sl.dlss_g 1000) on the presenting thread, whatever `skip` says.
void* volatile g_originalDlssgHookPresent = nullptr;
void* volatile g_originalDlssgHookPresent1 = nullptr;
void* volatile g_originalDlssgSetFullscreenStatePre = nullptr;
void* volatile g_originalDlssgResizeSwapChainPre = nullptr;
void* volatile g_originalDlssgResize1SwapChainPre = nullptr;
std::atomic<bool> g_absorbSupported{false};

// sl.dlss_g's present hooks and its fullscreen/resize before-hooks, one call at a time (see
// streamline_bridge_swapchain_serial.h: alt-tab in Witcher 3 freed a back buffer mid-present).
SwapchainCallSerializer g_dlssgSwapchainCalls;

void LogSerializedWait(const char* call, IDXGISwapChain* swapChain) {
    static std::atomic<uint32_t> waits{0};
    const uint32_t n = waits.fetch_add(1, std::memory_order_relaxed);
    if (n < 16 || (n % 256) == 0) {
        HookLogImportant(
            "Streamline bridge: sl.dlss_g %s waited for another thread's swapchain call to leave 2.x DLSS-G "
            "(swapchain=%p tid=0x%lX) - the title overlaps swapchain calls 2.x expects one at a time #%u",
            call, swapChain, GetCurrentThreadId(), n + 1);
    }
}

// Set by sl.common's hook for the present it absorbed; sl.dlss_g's hook, next on the same
// thread for the same present, sets `skip` and consumes it. Reassigned on every present.
thread_local bool t_absorbingPresent = false;

std::mutex g_ledgerMutex;
PresentMarkerLedger g_ledger;  // guarded by g_ledgerMutex
PresentTimeline g_timeline;    // guarded by g_ledgerMutex

static_assert(static_cast<uint32_t>(PresentAction::kForward) == 0 &&
                  static_cast<uint32_t>(PresentAction::kReMark) == 1 &&
                  static_cast<uint32_t>(PresentAction::kAbsorb) == 2,
              "PresentTimeline::ActionName follows PresentAction's order");

// The outcome and time of the previous present the guard saw. A title
// that re-presents after a failed or deferred present is the first explanation to rule out.
std::atomic<HRESULT> g_previousPresentResult{S_OK};
std::atomic<bool> g_previousPresentAbsorbed{false};
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

int64_t NowUs() { return QpcToUs(QpcNow()); }

TimelineKind TimelineKindFor(TitleActivity kind) {
    switch (kind) {
    case TitleActivity::kConstants:
        return TimelineKind::kConstants;
    case TitleActivity::kTag:
        return TimelineKind::kTag;
    case TitleActivity::kEvaluate:
        return TimelineKind::kEvaluate;
    case TitleActivity::kMarker:
        break;
    }
    return TimelineKind::kMarker;
}

void PublishTrampoline(void* trampoline, void* context) {
    InterlockedExchangePointer(static_cast<void* volatile*>(context), trampoline);
}

const char* UnmarkedPresentOutcome(PresentAction action, bool remarked) {
    if (action == PresentAction::kAbsorb) {
        return "absorbed it (a re-present: 2.x would read inputs the title is already rewriting) after";
    }
    return remarked ? "re-marked" : "could NOT re-mark (no frame token)";
}

void LogUnmarkedPresent(uint32_t n, PresentAction action, bool remarked, uint32_t frameIndex,
                        IDXGISwapChain* swapChain, UINT syncInterval, UINT flags, const TitlePresentActivity& now,
                        const TitlePresentActivity& before) {
    if (n >= 32 && (n % 256) != 0) {
        return;
    }
    const int64_t nowQpc = QpcNow();
    const int64_t previousQpc = g_previousPresentQpc.load(std::memory_order_relaxed);
    const long long sinceUs = previousQpc ? static_cast<long long>(QpcToUs(nowQpc - previousQpc)) : -1;
    const auto previousResult = static_cast<unsigned long>(g_previousPresentResult.load(std::memory_order_relaxed));
    const int previousAbsorbed = g_previousPresentAbsorbed.load(std::memory_order_relaxed) ? 1 : 0;
    HookLogImportant(
        "Streamline bridge: title presented without a Reflex PRESENT_START marker - %s frame %u (2.x DLSS-G "
        "checks Reflex and reads its tagged inputs at every present) #%u",
        UnmarkedPresentOutcome(action, remarked), frameIndex, n + 1);
    // No constants, tags or upscaler evaluate since the previous present means a re-present of
    // frame N; constants for a newer frame mean the title rendered a frame it did not mark.
    HookLogImportant(
        "Streamline bridge: unmarked present #%u - swapchain=%p sync=%u flags=0x%X sinceLastPresentUs=%lld "
        "lastPresent(hr=0x%08lX absorbed=%d) | since that present: constants=%u(frame %u) tags=%u "
        "evaluates=%u(frame %u) markers=%u(last %u frame %u) | before it: constants=%u(frame %u) tags=%u "
        "evaluates=%u(frame %u) markers=%u(last %u frame %u)",
        n + 1, swapChain, syncInterval, flags, sinceUs, previousResult, previousAbsorbed, now.constants,
        now.lastConstantsFrame, now.tags, now.evaluates, now.lastEvaluateFrame, now.markers, now.lastMarker,
        now.lastMarkerFrame, before.constants, before.lastConstantsFrame, before.tags, before.evaluates,
        before.lastEvaluateFrame, before.markers, before.lastMarker, before.lastMarkerFrame);
}

// Runs on the title's present thread, ahead of the present 2.x is about to count.
PresentAction BeforeCountedPresent(IDXGISwapChain* swapChain, UINT syncInterval, UINT flags) {
    const bool dlssgEnabled = DlssgEnabledOnAnyViewport();
    const bool absorbSupported = g_absorbSupported.load(std::memory_order_acquire);
    uint32_t frameIndex = 0;
    PresentAction action = PresentAction::kForward;
    TitlePresentActivity activity;
    TitlePresentActivity previousActivity;
    {
        std::lock_guard<std::mutex> lock(g_ledgerMutex);
        action = g_ledger.ClassifyPresent(flags, dlssgEnabled, absorbSupported, &frameIndex);
        if ((flags & kDxgiPresentTest) == 0) {
            const int64_t nowUs = NowUs();
            g_timeline.Record({nowUs, TimelineKind::kPresent, static_cast<uint32_t>(action), 0});
            if (action == PresentAction::kAbsorb) {
                g_timeline.ArmDump(nowUs);
            }
        }
        if (action != PresentAction::kForward) {
            activity = g_ledger.LastPresentActivity();
            previousActivity = g_ledger.PreviousPresentActivity();
        }
    }
    if (action == PresentAction::kForward) {
        return action;
    }
    const bool remarked = action == PresentAction::kReMark && SynthesizePresentMarkersFor(frameIndex);
    static std::atomic<uint32_t> count{0};
    const uint32_t n = count.fetch_add(1, std::memory_order_relaxed);
    LogUnmarkedPresent(n, action, remarked, frameIndex, swapChain, syncInterval, flags, activity, previousActivity);
    return action;
}

void AfterCountedPresent(UINT flags, HRESULT result, bool absorbed) {
    if ((flags & kDxgiPresentTest) != 0) {
        return;
    }
    g_previousPresentResult.store(result, std::memory_order_relaxed);
    g_previousPresentAbsorbed.store(absorbed, std::memory_order_relaxed);
    g_previousPresentQpc.store(QpcNow(), std::memory_order_relaxed);
}

// sl.common's hook decides; an absorbed present never reaches presentCommon (no frame counted).
// It never touches `skip`: with folded hooks that argument is not where it would write. sl.dlss_g's
// hook, whose two entry points are distinct, sets it.
bool AbsorbAtCommonHook(IDXGISwapChain* swapChain, UINT syncInterval, UINT flags) {
    t_absorbingPresent = BeforeCountedPresent(swapChain, syncInterval, flags) == PresentAction::kAbsorb;
    if (t_absorbingPresent) {
        AfterCountedPresent(flags, S_OK, true);
    }
    return t_absorbingPresent;
}

HRESULT HookedSlHookPresent(IDXGISwapChain* swapChain, UINT syncInterval, UINT flags, bool& skip) {
    if (AbsorbAtCommonHook(swapChain, syncInterval, flags)) {
        return S_OK;
    }
    auto* original = reinterpret_cast<SlHookPresentFn*>(
        InterlockedCompareExchangePointer(&g_originalHookPresent, nullptr, nullptr));
    const HRESULT result = original ? original(swapChain, syncInterval, flags, skip) : S_OK;
    AfterCountedPresent(flags, result, false);
    return result;
}

HRESULT HookedSlHookPresent1(IDXGISwapChain* swapChain, UINT syncInterval, UINT flags,
                             DXGI_PRESENT_PARAMETERS* params, bool& skip) {
    if (AbsorbAtCommonHook(swapChain, syncInterval, flags)) {
        return S_OK;
    }
    auto* original = reinterpret_cast<SlHookPresent1Fn*>(
        InterlockedCompareExchangePointer(&g_originalHookPresent1, nullptr, nullptr));
    const HRESULT result = original ? original(swapChain, syncInterval, flags, params, skip) : S_OK;
    AfterCountedPresent(flags, result, false);
    return result;
}

// The folded entry point. The fourth argument passes through untouched. A fifth (Present1's
// `skip`, on the stack) cannot be forwarded, and identical code for both callers cannot use it.
HRESULT HookedSlHookPresentShared(IDXGISwapChain* swapChain, UINT syncInterval, UINT flags, void* fourth) {
    if (AbsorbAtCommonHook(swapChain, syncInterval, flags)) {
        return S_OK;
    }
    auto* original = reinterpret_cast<SlHookPresentSharedFn*>(
        InterlockedCompareExchangePointer(&g_originalHookPresentShared, nullptr, nullptr));
    const HRESULT result = original ? original(swapChain, syncInterval, flags, fourth) : S_OK;
    AfterCountedPresent(flags, result, false);
    return result;
}

// True once: the present sl.common's hook just absorbed on this thread.
bool ConsumeAbsorbedPresent(bool& skip) {
    if (!t_absorbingPresent) {
        return false;
    }
    t_absorbingPresent = false;
    skip = true;
    return true;
}

// sl.dlss_g's slHookPresent runs its slHookPresent1 (session 20261001_150639 logged two returns per
// present), so only the outermost return of a present counts.
thread_local uint32_t t_dlssgHookDepth = 0;

// The timeline around an absorbed present, once two later presents came back from DLSS-G: how long
// DLSS-G's hook holds the title per present, and when the title's next frame arrived.
void AfterDlssgPresent(UINT flags, bool absorbed) {
    if ((flags & kDxgiPresentTest) != 0 || t_dlssgHookDepth != 0) {
        return;
    }
    std::string timeline;
    {
        std::lock_guard<std::mutex> lock(g_ledgerMutex);
        g_timeline.Record({NowUs(), TimelineKind::kDlssgReturn, 0, 0});
        if (absorbed || !g_timeline.PresentReturned()) {
            return;
        }
        timeline = g_timeline.Format(g_timeline.AbsorbedUs());
    }
    static std::atomic<uint32_t> dumps{0};
    const uint32_t n = dumps.fetch_add(1, std::memory_order_relaxed);
    if (n < 8 || (n % 64) == 0) {
        HookLogImportant("Streamline bridge: absorbed-present timeline #%u (ms from the absorbed present):%s", n + 1,
                         timeline.c_str());
    }
}

HRESULT HookedDlssgHookPresent(IDXGISwapChain* swapChain, UINT syncInterval, UINT flags, bool& skip) {
    if (ConsumeAbsorbedPresent(skip)) {
        AfterDlssgPresent(flags, true);
        return S_OK;
    }
    auto* original = reinterpret_cast<SlHookPresentFn*>(
        InterlockedCompareExchangePointer(&g_originalDlssgHookPresent, nullptr, nullptr));
    SwapchainCallScope serial(g_dlssgSwapchainCalls);
    if (serial.Waited()) {
        LogSerializedWait("Present", swapChain);
    }
    ++t_dlssgHookDepth;
    const HRESULT result = original ? original(swapChain, syncInterval, flags, skip) : S_OK;
    --t_dlssgHookDepth;
    AfterDlssgPresent(flags, false);
    return result;
}

HRESULT HookedDlssgHookPresent1(IDXGISwapChain* swapChain, UINT syncInterval, UINT flags,
                                DXGI_PRESENT_PARAMETERS* params, bool& skip) {
    if (ConsumeAbsorbedPresent(skip)) {
        AfterDlssgPresent(flags, true);
        return S_OK;
    }
    auto* original = reinterpret_cast<SlHookPresent1Fn*>(
        InterlockedCompareExchangePointer(&g_originalDlssgHookPresent1, nullptr, nullptr));
    SwapchainCallScope serial(g_dlssgSwapchainCalls);
    if (serial.Waited()) {
        LogSerializedWait("Present1", swapChain);
    }
    ++t_dlssgHookDepth;
    const HRESULT result = original ? original(swapChain, syncInterval, flags, params, skip) : S_OK;
    --t_dlssgHookDepth;
    AfterDlssgPresent(flags, false);
    return result;
}

// The before-hooks flush DLSS-G and force-destroy its back-buffer wrappers. The after-hooks stay
// outside the serializer: SetFullscreenStatePost waits for presents to settle.
HRESULT HookedDlssgSetFullscreenStatePre(IDXGISwapChain* swapChain, BOOL fullscreen, IDXGIOutput* target,
                                         bool& skip) {
    auto* original = reinterpret_cast<SlHookSetFullscreenStatePreFn*>(
        InterlockedCompareExchangePointer(&g_originalDlssgSetFullscreenStatePre, nullptr, nullptr));
    SwapchainCallScope serial(g_dlssgSwapchainCalls);
    if (serial.Waited()) {
        LogSerializedWait(fullscreen ? "SetFullscreenState(TRUE)" : "SetFullscreenState(FALSE)", swapChain);
    }
    return original ? original(swapChain, fullscreen, target, skip) : S_OK;
}

HRESULT HookedDlssgResizeSwapChainPre(IDXGISwapChain* swapChain, UINT bufferCount, UINT width, UINT height,
                                      DXGI_FORMAT format, UINT& swapChainFlags, bool& skip) {
    auto* original = reinterpret_cast<SlHookResizeSwapChainPreFn*>(
        InterlockedCompareExchangePointer(&g_originalDlssgResizeSwapChainPre, nullptr, nullptr));
    SwapchainCallScope serial(g_dlssgSwapchainCalls);
    if (serial.Waited()) {
        LogSerializedWait("ResizeBuffers", swapChain);
    }
    return original ? original(swapChain, bufferCount, width, height, format, swapChainFlags, skip) : S_OK;
}

HRESULT HookedDlssgResize1SwapChainPre(IDXGISwapChain* swapChain, UINT bufferCount, UINT width, UINT height,
                                       DXGI_FORMAT format, UINT swapChainFlags, const UINT* creationNodeMask,
                                       IUnknown* const* presentQueues, bool& skip) {
    auto* original = reinterpret_cast<SlHookResize1SwapChainPreFn*>(
        InterlockedCompareExchangePointer(&g_originalDlssgResize1SwapChainPre, nullptr, nullptr));
    SwapchainCallScope serial(g_dlssgSwapchainCalls);
    if (serial.Waited()) {
        LogSerializedWait("ResizeBuffers1", swapChain);
    }
    return original ? original(swapChain, bufferCount, width, height, format, swapChainFlags, creationNodeMask,
                               presentQueues, skip)
                    : S_OK;
}

bool IsInModule(void* address, HMODULE module) {
    HMODULE owner = nullptr;
    return address &&
           GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                              static_cast<LPCWSTR>(address), &owner) &&
           owner == module;
}

SlGetPluginFunctionFn* PluginFunctionLookup(HMODULE plugin) {
    auto* getPluginFunction =
        plugin ? reinterpret_cast<SlGetPluginFunctionFn*>(GetProcAddress(plugin, "slGetPluginFunction")) : nullptr;
    return IsInModule(reinterpret_cast<void*>(getPluginFunction), plugin) ? getPluginFunction : nullptr;
}

// The plugin publishes its hooks only through slGetPluginFunction, never as exports.
void* InstallOne(SlGetPluginFunctionFn* getPluginFunction, HMODULE plugin, const char* pluginName, const char* name,
                 void* detour, void* volatile* original) {
    void* target = getPluginFunction(name);
    if (!IsInModule(target, plugin)) {
        HookLogImportant("Streamline bridge: 2.x %s answered %s with %p, outside its image - not hooking it",
                         pluginName, name, target);
        return nullptr;
    }
    void* trampoline = nullptr;
    if (!InlineHook::InstallPublished(target, detour, &trampoline, PublishTrampoline,
                                      const_cast<void**>(original))) {
        HookLogImportant("Streamline bridge: could not hook 2.x %s %s at %p", pluginName, name, target);
        return nullptr;
    }
    return target;
}

// Each before-hook is hooked only at an address no other bridge detour owns: a linker-folded entry
// point (sl.common folds its two present hooks) cannot take a second detour with another signature.
void InstallSwapchainSerializer(SlGetPluginFunctionFn* dlssgLookup, HMODULE v2Dlssg, void* present, void* present1) {
    struct Entry {
        const char* name;
        void* detour;
        void* volatile* original;
        void* installed;
    };
    Entry entries[] = {
        {"slHookSetFullscreenStatePre", reinterpret_cast<void*>(&HookedDlssgSetFullscreenStatePre),
         &g_originalDlssgSetFullscreenStatePre, nullptr},
        {"slHookResizeSwapChainPre", reinterpret_cast<void*>(&HookedDlssgResizeSwapChainPre),
         &g_originalDlssgResizeSwapChainPre, nullptr},
        {"slHookResize1SwapChainPre", reinterpret_cast<void*>(&HookedDlssgResize1SwapChainPre),
         &g_originalDlssgResize1SwapChainPre, nullptr},
    };
    void* taken[5] = {present, present1, nullptr, nullptr, nullptr};
    size_t takenCount = 2;
    for (Entry& entry : entries) {
        void* target = dlssgLookup(entry.name);
        bool folded = false;
        for (size_t i = 0; i < takenCount; ++i) {
            folded = folded || (target != nullptr && target == taken[i]);
        }
        if (folded) {
            HookLogImportant(
                "Streamline bridge: 2.x sl.dlss_g %s shares entry point %p with another hooked function - not "
                "serializing it",
                entry.name, target);
            continue;
        }
        entry.installed = InstallOne(dlssgLookup, v2Dlssg, "sl.dlss_g", entry.name, entry.detour, entry.original);
        if (entry.installed) {
            taken[takenCount++] = entry.installed;
        }
    }
    HookLogImportant(
        "Streamline bridge: swapchain call serializer - sl.dlss_g Present=%p Present1=%p SetFullscreenStatePre=%p "
        "ResizeSwapChainPre=%p Resize1SwapChainPre=%p run one at a time (2.x's interposer leaves that to the title; "
        "a 1.x title may call SetFullscreenState from its window thread mid-present)",
        present, present1, entries[0].installed, entries[1].installed, entries[2].installed);
}

}  // namespace

void NoteTitlePresentStart(uint32_t frameIndex) {
    std::lock_guard<std::mutex> lock(g_ledgerMutex);
    g_ledger.NoteTitlePresentStart(frameIndex);
}

void NoteTitleActivity(TitleActivity kind, uint32_t frameIndex, uint32_t marker) {
    std::lock_guard<std::mutex> lock(g_ledgerMutex);
    g_ledger.NoteTitleActivity(kind, frameIndex, marker);
    g_timeline.Record({NowUs(), TimelineKindFor(kind), marker, frameIndex});
}

void NoteTitleSleepReturned() {
    std::lock_guard<std::mutex> lock(g_ledgerMutex);
    g_timeline.Record({NowUs(), TimelineKind::kSleepReturn, 0, 0});
}

bool InstallPresentMarkerGuard(HMODULE v2Common, HMODULE v2Dlssg) {
    static std::atomic<bool> attempted{false};
    if (attempted.exchange(true, std::memory_order_acq_rel)) {
        return true;  // one attempt per process: a second would stack a hook on the first
    }
    // sl.dlss_g's hooks do not depend on sl.common's: the serializer must hold without the guard.
    void* dlssgPresent = nullptr;
    void* dlssgPresent1 = nullptr;
    if (auto* dlssgLookup = PluginFunctionLookup(v2Dlssg)) {
        dlssgPresent = InstallOne(dlssgLookup, v2Dlssg, "sl.dlss_g", "slHookPresent",
                                  reinterpret_cast<void*>(&HookedDlssgHookPresent), &g_originalDlssgHookPresent);
        dlssgPresent1 = InstallOne(dlssgLookup, v2Dlssg, "sl.dlss_g", "slHookPresent1",
                                   reinterpret_cast<void*>(&HookedDlssgHookPresent1), &g_originalDlssgHookPresent1);
        InstallSwapchainSerializer(dlssgLookup, v2Dlssg, dlssgPresent, dlssgPresent1);
    } else {
        HookLogImportant(
            "Streamline bridge: no 2.x sl.dlss_g slGetPluginFunction (module=%p) - its present and fullscreen/resize "
            "hooks are not serialized",
            v2Dlssg);
    }

    auto* commonLookup = PluginFunctionLookup(v2Common);
    if (!commonLookup) {
        HookLogImportant(
            "Streamline bridge: no 2.x sl.common slGetPluginFunction (module=%p) - presents the title leaves without a "
            "Reflex PRESENT_START marker will fail DLSS-G's Reflex check",
            v2Common);
        return false;
    }
    // Both present paths must pass the guard. Folded entry points get one shared detour.
    void* present = nullptr;
    void* present1 = nullptr;
    const bool folded = commonLookup("slHookPresent") == commonLookup("slHookPresent1");
    if (folded) {
        present = InstallOne(commonLookup, v2Common, "sl.common", "slHookPresent",
                             reinterpret_cast<void*>(&HookedSlHookPresentShared), &g_originalHookPresentShared);
        present1 = present;
    } else {
        present = InstallOne(commonLookup, v2Common, "sl.common", "slHookPresent",
                             reinterpret_cast<void*>(&HookedSlHookPresent), &g_originalHookPresent);
        present1 = InstallOne(commonLookup, v2Common, "sl.common", "slHookPresent1",
                              reinterpret_cast<void*>(&HookedSlHookPresent1), &g_originalHookPresent1);
    }

    // Absorbing needs both plugins' hooks on both present paths: sl.common alone would leave
    // DLSS-G processing a present Streamline never counted.
    // sl.dlss_g's two entry points must be distinct: its detour is the one that writes `skip`.
    const bool absorb = present && present1 && dlssgPresent && dlssgPresent1 && dlssgPresent != dlssgPresent1;
    g_absorbSupported.store(absorb, std::memory_order_release);
    HookLogImportant(
        "Streamline bridge: present-marker guard - sl.common slHookPresent=%p slHookPresent1=%p%s, sl.dlss_g "
        "(module=%p) slHookPresent=%p slHookPresent1=%p - a title present without its own Reflex PRESENT_START "
        "while DLSS-G generates is %s",
        present, present1, folded ? " (one folded entry point, shared detour)" : "", v2Dlssg, dlssgPresent,
        dlssgPresent1,
        absorb ? "absorbed when it re-presents (no constants, tags or evaluate since the last present), "
                 "otherwise re-marked"
               : "re-marked (absorbing needs sl.common's and both distinct sl.dlss_g present hooks)");
    return present != nullptr;
}

}  // namespace ce::streamline_bridge

#endif  // x64
