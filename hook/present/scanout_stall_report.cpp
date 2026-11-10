#include "scanout_stall_report.h"

#include <dxgi1_6.h>

#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

#include "common/ipc/display_timing_shared.h"
#include "hook/metrics/perf_logger.h"
#include "hook/runtime/hook_clock.h"
#include "hook/runtime/hook_common.h"
#include "hook/runtime/ipc_client.h"
#include "presentation_path_policy.h"
#include "scanout_stall_policy.h"

namespace ce::scanout_stall {
namespace {

constexpr size_t kMaxAbove = 16;
// Episodes up to this one are reported in full; later ones only every kDetailedEpisodes-th, so a flapping window
// cannot fill the log.
constexpr uint64_t kDetailedEpisodes = 16;

struct AboveWindow {
    char className[32] = {};
    char exeName[32] = {};
    unsigned long long exStyle = 0;
    RECT rect = {};
};

struct WindowState {
    bool known = false;
    HWND hwnd = nullptr;
    bool visible = false;
    bool iconic = false;
    bool foreground = false;
    DWORD cloaked = 0;
    RECT window = {};
    RECT client = {};
    RECT monitor = {};
    unsigned long long exStyle = 0;
    char blockers[128] = {};
    AboveWindow above[kMaxAbove] = {};
    size_t aboveShown = 0;
    uint32_t aboveTotal = 0;
};

struct Snapshot {
    bool descKnown = false;
    DXGI_SWAP_CHAIN_DESC desc = {};
    HRESULT statsHr = E_FAIL;
    DXGI_FRAME_STATISTICS stats = {};
    HRESULT presentCountHr = E_FAIL;
    UINT lastPresentCount = 0;
    bool fullscreenKnown = false;
    BOOL fullscreen = FALSE;
    bool hwCompositionKnown = false;
    UINT hwComposition = 0;
    WindowState window;
    uint32_t signature = 0;  // changes when the window facts or the windows above it do
};

std::mutex g_Mutex;
StallTracker g_Tracker;
FirstChangeWatch g_Watch;
PresentResultTally g_Tally;
IDXGISwapChain* g_LastChain = nullptr;
UINT g_LastSync = 0;
UINT g_LastFlags = 0;
// The running episode: its window facts at the start, and the frame statistics to diff against.
uint32_t g_StartSignature = 0;
bool g_StartStatsKnown = false;
UINT g_StartRefreshCount = 0;
bool g_EpisodeDetailed = false;

uint64_t Id(const IDXGISwapChain* chain) {
    return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(chain));
}

bool ReadDisplay(DisplayObservation& observation, int64_t& nowUs, const SharedDisplayTiming** timing) {
    IPCClient* const ipc = g_IPC;
    SharedMemoryLayout* const shared = ipc ? ipc->GetSharedMem() : nullptr;
    if (!shared) {
        return false;
    }
    const SharedDisplayTiming& display = shared->displayTiming;
    LARGE_INTEGER counter = {};
    ce::hook_clock::QueryCounter(&counter);
    nowUs = DisplayTimingQpcToUs(counter.QuadPart, PerfLogger::GetQpcFrequency());
    const uint64_t generation = display.publicationGeneration.load(std::memory_order_acquire);
    // An odd generation is a reset in progress.
    observation.active = display.GetStatus() == DisplayTimingStatus::Active && (generation & 1u) == 0;
    observation.generation = generation;
    observation.sequence = display.writeSequence.load(std::memory_order_acquire);
    observation.publishUs = display.lastPublishQpcUs.load(std::memory_order_acquire);
    if (timing) {
        *timing = &display;
    }
    return true;
}

bool QueryCloaked(HWND window, DWORD* cloaked) {
    using DwmGetWindowAttributeFn = HRESULT(WINAPI*)(HWND, DWORD, PVOID, DWORD);
    constexpr DWORD kDwmaCloaked = 13;
    // Looked up per call: the module may load later than the first stall, and snapshots are rare.
    const HMODULE dwmapi = GetModuleHandleW(L"dwmapi.dll");
    const auto query =
        dwmapi ? reinterpret_cast<DwmGetWindowAttributeFn>(GetProcAddress(dwmapi, "DwmGetWindowAttribute")) : nullptr;
    return query && SUCCEEDED(query(window, kDwmaCloaked, cloaked, sizeof(*cloaked)));
}

uint32_t Mix(uint32_t hash, uint32_t value) {
    return (hash ^ value) * 16777619u;
}

void ExeNameOf(HWND window, char* out, size_t capacity) {
    out[0] = '\0';
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    if (pid == 0) {
        return;
    }
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) {
        std::snprintf(out, capacity, "pid%lu", static_cast<unsigned long>(pid));
        return;
    }
    char path[MAX_PATH] = {};
    DWORD length = static_cast<DWORD>(sizeof(path));
    if (QueryFullProcessImageNameA(process, 0, path, &length)) {
        const char* const name = std::strrchr(path, '\\');
        std::snprintf(out, capacity, "%s", name ? name + 1 : path);
    }
    CloseHandle(process);
}

