#define WIN32_LEAN_AND_MEAN
#define WINVER 0x0A00
#define _WIN32_WINNT 0x0A00

#include <d3d8.h>
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "testapp_common.h"

static int g_WindowWidth = 1280;
static int g_WindowHeight = 720;
static int g_PresentWidth = 1280;
static int g_PresentHeight = 720;
static int g_WorkloadPasses = 8;
static int g_VSync = 1;
static int g_Fullscreen = 1;
static bool g_Running = true;
static bool g_UseGdiFallback = false;
static auto g_StartTime = std::chrono::high_resolution_clock::now();
static HWND g_MainWindow = nullptr;
static HDC g_GdiBackbufferDc = nullptr;
static HBITMAP g_GdiBackbufferBitmap = nullptr;
static HGDIOBJ g_GdiBackbufferOldBitmap = nullptr;
static int g_GdiBackbufferWidth = 0;
static int g_GdiBackbufferHeight = 0;

static HMODULE g_D3D8Module = nullptr;
static HMODULE g_DwmApiModule = nullptr;
static IDirect3D8* g_D3D = nullptr;
static IDirect3DDevice8* g_Device = nullptr;
// NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization) - zero-initialized placeholder; enum fields are assigned before use
static D3DPRESENT_PARAMETERS g_PresentParameters = {};
static bool g_WindowActive = true;

using Direct3DCreate8Fn = IDirect3D8*(WINAPI*)(UINT sdkVersion);
using DwmFlushFn = HRESULT(WINAPI*)();

static DwmFlushFn g_DwmFlush = nullptr;

template <typename T>
static void SafeRelease(T*& ptr) {
    if (ptr) {
        ptr->Release();
        ptr = nullptr;
    }
}

static bool LogInitFailure(const char* step, HRESULT hr) {
    std::fprintf(stderr, "DX8 init failed at %s (hr=0x%08lX)\n", step, static_cast<unsigned long>(hr));
    return false;
}

static bool EnableGdiFallback(const char* reason, HRESULT hr) {
    if (!g_UseGdiFallback) {
        std::fprintf(stderr, "DX8: using GDI fallback after %s (hr=0x%08lX)\n", reason, static_cast<unsigned long>(hr));
    }

    SafeRelease(g_Device);
    SafeRelease(g_D3D);
    if (g_D3D8Module) {
        FreeLibrary(g_D3D8Module);
        g_D3D8Module = nullptr;
    }
    g_UseGdiFallback = true;
    return true;
}

static void CleanupGdiBackbuffer() {
    if (g_GdiBackbufferDc) {
        if (g_GdiBackbufferOldBitmap) {
            SelectObject(g_GdiBackbufferDc, g_GdiBackbufferOldBitmap);
            g_GdiBackbufferOldBitmap = nullptr;
        }
        if (g_GdiBackbufferBitmap) {
            DeleteObject(g_GdiBackbufferBitmap);
            g_GdiBackbufferBitmap = nullptr;
        }
        DeleteDC(g_GdiBackbufferDc);
        g_GdiBackbufferDc = nullptr;
    }

    g_GdiBackbufferWidth = 0;
    g_GdiBackbufferHeight = 0;
}

static bool EnsureGdiBackbuffer(HWND hwnd, int width, int height) {
    if (!hwnd || width <= 0 || height <= 0) {
        return false;
    }

    if (g_GdiBackbufferDc && g_GdiBackbufferWidth == width && g_GdiBackbufferHeight == height) {
        return true;
    }

    CleanupGdiBackbuffer();

    HDC windowDc = GetDC(hwnd);
    if (!windowDc) {
        return false;
    }

    HDC memoryDc = CreateCompatibleDC(windowDc);
    if (!memoryDc) {
        ReleaseDC(hwnd, windowDc);
        return false;
    }

    HBITMAP bitmap = CreateCompatibleBitmap(windowDc, width, height);
    ReleaseDC(hwnd, windowDc);
    if (!bitmap) {
        DeleteDC(memoryDc);
        return false;
    }

    HGDIOBJ oldBitmap = SelectObject(memoryDc, bitmap);
    if (!oldBitmap || oldBitmap == HGDI_ERROR) {
        DeleteObject(bitmap);
        DeleteDC(memoryDc);
        return false;
    }

    g_GdiBackbufferDc = memoryDc;
    g_GdiBackbufferBitmap = bitmap;
    g_GdiBackbufferOldBitmap = oldBitmap;
    g_GdiBackbufferWidth = width;
    g_GdiBackbufferHeight = height;
    return true;
}

