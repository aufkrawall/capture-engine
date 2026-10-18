#include "dx11_hook_internal.h"

#include <d3d11_4.h>

#include "common/logging/log_meter.h"
#include "hook/pacing/prerender_lookback.h"

// CE's `cpu_prerender_limit` for D3D10/11 swapchains, called from the DXGI Present detours.
//
// D3D11, limits 1-6: a marker on an ID3D11Fence, signaled right after each forwarded Present
// (D3D11PrerenderAfterPresent); before the next Present the thread waits in the kernel for the
// marker `limit` presents back (prerender_lookback.h). d3d11.dll implements
// ID3D11DeviceContext4::Signal as NDXGI::CDevice::SignalFence, which flushes the context before the
// kernel signal (10.0.26100, disassembled 2026-10-18), so the marker goes after Present, where the
// context is empty: one kernel signal and no extra submission. A wait first flushes what the
// frame recorded so far, so the GPU has it queued behind the frame it is finishing. Limit 0
// signals and waits before the Present (serial; the flush is the point).
//
// Until 2026-10-18 every D3D11 limit ran an event-query ring polled with GetData + SwitchToThread:
// the present thread spun for the whole wait (BioShock Infinite, 20261018_005433: 0.05-2 ms per
// present, charged to the startup_routing stage and absent from the perf CSV). The ring is the
// fallback for a device without fences (no ID3D11Device5 / ID3D11DeviceContext4, or CreateFence
// refused).
//
// D3D10 has no fences: limit 0 keeps its event query; limits 1-6 set DXGI's device frame latency,
// which the runtime enforces with its own wait. Until the second fix of 2026-10-18 none of it ran:
// both Present detours admitted D3D11 swapchains only, and a D3D10 swapchain on Windows 10+ also
// answers ID3D11Device, which was asked for first. The swapchain's classification picks the path.
namespace {

enum class Path : uint8_t { kNone, kFence, kQuery };

const char* PathName(Path path) {
    return path == Path::kFence ? "fence" : path == Path::kQuery ? "query" : "none";
}

struct State11 {
    ID3D11Device* device = nullptr;            // held: the swapchain's device
    ID3D11DeviceContext* context = nullptr;    // held: its immediate context
    ID3D11DeviceContext4* context4 = nullptr;  // held, fence path
    ID3D11Fence* fence = nullptr;              // held, fence path
    HANDLE event = nullptr;                    // fence path
    uint64_t signaled = 0;                     // value of the last marker
    std::vector<ID3D11Query*> queries;         // query path ring
    uint64_t queryFrame = 0;
    Path path = Path::kNone;
};

struct State10 {
    ID3D10Device* device = nullptr;  // held
    ID3D10Query* serialQuery = nullptr;
};

// One diagnostic window of the present thread's waits ([D3D11 PRERENDER], every 10 s while active).
struct Window {
    int64_t startUs = 0;
    uint32_t presents = 0;
    uint32_t blocked = 0;
    int64_t waitUs = 0;
    int64_t maxWaitUs = 0;
    uint64_t markers = 0;
};
constexpr int64_t kWindowUs = 10'000'000;

struct WindowReport {
    bool due = false;
    Window window;
    int64_t elapsedUs = 0;
    Path path = Path::kNone;
    int limit = 0;
};

std::mutex g_mutex;
State11 g_11;     // g_mutex
State10 g_10;     // g_mutex
Window g_window;  // g_mutex
std::atomic<uint64_t> g_totalPresents{0};
std::atomic<uint64_t> g_totalBlocked{0};
#ifdef CE_FLOW_TEST
std::atomic<bool> g_forceQueryRing{false};
#endif

// The device whose marker this thread's current Present owes (set before it, cleared after it).
thread_local const ID3D11Device* t_armedDevice = nullptr;
// The last wait on this thread, for the frame's perf CSV row.
thread_local int32_t t_lastWaitUs = 0;

template <class T>
void ReleaseHeld(T*& object) {
    if (object) {
        object->Release();
        object = nullptr;
    }
}

void ReleaseState11Locked() {
    for (ID3D11Query* query : g_11.queries) {
        ReleaseHeld(query);
    }
    g_11.queries.clear();
    ReleaseHeld(g_11.fence);
    ReleaseHeld(g_11.context4);
    ReleaseHeld(g_11.context);
    ReleaseHeld(g_11.device);
    if (g_11.event) {
        CloseHandle(g_11.event);
        g_11.event = nullptr;
    }
    g_11.signaled = 0;
    g_11.queryFrame = 0;
    g_11.path = Path::kNone;
}

// The fence path's objects, or why the device has none.
const char* BindFenceLocked() {
#ifdef CE_FLOW_TEST
    if (g_forceQueryRing.load(std::memory_order_relaxed)) {
        return "forced by the flow test";
    }
#endif
    ID3D11Device5* device5 = nullptr;
    if (FAILED(g_11.device->QueryInterface(IID_PPV_ARGS(&device5))) || !device5) {
        return "no ID3D11Device5";
    }
    const HRESULT created = device5->CreateFence(0, D3D11_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_11.fence));
    device5->Release();
    if (FAILED(created) || !g_11.fence) {
        g_11.fence = nullptr;
        return "CreateFence refused";
    }
    if (FAILED(g_11.context->QueryInterface(IID_PPV_ARGS(&g_11.context4))) || !g_11.context4) {
        g_11.context4 = nullptr;
        ReleaseHeld(g_11.fence);
        return "no ID3D11DeviceContext4";
    }
    g_11.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!g_11.event) {
        ReleaseHeld(g_11.context4);
        ReleaseHeld(g_11.fence);
        return "CreateEvent failed";
    }
    return nullptr;
}