// The visible, uncloaked top-level windows above the game's in the z-order that cover part of it.
void CollectAbove(HWND game, WindowState& state) {
    HWND root = GetAncestor(game, GA_ROOT);
    if (!root) {
        root = game;
    }
    int guard = 0;
    for (HWND other = GetWindow(root, GW_HWNDPREV); other && guard < 1024; other = GetWindow(other, GW_HWNDPREV)) {
        ++guard;
        if (!IsWindowVisible(other) || IsIconic(other)) {
            continue;
        }
        DWORD cloaked = 0;
        if (QueryCloaked(other, &cloaked) && cloaked != 0) {
            continue;
        }
        RECT rect = {};
        RECT overlap = {};
        if (!GetWindowRect(other, &rect) || !IntersectRect(&overlap, &rect, &state.window)) {
            continue;
        }
        ++state.aboveTotal;
        if (state.aboveShown >= kMaxAbove) {
            continue;
        }
        AboveWindow& entry = state.above[state.aboveShown++];
        entry.rect = rect;
        entry.exStyle = static_cast<unsigned long long>(GetWindowLongPtrW(other, GWL_EXSTYLE));
        GetClassNameA(other, entry.className, static_cast<int>(sizeof(entry.className)));
        ExeNameOf(other, entry.exeName, sizeof(entry.exeName));
    }
}

void CollectWindow(HWND window, UINT swapchainWidth, UINT swapchainHeight, WindowState& state) {
    if (!window || !IsWindow(window)) {
        return;
    }
    state.known = true;
    state.hwnd = window;
    state.visible = IsWindowVisible(window) != FALSE;
    state.iconic = IsIconic(window) != FALSE;
    HWND foreground = GetForegroundWindow();
    HWND foregroundRoot = foreground ? GetAncestor(foreground, GA_ROOT) : nullptr;
    HWND windowRoot = GetAncestor(window, GA_ROOT);
    state.foreground = foregroundRoot && foregroundRoot == (windowRoot ? windowRoot : window);
    QueryCloaked(window, &state.cloaked);
    state.exStyle = static_cast<unsigned long long>(GetWindowLongPtrW(window, GWL_EXSTYLE));

    presentation_path::WindowFacts facts;
    MONITORINFO monitorInfo = {};
    monitorInfo.cbSize = sizeof(monitorInfo);
    HMONITOR monitor = MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
    facts.windowKnown = GetWindowRect(window, &state.window) && GetClientRect(window, &state.client) && monitor &&
                        GetMonitorInfoW(monitor, &monitorInfo);
    if (facts.windowKnown) {
        state.monitor = monitorInfo.rcMonitor;
        facts.minimized = state.iconic;
        facts.layered = (state.exStyle & WS_EX_LAYERED) != 0;
        facts.topmost = (state.exStyle & WS_EX_TOPMOST) != 0;
        facts.window = {state.window.left, state.window.top, state.window.right, state.window.bottom};
        facts.clientWidth = state.client.right - state.client.left;
        facts.clientHeight = state.client.bottom - state.client.top;
        facts.monitor = {state.monitor.left, state.monitor.top, state.monitor.right, state.monitor.bottom};
        facts.swapchainWidth = swapchainWidth;
        facts.swapchainHeight = swapchainHeight;
        // DPI virtualization is judged by the Vulkan layer, which reads the window's awareness; here the facts are
        // as the presenting thread sees them.
        CollectAbove(window, state);
    }
    presentation_path::DescribeBlockers(presentation_path::IndependentFlipBlockers(facts), state.blockers,
                                        sizeof(state.blockers));
}

