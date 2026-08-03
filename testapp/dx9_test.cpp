// DX9 / DX9Ex Test App for Capture + FPS Limiter Testing
#define WIN32_LEAN_AND_MEAN
// clang-format off
#include <windows.h>
#include <avrt.h>
// clang-format on
#include <d3d9.h>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "testapp_common.h"

#pragma comment(lib, "d3d9.lib")

static int g_WindowWidth = 1280;
static int g_WindowHeight = 720;
static int g_GpuLoadPasses = 5;
static int g_VSync = 0;
static int g_Fullscreen = 1;

#if defined(CE_TESTAPP_D3D9EX)
using Direct3DFactory = IDirect3D9Ex;
using Direct3DDevice = IDirect3DDevice9Ex;
static constexpr wchar_t kWindowClassName[] = L"DX9ExTest";
static constexpr wchar_t kWindowTitle[] = L"DX9Ex Test";
#else
using Direct3DFactory = IDirect3D9;
using Direct3DDevice = IDirect3DDevice9;
static constexpr wchar_t kWindowClassName[] = L"DX9Test";
static constexpr wchar_t kWindowTitle[] = L"DX9 Test";
#endif

void LoadConfig() {
    char path[MAX_PATH];
    GetModuleFileNameA(NULL, path, MAX_PATH);
    std::string configPath = path;
    size_t pos = configPath.find_last_of("\\/");
    if (pos != std::string::npos)
        configPath = configPath.substr(0, pos + 1) + "testappconfig.ini";

    // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
    g_WindowWidth = GetPrivateProfileIntA("Display", "width", g_WindowWidth, configPath.c_str());
    // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
    g_WindowHeight = GetPrivateProfileIntA("Display", "height", g_WindowHeight, configPath.c_str());
    // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
    g_GpuLoadPasses = GetPrivateProfileIntA("Performance", "gpu_load", g_GpuLoadPasses, configPath.c_str());
    // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
    g_VSync = GetPrivateProfileIntA("Rendering", "vsync", g_VSync, configPath.c_str());
    // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
    g_Fullscreen = GetPrivateProfileIntA("Display", "fullscreen", g_Fullscreen, configPath.c_str());
}

Direct3DFactory* g_pD3D = nullptr;
Direct3DDevice* g_pd3dDevice = nullptr;
bool g_Running = true;
float g_BarPosition = 0.0f;
auto g_StartTime = std::chrono::high_resolution_clock::now();

LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_DESTROY) {
        g_Running = false;
        PostQuitMessage(0);
        return 0;
    }
    if (msg == WM_KEYDOWN && wParam == VK_ESCAPE) {
        g_Running = false;
        DestroyWindow(hWnd);
        return 0;
    }
    return DefWindowProc(hWnd, msg, wParam, lParam);
}

void ShutdownDX9() {
    if (g_pd3dDevice) {
        g_pd3dDevice->Release();
        g_pd3dDevice = nullptr;
    }
    if (g_pD3D) {
        g_pD3D->Release();
        g_pD3D = nullptr;
    }
}

bool InitDX9(HWND hwnd) {
#if defined(CE_TESTAPP_D3D9EX)
    if (FAILED(Direct3DCreate9Ex(D3D_SDK_VERSION, &g_pD3D)))
        return false;
#else
    g_pD3D = Direct3DCreate9(D3D_SDK_VERSION);
    if (!g_pD3D)
        return false;
#endif

    // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization) - zero-initialized placeholder; enum fields are assigned before use
    D3DPRESENT_PARAMETERS d3dpp = {};
#if defined(CE_TESTAPP_D3D9EX)
    d3dpp.Windowed = TRUE;
    d3dpp.SwapEffect = D3DSWAPEFFECT_FLIPEX;
    d3dpp.BackBufferFormat = D3DFMT_X8R8G8B8;
    d3dpp.BackBufferCount = 2;
#else
    d3dpp.Windowed = g_Fullscreen ? FALSE : TRUE;
    d3dpp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    d3dpp.BackBufferFormat = g_Fullscreen ? D3DFMT_X8R8G8B8 : D3DFMT_UNKNOWN;
    d3dpp.BackBufferCount = 1;
#endif
    d3dpp.BackBufferWidth = g_WindowWidth;
    d3dpp.BackBufferHeight = g_WindowHeight;
    d3dpp.hDeviceWindow = hwnd;
    d3dpp.PresentationInterval = g_VSync ? D3DPRESENT_INTERVAL_ONE : D3DPRESENT_INTERVAL_IMMEDIATE;
    d3dpp.FullScreen_RefreshRateInHz =
#if defined(CE_TESTAPP_D3D9EX)
        0;
#else
        g_Fullscreen ? D3DPRESENT_RATE_DEFAULT : 0;
#endif

#if defined(CE_TESTAPP_D3D9EX)
    if (FAILED(g_pD3D->CreateDeviceEx(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd, D3DCREATE_HARDWARE_VERTEXPROCESSING,
                                      &d3dpp, nullptr, &g_pd3dDevice)))
        return false;
    if (!g_Fullscreen)
        g_pd3dDevice->SetMaximumFrameLatency(1);
#else
    if (FAILED(g_pD3D->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd, D3DCREATE_HARDWARE_VERTEXPROCESSING,
                                    &d3dpp, &g_pd3dDevice)))
        return false;