void BindQueryRingLocked() {
    for (int i = 0; i < 16; ++i) {
        D3D11_QUERY_DESC desc = {};
        desc.Query = D3D11_QUERY_EVENT;
        ID3D11Query* query = nullptr;
        if (SUCCEEDED(g_11.device->CreateQuery(&desc, &query)) && query) {
            g_11.queries.push_back(query);
        }
    }
}

void RebindLocked(ID3D11Device* device) {
    ReleaseState11Locked();
    g_11.device = device;
    g_11.device->AddRef();
    g_11.device->GetImmediateContext(&g_11.context);
    if (!g_11.context) {
        HookLogImportant("DX11: Prerender stream has no immediate context for device=%p; limit not applied", device);
        return;
    }
    const char* noFence = BindFenceLocked();
    if (!noFence) {
        g_11.path = Path::kFence;
        HookLogImportant("DX11: Prerender stream bound device=%p path=fence (marker after each Present, kernel wait)",
                         device);
        return;
    }
    BindQueryRingLocked();
    g_11.path = g_11.queries.empty() ? Path::kNone : Path::kQuery;
    HookLogImportant("DX11: Prerender stream bound device=%p path=%s (fence unavailable: %s; queries=%zu)", device,
                     PathName(g_11.path), noFence, g_11.queries.size());
}

// Blocks until the fence reaches `target`. True when it had to wait.
bool WaitFenceLocked(uint64_t target, bool flushFirst, int64_t* waitedUs) {
    if (g_11.fence->GetCompletedValue() >= target) {
        return false;
    }
    if (g_11.device->GetDeviceRemovedReason() != S_OK) {
        return false;  // a removed device completes nothing more
    }
    static std::atomic<int> s_failureLogs{0};
    const int64_t startUs = PerfLogger::GetQpcUs();
    if (flushFirst) {
        g_11.context->Flush();
    }
    const HRESULT armed = g_11.fence->SetEventOnCompletion(target, g_11.event);
    if (SUCCEEDED(armed)) {
        const DWORD result = WaitForSingleObject(g_11.event, INFINITE);
        if (result != WAIT_OBJECT_0 && s_failureLogs.fetch_add(1, std::memory_order_relaxed) < 8) {
            HookLogImportant("DX11: Prerender fence wait failed result=%lu error=%lu target=%llu", result,
                             GetLastError(), static_cast<unsigned long long>(target));
        }
    } else if (s_failureLogs.fetch_add(1, std::memory_order_relaxed) < 8) {
        HookLogImportant("DX11: Prerender SetEventOnCompletion failed hr=0x%08X target=%llu", armed,
                         static_cast<unsigned long long>(target));
    }
    *waitedUs = PerfLogger::GetQpcUs() - startUs;
    return true;
}

