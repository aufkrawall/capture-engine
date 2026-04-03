#define WIN32_LEAN_AND_MEAN
#define WINVER 0x0A00
#define _WIN32_WINNT 0x0A00

#include <windows.h>
#include <d3d8.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "testapp_common.h"

static int g_WindowWidth = 1280;
static int g_WindowHeight = 720;
static int g_WorkloadPasses = 8;
static int g_VSync = 1;
static int g_Fullscreen = 1;
static bool g_Running = true;
static auto g_StartTime = std::chrono::high_resolution_clock::now();

static HMODULE g_D3D8Module = nullptr;
static IDirect3D8* g_D3D = nullptr;
static IDirect3DDevice8* g_Device = nullptr;

using Direct3DCreate8Fn = IDirect3D8*(WINAPI*)(UINT sdkVersion);

template <typename T>
static void SafeRelease(T*& ptr) {
    if (ptr) {
        ptr->Release();
        ptr = nullptr;
    }
}

static void LoadConfig() {
    char path[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    std::string configPath = path;
    size_t slashPos = configPath.find_last_of("\\/");
    if (slashPos != std::string::npos) {
        configPath = configPath.substr(0, slashPos + 1) + "testappconfig.ini";
    }

    g_WindowWidth = GetPrivateProfileIntA("Display", "width", g_WindowWidth, configPath.c_str());
    g_WindowHeight = GetPrivateProfileIntA("Display", "height", g_WindowHeight, configPath.c_str());
    g_WorkloadPasses = GetPrivateProfileIntA("Performance", "gpu_load", g_WorkloadPasses, configPath.c_str());
    g_VSync = GetPrivateProfileIntA("Rendering", "vsync", g_VSync, configPath.c_str());
    g_Fullscreen = GetPrivateProfileIntA("Display", "fullscreen", g_Fullscreen, configPath.c_str());
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_DESTROY:
            g_Running = false;
            PostQuitMessage(0);
            return 0;
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
        return nullptr;
    }

    auto direct3DCreate8 = reinterpret_cast<Direct3DCreate8Fn>(GetProcAddress(g_D3D8Module, "Direct3DCreate8"));
    if (!direct3DCreate8) {
        FreeLibrary(g_D3D8Module);
        g_D3D8Module = nullptr;
        return nullptr;
    }

    return direct3DCreate8(D3D_SDK_VERSION);
}

static bool InitDX8(HWND hwnd) {
    g_D3D = CreateDirect3D8Instance();
    if (!g_D3D) {
        return false;
    }

    D3DPRESENT_PARAMETERS pp = {};
    pp.BackBufferWidth = static_cast<UINT>(g_WindowWidth);
    pp.BackBufferHeight = static_cast<UINT>(g_WindowHeight);
    pp.BackBufferFormat = D3DFMT_UNKNOWN;
    pp.BackBufferCount = 1;
    pp.MultiSampleType = D3DMULTISAMPLE_NONE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.hDeviceWindow = hwnd;
    pp.Windowed = TRUE;
    pp.EnableAutoDepthStencil = FALSE;
    pp.Flags = 0;
    pp.FullScreen_RefreshRateInHz = 0;
    pp.FullScreen_PresentationInterval = g_VSync ? D3DPRESENT_INTERVAL_ONE : D3DPRESENT_INTERVAL_IMMEDIATE;

    HRESULT hr = g_D3D->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
                                     D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &g_Device);
    if (FAILED(hr)) {
        hr = g_D3D->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
                                 D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &g_Device);
    }
    if (FAILED(hr)) {
        return false;
    }

    g_Device->SetRenderState(D3DRS_LIGHTING, FALSE);
    g_Device->SetRenderState(D3DRS_ZENABLE, FALSE);
    g_Device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    return true;
}

static void RenderFrame() {
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

        const float cx = g_WindowWidth * 0.5f;
        const float cy = g_WindowHeight * 0.5f;
        const float radius = std::min(g_WindowWidth, g_WindowHeight) * 0.22f;
        const int passes = std::max(1, g_WorkloadPasses);

        for (int pass = 0; pass < passes; ++pass) {
            const float passAngle = t * 1.7f + static_cast<float>(pass) * 0.25f;
            const float offsetX = std::sin(t * 0.4f + pass * 0.6f) * g_WindowWidth * 0.14f;
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

    g_Device->Present(nullptr, nullptr, nullptr, nullptr);
}

int main(int argc, char* argv[]) {
    Sleep(500);

    testapp::EnableGameDpiAwareness();
    testapp::ApplyGameScheduling();
    LoadConfig();

    if (argc >= 3) {
        g_WindowWidth = atoi(argv[1]);
        g_WindowHeight = atoi(argv[2]);
    }
    if (argc >= 4) {
        g_WorkloadPasses = atoi(argv[3]);
    }

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandle(nullptr);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"DX8Test";
    RegisterClassExW(&wc);

    const RECT monitorRect = testapp::GetPrimaryMonitorRect();
    if (g_Fullscreen) {
        g_WindowWidth = monitorRect.right - monitorRect.left;
        g_WindowHeight = monitorRect.bottom - monitorRect.top;
    }

    const DWORD winStyle = g_Fullscreen ? WS_POPUP : WS_OVERLAPPEDWINDOW;
    const int posX = g_Fullscreen ? monitorRect.left : CW_USEDEFAULT;
    const int posY = g_Fullscreen ? monitorRect.top : CW_USEDEFAULT;
    const RECT windowRect = testapp::AdjustWindowRectForClientSize(winStyle, 0, g_WindowWidth, g_WindowHeight);
    const int winW = g_Fullscreen ? g_WindowWidth : (windowRect.right - windowRect.left);
    const int winH = g_Fullscreen ? g_WindowHeight : (windowRect.bottom - windowRect.top);

    HWND hwnd = CreateWindowW(L"DX8Test", L"DX8 Test", winStyle, posX, posY, winW, winH, nullptr, nullptr,
                              wc.hInstance, nullptr);
    if (!hwnd) {
        return 1;
    }

    if (!InitDX8(hwnd)) {
        SafeRelease(g_Device);
        SafeRelease(g_D3D);
        DestroyWindow(hwnd);
        return 1;
    }

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    MSG msg = {};
    while (g_Running) {
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        RenderFrame();
    }

    SafeRelease(g_Device);
    SafeRelease(g_D3D);
    if (g_D3D8Module) {
        FreeLibrary(g_D3D8Module);
        g_D3D8Module = nullptr;
    }
    return 0;
}