static void InitializeFramePacingSupport() {
    if (g_DwmFlush) {
        return;
    }

    if (!g_DwmApiModule) {
        g_DwmApiModule = LoadLibraryA("dwmapi.dll");
    }
    if (g_DwmApiModule) {
        g_DwmFlush = reinterpret_cast<DwmFlushFn>(GetProcAddress(g_DwmApiModule, "DwmFlush"));
    }
}

static D3DFORMAT ResolveBackBufferFormat() {
    D3DDISPLAYMODE displayMode = {};
    if (g_D3D && SUCCEEDED(g_D3D->GetAdapterDisplayMode(D3DADAPTER_DEFAULT, &displayMode)) &&
        displayMode.Format != D3DFMT_UNKNOWN) {
        return displayMode.Format;
    }

    return D3DFMT_X8R8G8B8;
}

static void LoadConfig() {
    char path[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    std::string configPath = path;
    size_t slashPos = configPath.find_last_of("\\/");
    if (slashPos != std::string::npos) {
        configPath = configPath.substr(0, slashPos + 1) + "testappconfig.ini";
    }

    // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
    g_WindowWidth = GetPrivateProfileIntA("Display", "width", g_WindowWidth, configPath.c_str());
    // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
    g_WindowHeight = GetPrivateProfileIntA("Display", "height", g_WindowHeight, configPath.c_str());
    // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
    g_WorkloadPasses = GetPrivateProfileIntA("Performance", "gpu_load", g_WorkloadPasses, configPath.c_str());
    // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
    g_VSync = GetPrivateProfileIntA("Rendering", "vsync", g_VSync, configPath.c_str());
    // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
    g_Fullscreen = GetPrivateProfileIntA("Display", "fullscreen", g_Fullscreen, configPath.c_str());
}

static void UpdatePresentParameters(HWND hwnd) {
    // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization) - zero-initialized placeholder; enum fields are assigned before use
    g_PresentParameters = {};
    g_PresentParameters.BackBufferWidth = 0;
    g_PresentParameters.BackBufferHeight = 0;
    g_PresentParameters.BackBufferFormat = ResolveBackBufferFormat();
    g_PresentParameters.BackBufferCount = 1;
    g_PresentParameters.MultiSampleType = D3DMULTISAMPLE_NONE;
    g_PresentParameters.SwapEffect = D3DSWAPEFFECT_DISCARD;
    g_PresentParameters.hDeviceWindow = hwnd;
    g_PresentParameters.Windowed = TRUE;
    g_PresentParameters.EnableAutoDepthStencil = FALSE;
    g_PresentParameters.Flags = 0;
    g_PresentParameters.FullScreen_RefreshRateInHz = 0;
    g_PresentParameters.FullScreen_PresentationInterval =
        g_VSync ? D3DPRESENT_INTERVAL_DEFAULT : D3DPRESENT_INTERVAL_IMMEDIATE;
}

static bool ApplyDeviceRenderState() {
    if (!g_Device) {
        return false;
    }

    HRESULT hr = g_Device->SetRenderState(D3DRS_LIGHTING, FALSE);
    if (FAILED(hr)) {
        return EnableGdiFallback("SetRenderState(LIGHTING)", hr);
    }
    hr = g_Device->SetRenderState(D3DRS_ZENABLE, FALSE);
    if (FAILED(hr)) {
        return EnableGdiFallback("SetRenderState(ZENABLE)", hr);
    }
    hr = g_Device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    if (FAILED(hr)) {
        return EnableGdiFallback("SetRenderState(CULLMODE)", hr);
    }
    return true;
}

static bool ResetDX8Device(HWND hwnd) {
    if (!g_Device || !hwnd) {
        return false;
    }

    if (g_Fullscreen) {
        const RECT monitorRect = testapp::GetPrimaryMonitorRect();
        g_PresentWidth = monitorRect.right - monitorRect.left;
        g_PresentHeight = monitorRect.bottom - monitorRect.top;
        g_WindowWidth = g_PresentWidth;
        g_WindowHeight = g_PresentHeight;
    } else {
        RECT clientRect = {};
        if (GetClientRect(hwnd, &clientRect)) {
            const int clientWidth = clientRect.right - clientRect.left;
            const int clientHeight = clientRect.bottom - clientRect.top;
            if (clientWidth > 0 && clientHeight > 0) {
                g_PresentWidth = clientWidth;
                g_PresentHeight = clientHeight;
                g_WindowWidth = clientWidth;
                g_WindowHeight = clientHeight;
            }
        }
    }

    UpdatePresentParameters(hwnd);

    HRESULT hr = g_Device->Reset(&g_PresentParameters);
    if (FAILED(hr)) {
        if (hr != D3DERR_DEVICELOST && hr != D3DERR_DEVICENOTRESET) {
            return EnableGdiFallback("IDirect3DDevice8::Reset", hr);
        }
        return false;
    }

    return ApplyDeviceRenderState();
}

static bool EnsureDeviceReady(HWND hwnd) {
    if (g_UseGdiFallback) {
        return true;
    }
    if (!g_Device) {
        return false;
    }

    const HRESULT hr = g_Device->TestCooperativeLevel();
    if (hr == D3D_OK) {
        return true;
    }
    if (hr == D3DERR_DEVICENOTRESET) {
        return ResetDX8Device(hwnd);
    }
    if (hr == D3DERR_DEVICELOST) {
        return false;
    }

    return EnableGdiFallback("IDirect3DDevice8::TestCooperativeLevel", hr);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_DESTROY:
            g_Running = false;
            PostQuitMessage(0);
            return 0;
        case WM_ACTIVATEAPP:
            g_WindowActive = wParam != FALSE;
            return 0;
        case WM_ERASEBKGND:
            if (g_UseGdiFallback) {
                return 1;
            }
            break;
        case WM_KEYDOWN:
            if (wParam == VK_ESCAPE) {
                g_Running = false;
                DestroyWindow(hwnd);
                return 0;
            }
            break;
        default:
            break;
    }

    return DefWindowProc(hwnd, msg, wParam, lParam);
}