uint32_t SignatureOf(const WindowState& state) {
    uint32_t hash = 2166136261u;
    hash = Mix(hash, state.known ? 1u : 0u);
    hash = Mix(hash, state.visible ? 1u : 0u);
    hash = Mix(hash, state.iconic ? 1u : 0u);
    hash = Mix(hash, state.foreground ? 1u : 0u);
    hash = Mix(hash, static_cast<uint32_t>(state.cloaked));
    for (const RECT* rect : {&state.window, &state.monitor}) {
        hash = Mix(hash, static_cast<uint32_t>(rect->left));
        hash = Mix(hash, static_cast<uint32_t>(rect->top));
        hash = Mix(hash, static_cast<uint32_t>(rect->right));
        hash = Mix(hash, static_cast<uint32_t>(rect->bottom));
    }
    hash = Mix(hash, static_cast<uint32_t>(state.exStyle));
    hash = Mix(hash, state.aboveTotal);
    for (size_t i = 0; i < state.aboveShown; ++i) {
        for (const char* c = state.above[i].exeName; *c; ++c) {
            hash = Mix(hash, static_cast<uint8_t>(*c));
        }
        hash = Mix(hash, static_cast<uint32_t>(state.above[i].exStyle));
        hash = Mix(hash, static_cast<uint32_t>(state.above[i].rect.left));
        hash = Mix(hash, static_cast<uint32_t>(state.above[i].rect.top));
    }
    return hash == 0 ? 1u : hash;
}

// Called on the presenting thread with its own swapchain: nothing here dereferences a chain it does not hold.
void Collect(IDXGISwapChain* chain, bool full, Snapshot& snapshot) {
    snapshot.descKnown = SUCCEEDED(chain->GetDesc(&snapshot.desc));
    snapshot.statsHr = chain->GetFrameStatistics(&snapshot.stats);
    snapshot.presentCountHr = chain->GetLastPresentCount(&snapshot.lastPresentCount);
    if (full) {
        IDXGIOutput* fullscreenOutput = nullptr;
        snapshot.fullscreenKnown = SUCCEEDED(chain->GetFullscreenState(&snapshot.fullscreen, &fullscreenOutput));
        if (fullscreenOutput) {
            fullscreenOutput->Release();
        }
        IDXGIOutput* output = nullptr;
        if (SUCCEEDED(chain->GetContainingOutput(&output)) && output) {
            IDXGIOutput6* output6 = nullptr;
            if (SUCCEEDED(output->QueryInterface(IID_PPV_ARGS(&output6))) && output6) {
                snapshot.hwCompositionKnown = SUCCEEDED(output6->CheckHardwareCompositionSupport(&snapshot.hwComposition));
                output6->Release();
            }
            output->Release();
        }
    }
    if (snapshot.descKnown) {
        CollectWindow(snapshot.desc.OutputWindow, snapshot.desc.BufferDesc.Width, snapshot.desc.BufferDesc.Height,
                      snapshot.window);
    }
    snapshot.signature = SignatureOf(snapshot.window);
}