// The pre-2026-10-18 ring, for devices without fences: GetData flushes when the query is unsubmitted.
bool WaitQueryRingLocked(int limit, int64_t* waitedUs) {
    const size_t size = g_11.queries.size();
    ID3D11Query* current = g_11.queries[g_11.queryFrame % size];
    g_11.context->End(current);
    ID3D11Query* waitFor = nullptr;
    if (limit == 0) {
        waitFor = current;
    } else {
        const auto lookback = static_cast<uint64_t>(ce::prerender_lookback::ClampLookback(limit));
        if (g_11.queryFrame >= lookback) {
            waitFor = g_11.queries[(g_11.queryFrame - lookback) % size];
        }
    }
    ++g_11.queryFrame;
    if (!waitFor || g_11.context->GetData(waitFor, nullptr, 0, 0) != S_FALSE) {
        return false;
    }
    const int64_t startUs = PerfLogger::GetQpcUs();
    while (g_11.context->GetData(waitFor, nullptr, 0, 0) == S_FALSE) {
        SwitchToThread();
    }
    *waitedUs = PerfLogger::GetQpcUs() - startUs;
    return true;
}

WindowReport NoteWindowLocked(int limit, bool blocked, int64_t waitedUs) {
    const int64_t nowUs = PerfLogger::GetQpcUs();
    if (g_window.presents == 0 && g_window.startUs == 0) {
        g_window.startUs = nowUs;
    }
    ++g_window.presents;
    if (blocked) {
        ++g_window.blocked;
        g_window.waitUs += waitedUs;
        g_window.maxWaitUs = (std::max)(g_window.maxWaitUs, waitedUs);
    }
    WindowReport report;
    if (nowUs - g_window.startUs >= kWindowUs) {
        report = {true, g_window, nowUs - g_window.startUs, g_11.path, limit};
        g_window = {};
        g_window.startUs = nowUs;
    }
    return report;
}

void LogWindow(const WindowReport& report) {
    if (!report.due || report.window.presents == 0) {
        return;
    }
    const Window& w = report.window;
    HookLogImportant(
        "[D3D11 PRERENDER] window=%lldms path=%s limit=%d presents=%u blocked=%u wait(meanUs=%.0f maxUs=%lld "
        "totalMs=%.1f) markers=%llu",
        static_cast<long long>(report.elapsedUs / 1000), PathName(report.path), report.limit, w.presents, w.blocked,
        w.blocked != 0 ? static_cast<double>(w.waitUs) / w.blocked : 0.0, static_cast<long long>(w.maxWaitUs),
        static_cast<double>(w.waitUs) / 1000.0, static_cast<unsigned long long>(w.markers));
}

void BeforePresent11(ID3D11Device* device, int limit) {
    WindowReport report;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_11.device != device) {
            RebindLocked(device);
        }
        int64_t waitedUs = 0;
        bool blocked = false;
        if (g_11.path == Path::kFence) {
            if (limit == 0) {
                const uint64_t value = g_11.signaled + 1;
                if (SUCCEEDED(g_11.context4->Signal(g_11.fence, value))) {
                    g_11.signaled = value;
                    ++g_window.markers;
                    blocked = WaitFenceLocked(value, false, &waitedUs);
                }
            } else {
                const uint64_t target = ce::prerender_lookback::WaitTarget(g_11.signaled, limit);
                if (target != 0) {
                    blocked = WaitFenceLocked(target, true, &waitedUs);
                }
                t_armedDevice = device;
            }
        } else if (g_11.path == Path::kQuery) {
            blocked = WaitQueryRingLocked(limit, &waitedUs);
        }
        t_lastWaitUs = static_cast<int32_t>((std::min)(waitedUs, static_cast<int64_t>(INT32_MAX)));
        g_totalPresents.fetch_add(1, std::memory_order_relaxed);
        dx11_hook_g_DiagPrerenderFrames.fetch_add(1, std::memory_order_relaxed);
        if (blocked) {
            g_totalBlocked.fetch_add(1, std::memory_order_relaxed);
            dx11_hook_g_DiagPrerenderWaits.fetch_add(1, std::memory_order_relaxed);
        }
        report = NoteWindowLocked(limit, blocked, waitedUs);
    }
    LogWindow(report);
}