struct TLVertex {
    float x;
    float y;
    float z;
    float rhw;
    D3DCOLOR color;
};

static constexpr DWORD kVertexFormat = D3DFVF_XYZRHW | D3DFVF_DIFFUSE;

static IDirect3D8* CreateDirect3D8Instance() {
    g_D3D8Module = LoadLibraryA("d3d8.dll");
    if (!g_D3D8Module) {
        std::fprintf(stderr, "DX8 init failed at LoadLibraryA(d3d8.dll) (gle=%lu)\n", GetLastError());
        return nullptr;
    }

    auto direct3DCreate8 = reinterpret_cast<Direct3DCreate8Fn>(GetProcAddress(g_D3D8Module, "Direct3DCreate8"));
    if (!direct3DCreate8) {
        std::fprintf(stderr, "DX8 init failed at GetProcAddress(Direct3DCreate8) (gle=%lu)\n", GetLastError());
        FreeLibrary(g_D3D8Module);
        g_D3D8Module = nullptr;
        return nullptr;
    }

    return direct3DCreate8(D3D_SDK_VERSION);
}

static bool InitDX8(HWND hwnd) {
    g_D3D = CreateDirect3D8Instance();
    if (!g_D3D) {
        return EnableGdiFallback("CreateDirect3D8Instance", HRESULT_FROM_WIN32(GetLastError()));
    }

    UpdatePresentParameters(hwnd);

    HRESULT hr = g_D3D->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd, D3DCREATE_HARDWARE_VERTEXPROCESSING,
                                     &g_PresentParameters, &g_Device);
    if (FAILED(hr)) {
        hr = g_D3D->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd, D3DCREATE_SOFTWARE_VERTEXPROCESSING,
                                 &g_PresentParameters, &g_Device);
    }
    if (FAILED(hr)) {
        hr = g_D3D->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_REF, hwnd, D3DCREATE_SOFTWARE_VERTEXPROCESSING,
                                 &g_PresentParameters, &g_Device);
    }
    if (FAILED(hr)) {
        return EnableGdiFallback("IDirect3D8::CreateDevice", hr);
    }

    return ApplyDeviceRenderState();
}