std::string DescribeFrameStats(const Snapshot& snapshot) {
    char text[160];
    if (SUCCEEDED(snapshot.statsHr)) {
        std::snprintf(text, sizeof(text), "presentCount=%u refreshCount=%u syncRefreshCount=%u lastPresentCount=%u",
                      snapshot.stats.PresentCount, snapshot.stats.PresentRefreshCount, snapshot.stats.SyncRefreshCount,
                      SUCCEEDED(snapshot.presentCountHr) ? snapshot.lastPresentCount : 0u);
    } else {
        std::snprintf(text, sizeof(text), "unavailable hr=0x%08X lastPresentCount=%u",
                      static_cast<unsigned>(snapshot.statsHr),
                      SUCCEEDED(snapshot.presentCountHr) ? snapshot.lastPresentCount : 0u);
    }
    return text;
}

const char* ExStyleFlags(unsigned long long exStyle, char* out, size_t capacity) {
    size_t length = 0;
    const auto add = [&](unsigned long long bit, char letter) {
        if ((exStyle & bit) != 0 && length + 1 < capacity) {
            out[length++] = letter;
        }
    };
    add(WS_EX_TOPMOST, 'T');
    add(WS_EX_LAYERED, 'L');
    add(WS_EX_TRANSPARENT, 'X');
    add(WS_EX_NOACTIVATE, 'N');
    add(WS_EX_TOOLWINDOW, 'W');
    out[length] = '\0';
    return out;
}

std::string DescribeAbove(const WindowState& state) {
    std::string text;
    for (size_t i = 0; i < state.aboveShown; ++i) {
        const AboveWindow& entry = state.above[i];
        char flags[8] = {};
        char item[192];
        std::snprintf(item, sizeof(item), "%s{class=%s exe=%s ex=0x%llX/%s rect=%ld,%ld-%ld,%ld}", i ? " " : "",
                      entry.className, entry.exeName, entry.exStyle, ExStyleFlags(entry.exStyle, flags, sizeof(flags)),
                      static_cast<long>(entry.rect.left), static_cast<long>(entry.rect.top),
                      static_cast<long>(entry.rect.right), static_cast<long>(entry.rect.bottom));
        text += item;
    }
    return text.empty() ? std::string("none") : text;
}

const char* FirstChangeKind(const StallStep& end, const SharedDisplayTiming* timing, uint64_t sequence) {
    if (end.generationChanged) {
        return "stream-reset";
    }
    if (!timing || sequence == 0) {
        return "unknown";
    }
    const auto& sample = timing->samples[(sequence - 1) & (DISPLAY_TIMING_RING_SIZE - 1)];
    if (sample.sequence.load(std::memory_order_acquire) != sequence) {
        return "unknown";
    }
    return (sample.flags.load(std::memory_order_relaxed) & kDisplayTimingComposed) != 0 ? "composed" : "own-flip";
}