void BeforePresent10(ID3D10Device* device, int limit) {
    if (limit != 0) {
        // DXGI's device frame latency: the runtime holds the producer `limit` frames ahead itself.
        IDXGIDevice1* dxgi = nullptr;
        if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dxgi))) || !dxgi) {
            return;
        }
        UINT current = 0;
        const auto wanted = static_cast<UINT>(ce::prerender_lookback::ClampLookback(limit));
        if (SUCCEEDED(dxgi->GetMaximumFrameLatency(&current)) && current != wanted) {
            const HRESULT hr = dxgi->SetMaximumFrameLatency(wanted);
            static ce::log_meter::ChangeGate s_gate;
            if (const auto verdict = s_gate.Observe(ce::log_meter::FieldKey(device, current, wanted, hr))) {
                HookLogImportant("D3D10: Prerender limit as device frame latency %u -> %u hr=0x%08X (device=%p)%s",
                                 current, wanted, hr, device, ce::log_meter::SuppressedNote(verdict.suppressed).c_str());
            }
        }
        dxgi->Release();
        return;
    }
    // DXGI has no zero-depth latency: serialize on a native event query.
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_10.device != device) {
        ReleaseHeld(g_10.serialQuery);
        ReleaseHeld(g_10.device);
        g_10.device = device;
        g_10.device->AddRef();
        HookLogImportant("D3D10: Serial prerender query rebound to device=%p", device);
    }
    if (!g_10.serialQuery) {
        D3D10_QUERY_DESC desc = {};
        desc.Query = D3D10_QUERY_EVENT;
        device->CreateQuery(&desc, &g_10.serialQuery);
    }
    if (!g_10.serialQuery) {
        return;
    }
    g_10.serialQuery->End();
    device->Flush();
    while (g_10.serialQuery->GetData(nullptr, 0, 0) == S_FALSE) {
        SwitchToThread();
    }
    dx11_hook_g_DiagPrerenderFrames.fetch_add(1, std::memory_order_relaxed);
}

bool TryBeforePresent11(IDXGISwapChain* swapChain, int depth) {
    ID3D11Device* device = nullptr;
    if (FAILED(swapChain->GetDevice(IID_PPV_ARGS(&device))) || !device) {
        return false;
    }
    BeforePresent11(device, depth);
    device->Release();
    return true;
}

bool TryBeforePresent10(IDXGISwapChain* swapChain, int depth) {
    ID3D10Device* device = nullptr;
    if (FAILED(swapChain->GetDevice(IID_PPV_ARGS(&device))) || !device) {
        return false;
    }
    BeforePresent10(device, depth);
    device->Release();
    return true;
}

}  // namespace

void ApplyPrerenderLimit(IDXGISwapChain* pSwapChain, float limit, bool d3d10Swapchain) {
    if (limit < 0.0f || !pSwapChain) {
        return;
    }
    const int depth = static_cast<int>(limit);
    // A D3D10 swapchain on Windows 10+ also answers ID3D11Device (D3D10-on-D3D11); asking for
    // ID3D11Device first sent it down the D3D11 path. The other device is the fallback only.
    const bool applied10 = d3d10Swapchain && TryBeforePresent10(pSwapChain, depth);
    const bool applied11 = !applied10 && TryBeforePresent11(pSwapChain, depth);
    const bool fallback10 = !applied10 && !applied11 && !d3d10Swapchain && TryBeforePresent10(pSwapChain, depth);
    if (applied10 || applied11 || fallback10) {
        static ce::log_meter::ChangeGate s_route;
        const char* route = applied11 ? "d3d11" : "d3d10";
        if (const auto verdict =
                s_route.Observe(ce::log_meter::FieldKey(pSwapChain, d3d10Swapchain, applied11, depth))) {
            HookLogImportant("D3D10/11: Prerender limit %d on the %s path (swapchain=%p classified=%s)%s", depth,
                             route, pSwapChain, d3d10Swapchain ? "d3d10" : "d3d11",
                             ce::log_meter::SuppressedNote(verdict.suppressed).c_str());
        }
        return;
    }
    static std::atomic<int> s_unavailableLogs{0};
    if (s_unavailableLogs.fetch_add(1, std::memory_order_relaxed) < 5) {
        HookLogImportant("D3D10/11: Prerender limit has no D3D10/11 device for swapchain=%p", pSwapChain);
    }
}