static void RenderFrame() {
    if (g_UseGdiFallback) {
        if (!g_MainWindow) {
            return;
        }

        const auto now = std::chrono::high_resolution_clock::now();
        const float t = std::chrono::duration<float>(now - g_StartTime).count();
        const float barPosition = static_cast<float>(std::fmod(static_cast<double>(t * 0.5f), 1.0));

        RECT clientRect = {};
        GetClientRect(g_MainWindow, &clientRect);
        if (!EnsureGdiBackbuffer(g_MainWindow, clientRect.right, clientRect.bottom)) {
            return;
        }

        HDC dc = GetDC(g_MainWindow);
        if (!dc) {
            return;
        }

        HDC drawDc = g_GdiBackbufferDc;

        HBRUSH background = CreateSolidBrush(RGB(28, 32, 60));
        FillRect(drawDc, &clientRect, background);
        DeleteObject(background);

        RECT panel = {clientRect.right / 2 - 110, clientRect.bottom / 2 - 72, clientRect.right / 2 + 110,
                      clientRect.bottom / 2 + 72};
        HBRUSH panelBrush = CreateSolidBrush(RGB(52, 86, 140));
        FillRect(drawDc, &panel, panelBrush);
        DeleteObject(panelBrush);

        const int maxBarX = std::max<int>(1, clientRect.right - 120);
        // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
        RECT movingBar = {(LONG)(barPosition * maxBarX), clientRect.bottom / 2 - 48,
                          // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
                          (LONG)(barPosition * maxBarX + 120), clientRect.bottom / 2 + 48};
        HBRUSH barBrush = CreateSolidBrush(RGB(248, 242, 220));
        FillRect(drawDc, &movingBar, barBrush);
        DeleteObject(barBrush);

        for (int pass = 0; pass < std::max(1, g_WorkloadPasses); ++pass) {
            RECT stripe = {(pass * 73 + static_cast<int>(t * 90.0f)) % std::max<int>(1, clientRect.right),
                           (pass * 41 + static_cast<int>(t * 70.0f)) % std::max<int>(1, clientRect.bottom), 0, 0};
            stripe.right = std::min(clientRect.right, stripe.left + 48);
            stripe.bottom = std::min(clientRect.bottom, stripe.top + 18);
            HBRUSH stripeBrush = CreateSolidBrush((pass % 2) == 0 ? RGB(106, 238, 170) : RGB(255, 120, 104));
            FillRect(drawDc, &stripe, stripeBrush);
            DeleteObject(stripeBrush);
        }

        BitBlt(dc, 0, 0, clientRect.right, clientRect.bottom, drawDc, 0, 0, SRCCOPY);

        ReleaseDC(g_MainWindow, dc);
        return;
    }

    if (!g_Device) {
        return;
    }

    const auto now = std::chrono::high_resolution_clock::now();
    const float t = std::chrono::duration<float>(now - g_StartTime).count();

    const int clearR = static_cast<int>(64.0f + 48.0f * std::sin(t * 0.7f));
    const int clearG = static_cast<int>(80.0f + 64.0f * std::sin(t * 0.9f + 0.8f));
    const int clearB = static_cast<int>(110.0f + 72.0f * std::sin(t * 1.1f + 1.6f));
    g_Device->Clear(0, nullptr, D3DCLEAR_TARGET,
                    D3DCOLOR_XRGB(std::clamp(clearR, 0, 255), std::clamp(clearG, 0, 255), std::clamp(clearB, 0, 255)),
                    1.0f, 0);

    if (SUCCEEDED(g_Device->BeginScene())) {
        g_Device->SetVertexShader(kVertexFormat);

        // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
        const float cx = g_WindowWidth * 0.5f;
        // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
        const float cy = g_WindowHeight * 0.5f;
        // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
        const float radius = std::min(g_WindowWidth, g_WindowHeight) * 0.22f;
        const int passes = std::max(1, g_WorkloadPasses);

        for (int pass = 0; pass < passes; ++pass) {
            const float passAngle = t * 1.7f + static_cast<float>(pass) * 0.25f;
            // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
            const float offsetX = std::sin(t * 0.4f + pass * 0.6f) * g_WindowWidth * 0.14f;
            // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
            const float offsetY = std::cos(t * 0.5f + pass * 0.4f) * g_WindowHeight * 0.11f;

            TLVertex triangle[3] = {};
            for (int i = 0; i < 3; ++i) {
                const float angle = passAngle + static_cast<float>(i) * 2.0943951f;
                triangle[i].x = cx + offsetX + std::cos(angle) * radius;
                triangle[i].y = cy + offsetY + std::sin(angle) * radius;
                triangle[i].z = 0.5f;
                triangle[i].rhw = 1.0f;
            }

            triangle[0].color = D3DCOLOR_XRGB(255, 96, 96);
            triangle[1].color = D3DCOLOR_XRGB(96, 255, 160);
            triangle[2].color = D3DCOLOR_XRGB(96, 160, 255);
            g_Device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, triangle, sizeof(TLVertex));
        }

        g_Device->EndScene();
    }

    HRESULT hr = g_Device->Present(nullptr, nullptr, nullptr, nullptr);
    if (hr == D3DERR_DEVICELOST || hr == D3DERR_DEVICENOTRESET) {
        return;
    }
    if (FAILED(hr)) {
        EnableGdiFallback("IDirect3DDevice8::Present", hr);
        return;
    }

    if (g_VSync && g_DwmFlush) {
        g_DwmFlush();
    }
}

    // NOLINTNEXTLINE(bugprone-exception-escape) - standalone test harness: an unexpected exception terminating the process is acceptable and yields a nonzero exit
