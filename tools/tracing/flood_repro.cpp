// flood_repro: does a window-manager call loop on one thread delay the compositor showing a newly created
// flip-model swapchain? Mirrors GTA V Enhanced's save load: a borderless monitor-sized window presents with
// swapchain A, one non-GUI thread spins on user32/imm32 calls, the window switches to a new swapchain B (as a
// frame generation enable does), the spin stops some time later. Standalone diagnostic, not part of the product:
// tools/tracing/run_flood_repro.py builds it with the project's clang into build/tracing/ and runs it under ETW.
// Result (2026-11-13, llm-wiki/display-stall-trace.md): only ClipCursor creates compositor work, one dwm.exe batch
// per call; above DWM's ~50k batches/s the new swapchain waits until the backlog has drained.
//
// usage: flood_repro.exe <mode> [spinBeforeMs] [spinAfterMs] [rateCyclesPerSecond] [maxPostMs]
//   mode: none | clip | layout | ime | point | all   (comma lists allowed, e.g. clip,layout)
// Prints QPC timestamps (same clock as an ETW session started with -ct perf) for every phase, the spinner's
// cycles per 100 ms, call failures, and the first successful frame statistics of swapchain B. Esc aborts.
#include <windows.h>
#include <imm.h>
#include <d3d11.h>
#include <dxgi1_5.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

namespace {

enum : unsigned { kClip = 1, kLayout = 2, kIme = 4, kPoint = 8 };
constexpr const char* kCallNames[4] = {"clip", "layout", "ime", "point"};

int64_t g_freq = 0;
HWND g_hwnd = nullptr;
std::atomic<bool> g_abort{false};

int64_t Qpc() {
    LARGE_INTEGER value;
    QueryPerformanceCounter(&value);
    return value.QuadPart;
}

int64_t MsToTicks(int64_t ms) { return ms * g_freq / 1000; }

unsigned ParseMode(const char* text) {
    unsigned bits = 0;
    char buffer[128];
    strncpy(buffer, text, sizeof(buffer) - 1);
    buffer[sizeof(buffer) - 1] = 0;
    for (char* part = strtok(buffer, ","); part; part = strtok(nullptr, ",")) {
        if (!strcmp(part, "clip")) bits |= kClip;
        else if (!strcmp(part, "layout")) bits |= kLayout;
        else if (!strcmp(part, "ime")) bits |= kIme;
        else if (!strcmp(part, "point")) bits |= kPoint;
        else if (!strcmp(part, "all")) bits |= kClip | kLayout | kIme | kPoint;
        else if (strcmp(part, "none") != 0) {
            fprintf(stderr, "unknown mode part '%s'\n", part);
            exit(2);
        }
    }
    return bits;
}

struct Spinner {
    unsigned bits = 0;
    int rate = 0;
    int64_t origin = 0;
    std::atomic<bool> stop{false};
    std::vector<uint32_t> buckets = std::vector<uint32_t>(1200, 0);  // 100 ms each from origin
    uint64_t cycles = 0;
    uint64_t failures[4] = {};
    DWORD firstError[4] = {};
    int64_t firstCycle = 0;
    int64_t lastCycle = 0;

    void Fail(int call) {
        if (failures[call]++ == 0) firstError[call] = GetLastError();
    }