bool D3D11PrerenderSignalPending() {
    return t_armedDevice != nullptr;
}

void D3D11PrerenderAfterPresent() {
    const ID3D11Device* armed = t_armedDevice;
    if (!armed) {
        return;
    }
    t_armedDevice = nullptr;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_11.device != armed || g_11.path != Path::kFence) {
        return;  // rebound meanwhile: the new stream starts with its own first marker
    }
    const uint64_t value = g_11.signaled + 1;
    const HRESULT hr = g_11.context4->Signal(g_11.fence, value);
    if (SUCCEEDED(hr)) {
        g_11.signaled = value;
        ++g_window.markers;
        return;
    }
    static std::atomic<int> s_signalLogs{0};
    if (s_signalLogs.fetch_add(1, std::memory_order_relaxed) < 8) {
        HookLogImportant("DX11: Prerender marker Signal failed hr=0x%08X value=%llu", hr,
                         static_cast<unsigned long long>(value));
    }
}

int32_t TakeD3D11PrerenderWaitUs() {
    const int32_t waitedUs = t_lastWaitUs;
    t_lastWaitUs = 0;
    return waitedUs;
}

void ReleaseD3D11PrerenderState() {
    std::lock_guard<std::mutex> lock(g_mutex);
    ReleaseState11Locked();
    ReleaseHeld(g_10.serialQuery);
    ReleaseHeld(g_10.device);
    g_window = {};
}

#ifdef CE_FLOW_TEST
// The detours' two halves around a present the test makes itself (a Flush stands in for it).
extern "C" __declspec(dllexport) void CEFlow_D3D11PrerenderBefore(ID3D11Device* device, int limit) {
    BeforePresent11(device, limit);
}
// The detours' entry point itself, with the swapchain's classification.
extern "C" __declspec(dllexport) void CEFlow_ApplyPrerenderLimit(IDXGISwapChain* swapChain, float limit,
                                                                 bool d3d10Swapchain) {
    ApplyPrerenderLimit(swapChain, limit, d3d10Swapchain);
}
extern "C" __declspec(dllexport) void CEFlow_D3D11PrerenderAfter() {
    D3D11PrerenderAfterPresent();
}
extern "C" __declspec(dllexport) void CEFlow_D3D11PrerenderRelease() {
    ReleaseD3D11PrerenderState();
}
extern "C" __declspec(dllexport) void CEFlow_D3D11PrerenderForceQueryRing(bool force) {
    g_forceQueryRing.store(force, std::memory_order_relaxed);
}
// path (0 none, 1 fence, 2 query), markers signaled, fence completed value, presents, blocked
// presents, armed (a marker owed by this thread).
extern "C" __declspec(dllexport) uint32_t CEFlow_GetD3D11PrerenderStats(uint64_t* values, uint32_t count) {
    std::lock_guard<std::mutex> lock(g_mutex);
    const uint64_t stats[] = {static_cast<uint64_t>(g_11.path), g_11.signaled,
                              g_11.fence ? g_11.fence->GetCompletedValue() : 0,
                              g_totalPresents.load(std::memory_order_relaxed),
                              g_totalBlocked.load(std::memory_order_relaxed), t_armedDevice != nullptr ? 1u : 0u};
    const uint32_t n = count < 6 ? count : 6;
    for (uint32_t i = 0; i < n; ++i) {
        values[i] = stats[i];
    }
    return n;
}
#endif