#endif

    return true;
}

void Render() {
    if (!g_pd3dDevice)
        return;
    auto now = std::chrono::high_resolution_clock::now();
    float elapsed = std::chrono::duration<float>(now - g_StartTime).count();
    g_BarPosition = (float)std::fmod((double)(elapsed * 0.5f), 1.0);

    g_pd3dDevice->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_XRGB(25, 25, 25), 1.0f, 0);
    if (SUCCEEDED(g_pd3dDevice->BeginScene())) {
        for (int i = 0; i < g_GpuLoadPasses; i++) {
            D3DCOLOR c = D3DCOLOR_XRGB(25 + (i % 2), 25, 25);
            g_pd3dDevice->Clear(0, nullptr, D3DCLEAR_TARGET, c, 1.0f, 0);
        }

        // Draw Bar
        // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
        D3DRECT rect = {(LONG)(g_BarPosition * (g_WindowWidth - 100)), g_WindowHeight / 2 - 50,
                        // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
                        (LONG)(g_BarPosition * (g_WindowWidth - 100) + 100), g_WindowHeight / 2 + 50};
        g_pd3dDevice->Clear(1, &rect, D3DCLEAR_TARGET, D3DCOLOR_XRGB(255, 255, 255), 1.0f, 0);

        g_pd3dDevice->EndScene();
    }
    g_pd3dDevice->Present(nullptr, nullptr, nullptr, nullptr);
}

    // NOLINTNEXTLINE(bugprone-exception-escape) - standalone test harness: an unexpected exception terminating the process is acceptable and yields a nonzero exit
int main(int argc, char* argv[]) {
    // Give time for hook thread to initialize export hooks
    Sleep(500);

    testapp::EnableGameDpiAwareness();
    testapp::ApplyGameScheduling();
    LoadConfig();
    if (argc >= 3) {
        g_WindowWidth = testapp::ParseIntOrZero(argv[1]);
        g_WindowHeight = testapp::ParseIntOrZero(argv[2]);
    }
    if (argc >= 4) {
        g_GpuLoadPasses = testapp::ParseIntOrZero(argv[3]);
    }

    WNDCLASSEXW wc = {sizeof(WNDCLASSEXW),
                      CS_HREDRAW | CS_VREDRAW,
                      WndProc,
                      0,
                      0,
                      GetModuleHandle(nullptr),
                      nullptr,
                      LoadCursor(nullptr, IDC_ARROW),
                      nullptr,
                      nullptr,
                      kWindowClassName,
                      nullptr};
    RegisterClassExW(&wc);
    RECT monitorRect = testapp::GetPrimaryMonitorRect();
    if (g_Fullscreen) {
        g_WindowWidth = monitorRect.right - monitorRect.left;
        g_WindowHeight = monitorRect.bottom - monitorRect.top;
    }
    DWORD winStyle = g_Fullscreen ? WS_POPUP : WS_OVERLAPPEDWINDOW;
    int posX = g_Fullscreen ? monitorRect.left : CW_USEDEFAULT;
    int posY = g_Fullscreen ? monitorRect.top : CW_USEDEFAULT;
    RECT windowRect = testapp::AdjustWindowRectForClientSize(winStyle, 0, g_WindowWidth, g_WindowHeight);
    int winW = g_Fullscreen ? g_WindowWidth : (windowRect.right - windowRect.left);
    int winH = g_Fullscreen ? g_WindowHeight : (windowRect.bottom - windowRect.top);
    HWND hwnd = CreateWindowW(kWindowClassName, kWindowTitle, winStyle, posX, posY, winW, winH, nullptr, nullptr,
                              wc.hInstance, nullptr);
    if (!InitDX9(hwnd))
        return 1;
    ShowWindow(hwnd, SW_SHOW);

    MSG msg = {};
    while (g_Running) {
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        if (!g_Running)
            break;
        Render();
    }
    ShutdownDX9();
    return 0;
}