void LogStart(IDXGISwapChain* chain, const StallStep& step, const PresentResultTally& tally, UINT sync, UINT flags,
              const DisplayObservation& observation, int64_t nowUs, const Snapshot& snapshot) {
    char results[160];
    tally.Describe(results, sizeof(results));
    char hwComposition[24] = "unknown";
    if (snapshot.hwCompositionKnown) {
        std::snprintf(hwComposition, sizeof(hwComposition), "0x%X", snapshot.hwComposition);
    }
    const unsigned long long episode = static_cast<unsigned long long>(step.episode);
    HookLogImportant("[SCANOUT STALL] ep=%llu start sc=%p ageUs=%lld presentsSinceChange=%u display(seq=%llu gen=%llu "
                     "publishAgeUs=%lld) lastPresent(sync=%u flags=0x%X) results=[%s] frameStats(%s) fullscreen=%d "
                     "hwComposition=%s",
                     episode, chain, static_cast<long long>(step.ageUs), step.presentsSinceChange,
                     static_cast<unsigned long long>(observation.sequence),
                     static_cast<unsigned long long>(observation.generation),
                     static_cast<long long>(observation.publishUs > 0 ? nowUs - observation.publishUs : -1), sync,
                     flags, results, DescribeFrameStats(snapshot).c_str(),
                     snapshot.fullscreenKnown ? (snapshot.fullscreen ? 1 : 0) : -1, hwComposition);
    if (snapshot.descKnown) {
        HookLogImportant("[SCANOUT STALL] ep=%llu swapchain fmt=%u buffers=%u effect=%u flags=0x%X size=%ux%u windowed=%d "
                         "hwnd=%p",
                         episode, static_cast<unsigned>(snapshot.desc.BufferDesc.Format), snapshot.desc.BufferCount,
                         static_cast<unsigned>(snapshot.desc.SwapEffect), snapshot.desc.Flags,
                         snapshot.desc.BufferDesc.Width, snapshot.desc.BufferDesc.Height,
                         snapshot.desc.Windowed ? 1 : 0, snapshot.desc.OutputWindow);
    }
    const WindowState& window = snapshot.window;
    if (window.known) {
        char exFlags[8] = {};
        HookLogImportant("[SCANOUT STALL] ep=%llu window visible=%d iconic=%d cloaked=0x%lX foreground=%d "
                         "rect=%ld,%ld-%ld,%ld client=%ldx%ld monitor=%ld,%ld-%ld,%ld ex=0x%llX/%s flipBlockers=%s",
                         episode, window.visible ? 1 : 0, window.iconic ? 1 : 0, static_cast<unsigned long>(window.cloaked),
                         window.foreground ? 1 : 0, static_cast<long>(window.window.left),
                         static_cast<long>(window.window.top), static_cast<long>(window.window.right),
                         static_cast<long>(window.window.bottom),
                         static_cast<long>(window.client.right - window.client.left),
                         static_cast<long>(window.client.bottom - window.client.top),
                         static_cast<long>(window.monitor.left), static_cast<long>(window.monitor.top),
                         static_cast<long>(window.monitor.right), static_cast<long>(window.monitor.bottom),
                         window.exStyle, ExStyleFlags(window.exStyle, exFlags, sizeof(exFlags)), window.blockers);
        HookLogImportant("[SCANOUT STALL] ep=%llu above total=%u shown=%zu %s", episode, window.aboveTotal,
                         window.aboveShown, DescribeAbove(window).c_str());
    } else {
        HookLogImportant("[SCANOUT STALL] ep=%llu window unknown (no window handle on the swapchain)", episode);
    }
}

void LogDiff(const StallStep& step, const PresentResultTally& tally, const Snapshot& snapshot, bool windowChanged,
             bool startStatsKnown, UINT startRefreshCount) {
    char results[160];
    tally.Describe(results, sizeof(results));
    const long refreshDelta =
        startStatsKnown && SUCCEEDED(snapshot.statsHr)
            ? static_cast<long>(snapshot.stats.PresentRefreshCount) - static_cast<long>(startRefreshCount)
            : -1;
    HookLogImportant("[SCANOUT STALL] ep=%llu still-stalled ageUs=%lld presentsSinceChange=%u results=[%s] "
                     "refreshCountSinceStart=%ld foreground=%d cloaked=0x%lX above=%u windowChanged=%d",
                     static_cast<unsigned long long>(step.episode), static_cast<long long>(step.ageUs),
                     step.presentsSinceChange, results, refreshDelta, snapshot.window.foreground ? 1 : 0,
                     static_cast<unsigned long>(snapshot.window.cloaked), snapshot.window.aboveTotal,
                     windowChanged ? 1 : 0);
}

void LogEnd(const StallStep& step, const PresentResultTally& tally, const Snapshot& snapshot, bool windowChanged,
            const char* firstChange, uint64_t suppressedEpisodes) {
    char results[160];
    tally.Describe(results, sizeof(results));
    HookLogImportant("[SCANOUT STALL] ep=%llu end durationUs=%lld presents=%u firstChange=%s sequenceAdvance=%llu "
                     "results=[%s] windowChanged=%d foreground=%d cloaked=0x%lX above=%u episodesNotLogged=%llu",
                     static_cast<unsigned long long>(step.episode), static_cast<long long>(step.episodeUs),
                     step.episodePresents, firstChange, static_cast<unsigned long long>(step.sequenceAdvance), results,
                     windowChanged ? 1 : 0, snapshot.window.foreground ? 1 : 0,
                     static_cast<unsigned long>(snapshot.window.cloaked), snapshot.window.aboveTotal,
                     static_cast<unsigned long long>(suppressedEpisodes));
}

}  // namespace