    void Run() {
        SetThreadDescription(GetCurrentThread(), L"flood_repro spinner");
        const HKL layout = GetKeyboardLayout(0);
        const int64_t bucketTicks = MsToTicks(100);
        const int64_t period = rate > 0 ? g_freq / rate : 0;
        int64_t next = Qpc();
        while (!stop.load(std::memory_order_relaxed)) {
            if (bits & kClip) {
                RECT rect;
                GetWindowRect(g_hwnd, &rect);
                SetLastError(0);
                if (!ClipCursor(&rect)) Fail(0);
            }
            if (bits & kLayout) {
                SetLastError(0);
                if (!ActivateKeyboardLayout(layout, 0)) Fail(1);
            }
            if (bits & kIme) {
                SetLastError(0);
                HIMC context = ImmGetContext(g_hwnd);
                DWORD conversion = 0, sentence = 0;
                if (!context || !ImmGetConversionStatus(context, &conversion, &sentence)) Fail(2);
                if (context) ImmReleaseContext(g_hwnd, context);
            }
            if (bits & kPoint) {
                POINT point;
                GetCursorPos(&point);
                WindowFromPoint(point);
                GUITHREADINFO info = {};
                info.cbSize = sizeof(info);
                SetLastError(0);
                if (!GetGUIThreadInfo(0, &info)) Fail(3);
            }
            const int64_t now = Qpc();
            if (!firstCycle) firstCycle = now;
            lastCycle = now;
            ++cycles;
            const int64_t bucket = (now - origin) / bucketTicks;
            if (bucket >= 0 && bucket < static_cast<int64_t>(buckets.size())) ++buckets[bucket];
            if (period) {
                next += period;
                while (Qpc() < next) YieldProcessor();
            }
        }
        if (bits & kClip) ClipCursor(nullptr);
    }
};

LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_KEYDOWN && wparam == VK_ESCAPE) g_abort = true;
    if (message == WM_CLOSE) {
        g_abort = true;
        return 0;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

bool ForceForeground(HWND hwnd) {
    if (SetForegroundWindow(hwnd) && GetForegroundWindow() == hwnd) return true;
    const HWND current = GetForegroundWindow();
    const DWORD theirs = current ? GetWindowThreadProcessId(current, nullptr) : 0;
    const DWORD ours = GetCurrentThreadId();
    if (theirs && theirs != ours) AttachThreadInput(ours, theirs, TRUE);
    BringWindowToTop(hwnd);
    SetForegroundWindow(hwnd);
    if (theirs && theirs != ours) AttachThreadInput(ours, theirs, FALSE);
    return GetForegroundWindow() == hwnd;
}

void Pump() {
    MSG message;
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}

struct Renderer {
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    IDXGIFactory2* factory = nullptr;
    UINT flags = 0;

    bool Init() {
        const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0};
        if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                     levels, 1, D3D11_SDK_VERSION, &device, nullptr, &context)))
            return false;
        IDXGIDevice* dxgiDevice = nullptr;
        IDXGIAdapter* adapter = nullptr;
        device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgiDevice));
        dxgiDevice->GetAdapter(&adapter);
        adapter->GetParent(__uuidof(IDXGIFactory2), reinterpret_cast<void**>(&factory));
        adapter->Release();
        dxgiDevice->Release();
        IDXGIFactory5* factory5 = nullptr;
        BOOL tearing = FALSE;
        if (SUCCEEDED(factory->QueryInterface(__uuidof(IDXGIFactory5), reinterpret_cast<void**>(&factory5)))) {
            factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing, sizeof(tearing));
            factory5->Release();
        }
        flags = tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
        return true;
    }

    IDXGISwapChain1* MakeChain(UINT width, UINT height) {
        DXGI_SWAP_CHAIN_DESC1 desc = {};
        desc.Width = width;
        desc.Height = height;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 3;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        desc.Flags = flags;
        IDXGISwapChain1* chain = nullptr;
        const HRESULT hr = factory->CreateSwapChainForHwnd(device, g_hwnd, &desc, nullptr, nullptr, &chain);
        if (FAILED(hr)) {
            printf("CreateSwapChainForHwnd failed hr=0x%08lX\n", static_cast<unsigned long>(hr));
            return nullptr;
        }
        factory->MakeWindowAssociation(g_hwnd, DXGI_MWA_NO_ALT_ENTER);
        return chain;
    }

    HRESULT Frame(IDXGISwapChain1* chain, const float color[4]) {
        ID3D11Texture2D* buffer = nullptr;
        ID3D11RenderTargetView* view = nullptr;
        chain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&buffer));
        device->CreateRenderTargetView(buffer, nullptr, &view);
        context->ClearRenderTargetView(view, color);
        view->Release();
        buffer->Release();
        return chain->Present(1, 0);
    }
};

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: flood_repro.exe <none|clip|layout|ime|point|all> [spinBeforeMs] [spinAfterMs] [rate]\n");
        return 2;
    }
    setvbuf(stdout, nullptr, _IOFBF, 1 << 16);
    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    g_freq = frequency.QuadPart;
    const unsigned bits = ParseMode(argv[1]);
    const int preMs = 1500;
    const int spinBeforeMs = argc > 2 ? atoi(argv[2]) : 2000;
    const int spinAfterMs = argc > 3 ? atoi(argv[3]) : 3000;
    const int rate = argc > 4 ? atoi(argv[4]) : 0;
    const int postMs = argc > 5 ? atoi(argv[5]) : 2500;  // upper bound; ends 1 s after B is shown

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    WNDCLASSW windowClass = {};
    windowClass.lpfnWndProc = WindowProc;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));  // IDC_ARROW
    windowClass.lpszClassName = L"FloodReproWindow";
    RegisterClassW(&windowClass);
    MONITORINFO monitor = {};
    monitor.cbSize = sizeof(monitor);
    GetMonitorInfoW(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY), &monitor);
    const RECT area = monitor.rcMonitor;
    const UINT width = static_cast<UINT>(area.right - area.left);
    const UINT height = static_cast<UINT>(area.bottom - area.top);
    g_hwnd = CreateWindowExW(WS_EX_APPWINDOW, windowClass.lpszClassName, L"flood repro", WS_POPUP | WS_VISIBLE,
                             area.left, area.top, static_cast<int>(width), static_cast<int>(height), nullptr, nullptr,
                             windowClass.hInstance, nullptr);
    const bool foreground = ForceForeground(g_hwnd);
    Pump();

    Renderer renderer;
    if (!renderer.Init()) {
        printf("D3D11CreateDevice failed\n");
        return 1;
    }
    const float colorA[4] = {0.05f, 0.08f, 0.25f, 1.0f};
    const float colorB[4] = {0.05f, 0.25f, 0.08f, 1.0f};
    IDXGISwapChain1* chain = renderer.MakeChain(width, height);
    if (!chain) return 1;

    const int64_t origin = Qpc();
    printf("repro pid=%lu mode=%s bits=0x%X size=%ux%u freq=%lld origin=%lld foreground=%d tearing=%d rate=%d\n",
           GetCurrentProcessId(), argv[1], bits, width, height, static_cast<long long>(g_freq),
           static_cast<long long>(origin), foreground ? 1 : 0, renderer.flags ? 1 : 0, rate);

    Spinner spinner;
    spinner.bits = bits;
    spinner.rate = rate;
    spinner.origin = origin;
    std::thread spinThread;

    const int64_t spinStartAt = origin + MsToTicks(preMs);
    const int64_t switchAt = spinStartAt + MsToTicks(spinBeforeMs);
    const int64_t spinStopAt = switchAt + MsToTicks(spinAfterMs);
    const int64_t endAt = spinStopAt + MsToTicks(postMs);
    int64_t spinStarted = 0, switched = 0, spinStopped = 0, firstStats = 0;
    UINT firstStatsCount = 0;
    HRESULT lastStatsResult = S_OK;
    unsigned presentsA = 0, presentsB = 0, presentFailures = 0;
    bool onB = false;

    while (!g_abort) {
        Pump();
        const int64_t now = Qpc();
        if (now >= endAt) break;
        if (spinStopped && firstStats && now >= firstStats + MsToTicks(1000) && now >= spinStopped + MsToTicks(1000))
            break;
        if (!spinStarted && now >= spinStartAt) {
            spinStarted = Qpc();
            printf("event spin_start qpc=%lld foreground=%d\n", static_cast<long long>(spinStarted),
                   GetForegroundWindow() == g_hwnd ? 1 : 0);
            if (bits) spinThread = std::thread([&spinner] { spinner.Run(); });
        }
        if (!onB && now >= switchAt) {
            renderer.context->ClearState();
            renderer.context->Flush();
            chain->Release();
            chain = nullptr;
            switched = Qpc();
            chain = renderer.MakeChain(width, height);
            if (!chain) break;
            onB = true;
            printf("event switch qpc=%lld created_qpc=%lld foreground=%d\n", static_cast<long long>(switched),
                   static_cast<long long>(Qpc()), GetForegroundWindow() == g_hwnd ? 1 : 0);
        }
        if (!spinStopped && now >= spinStopAt) {
            spinner.stop = true;
            if (spinThread.joinable()) spinThread.join();
            spinStopped = Qpc();
            printf("event spin_stop qpc=%lld\n", static_cast<long long>(spinStopped));
        }
        const HRESULT hr = renderer.Frame(chain, onB ? colorB : colorA);
        if (FAILED(hr)) ++presentFailures;
        if (onB) {
            ++presentsB;
            if (!firstStats) {
                DXGI_FRAME_STATISTICS stats = {};
                lastStatsResult = chain->GetFrameStatistics(&stats);
                if (SUCCEEDED(lastStatsResult) && stats.PresentCount > 0) {
                    firstStats = Qpc();
                    firstStatsCount = stats.PresentCount;
                }
            }
        } else {
            ++presentsA;
        }
    }
    if (!spinner.stop) {
        spinner.stop = true;
        if (spinThread.joinable()) spinThread.join();
    }
    ClipCursor(nullptr);
    const int64_t ended = Qpc();
    auto ms = [&](int64_t qpc) { return qpc ? (qpc - switched) * 1000.0 / g_freq : -1.0; };
    printf("event end qpc=%lld aborted=%d\n", static_cast<long long>(ended), g_abort ? 1 : 0);
    printf("presents A=%u B=%u failures=%u firstStatsB qpc=%lld (+%.1f ms after switch, presentCount=%u) "
           "lastStatsHr=0x%08lX\n",
           presentsA, presentsB, presentFailures, static_cast<long long>(firstStats), ms(firstStats), firstStatsCount,
           static_cast<unsigned long>(lastStatsResult));
    printf("spin cycles=%llu first=%lld last=%lld", static_cast<unsigned long long>(spinner.cycles),
           static_cast<long long>(spinner.firstCycle), static_cast<long long>(spinner.lastCycle));
    for (int call = 0; call < 4; ++call)
        if (bits & (1u << call))
            printf(" %s_failures=%llu(err=%lu)", kCallNames[call], static_cast<unsigned long long>(spinner.failures[call]),
                   spinner.firstError[call]);
    printf("\nspin per100ms:");
    const int64_t lastBucket = (ended - origin) / MsToTicks(100);
    for (int64_t bucket = 0; bucket <= lastBucket && bucket < static_cast<int64_t>(spinner.buckets.size()); ++bucket)
        printf(" %u", spinner.buckets[bucket]);
    printf("\n");
    fflush(stdout);
    if (chain) chain->Release();
    DestroyWindow(g_hwnd);
    return 0;
}