int main(int argc, char* argv[]) {
    Sleep(500);

    testapp::EnableGameDpiAwareness();
    testapp::ApplyGameScheduling();
    LoadConfig();
    InitializeFramePacingSupport();

    if (testapp::LaunchX86SiblingProcess(argc, argv)) {
        return 0;
    }

    if (argc >= 3) {
        g_WindowWidth = testapp::ParseIntOrZero(argv[1]);
        g_WindowHeight = testapp::ParseIntOrZero(argv[2]);
    }
    if (argc >= 4) {
        g_WorkloadPasses = testapp::ParseIntOrZero(argv[3]);
    }

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandle(nullptr);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"DX8Test";
    RegisterClassExW(&wc);

    const RECT monitorRect = testapp::GetPrimaryMonitorRect();
    g_PresentWidth = g_Fullscreen ? (monitorRect.right - monitorRect.left) : g_WindowWidth;
    g_PresentHeight = g_Fullscreen ? (monitorRect.bottom - monitorRect.top) : g_WindowHeight;
    if (g_Fullscreen) {
        g_WindowWidth = g_PresentWidth;
        g_WindowHeight = g_PresentHeight;
    }

    const DWORD winStyle = g_Fullscreen ? WS_POPUP : WS_OVERLAPPEDWINDOW;
    const int posX = g_Fullscreen ? monitorRect.left : CW_USEDEFAULT;
    const int posY = g_Fullscreen ? monitorRect.top : CW_USEDEFAULT;
    const RECT windowRect = testapp::AdjustWindowRectForClientSize(winStyle, 0, g_PresentWidth, g_PresentHeight);
    const int winW = g_Fullscreen ? g_PresentWidth : (windowRect.right - windowRect.left);
    const int winH = g_Fullscreen ? g_PresentHeight : (windowRect.bottom - windowRect.top);

    HWND hwnd = CreateWindowW(L"DX8Test", L"DX8 Test", winStyle, posX, posY, winW, winH, nullptr, nullptr, wc.hInstance,
                              nullptr);
    if (!hwnd) {
        return 1;
    }
    g_MainWindow = hwnd;

    if (!testapp::PrimeWindowForBenchmark(hwnd, g_Fullscreen != 0, g_PresentWidth, g_PresentHeight)) {
        DestroyWindow(hwnd);
        return 0;
    }

    if (!InitDX8(hwnd)) {
        SafeRelease(g_Device);
        SafeRelease(g_D3D);
        DestroyWindow(hwnd);
        return 1;
    }

    if (!testapp::PrimeWindowForBenchmark(hwnd, g_Fullscreen != 0, g_PresentWidth, g_PresentHeight)) {
        SafeRelease(g_Device);
        SafeRelease(g_D3D);
        DestroyWindow(hwnd);
        return 0;
    }

    MSG msg = {};
    while (g_Running) {
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        if (!g_Running)
            break;
        if (!g_UseGdiFallback && !EnsureDeviceReady(hwnd)) {
            Sleep(g_Fullscreen && !g_WindowActive ? 50 : 10);
            continue;
        }
        RenderFrame();
    }

    SafeRelease(g_Device);
    SafeRelease(g_D3D);
    CleanupGdiBackbuffer();
    if (g_DwmApiModule) {
        FreeLibrary(g_DwmApiModule);
        g_DwmApiModule = nullptr;
        g_DwmFlush = nullptr;
    }
    if (g_D3D8Module) {
        FreeLibrary(g_D3D8Module);
        g_D3D8Module = nullptr;
    }
    return 0;
}