void ArmFirstScreenChange(uint64_t handoff, IDXGISwapChain* swapChain) {
    DisplayObservation baseline;
    int64_t nowUs = 0;
    ReadDisplay(baseline, nowUs, nullptr);
    std::lock_guard<std::mutex> lock(g_Mutex);
    g_Watch.Arm(handoff, Id(swapChain), nowUs, baseline);
}

void OnPresentReturned(IDXGISwapChain* swapChain, UINT syncInterval, UINT flags, HRESULT result) {
    if (!swapChain || HookIsShuttingDown()) {
        return;
    }
    DisplayObservation observation;
    int64_t nowUs = 0;
    const SharedDisplayTiming* timing = nullptr;
    if (!ReadDisplay(observation, nowUs, &timing)) {
        return;
    }

    StallStep step;
    FirstChange first;
    PresentResultTally tally;
    bool detailed = false;
    uint64_t episodesNotLogged = 0;
    uint32_t startSignature = 0;
    bool startStatsKnown = false;
    UINT startRefreshCount = 0;
    {
        // A busy lock drops this present's observation: a diagnostic never delays a Present.
        std::unique_lock<std::mutex> lock(g_Mutex, std::try_to_lock);
        if (!lock.owns_lock()) {
            return;
        }
        if (swapChain != g_LastChain) {
            g_LastChain = swapChain;
            g_Tally.Reset();
            g_Tracker.NoteNewSwapchain();
        }
        g_Tally.Note(static_cast<int32_t>(result));
        g_LastSync = syncInterval;
        g_LastFlags = flags;
        first = g_Watch.OnPresent(Id(swapChain), nowUs, observation);
        step = g_Tracker.OnPresent(nowUs, observation);
        if (step.event != StallEvent::kNone) {
            tally = g_Tally;
            if (step.event == StallEvent::kStart) {
                g_EpisodeDetailed = step.episode <= kDetailedEpisodes || step.episode % kDetailedEpisodes == 0;
            }
            detailed = g_EpisodeDetailed;
            startSignature = g_StartSignature;
            startStatsKnown = g_StartStatsKnown;
            startRefreshCount = g_StartRefreshCount;
            episodesNotLogged = step.episode > kDetailedEpisodes ? step.episode - kDetailedEpisodes : 0;
        }
    }

    if (first.reached) {
        HookLogImportant("[SL STARTUP] handoff=%llu sc=%p firstScreenChange sinceHandoffUs=%lld presentsBefore=%u%s",
                         static_cast<unsigned long long>(first.handoff), swapChain,
                         static_cast<long long>(first.sinceHandoffUs), first.presentsBefore,
                         first.generationChanged ? " (display stream reset)" : "");
    }
    if (step.event == StallEvent::kNone || !detailed) {
        return;
    }

    Snapshot snapshot;
    Collect(swapChain, step.event == StallEvent::kStart, snapshot);
    switch (step.event) {
        case StallEvent::kStart:
            LogStart(swapChain, step, tally, syncInterval, flags, observation, nowUs, snapshot);
            {
                std::lock_guard<std::mutex> lock(g_Mutex);
                g_StartSignature = snapshot.signature;
                g_StartStatsKnown = SUCCEEDED(snapshot.statsHr);
                g_StartRefreshCount = snapshot.stats.PresentRefreshCount;
            }
            break;
        case StallEvent::kDiff:
            LogDiff(step, tally, snapshot, snapshot.signature != startSignature, startStatsKnown, startRefreshCount);
            break;
        case StallEvent::kEnd:
            LogEnd(step, tally, snapshot, snapshot.signature != startSignature,
                   FirstChangeKind(step, timing, observation.sequence), episodesNotLogged);
            break;
        case StallEvent::kNone:
            break;
    }
}

}  // namespace ce::scanout_stall
