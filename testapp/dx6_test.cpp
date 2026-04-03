#define WIN32_LEAN_AND_MEAN
#define WINVER 0x0A00
#define _WIN32_WINNT 0x0A00
#define DIRECTDRAW_VERSION 0x0600
#define DIRECT3D_VERSION 0x0600

#include <windows.h>
#include <ddraw.h>
#include <d3d.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "testapp_common.h"

#pragma comment(lib, "ddraw.lib")
#pragma comment(lib, "dxguid.lib")

static int g_WindowWidth = 1280;
static int g_WindowHeight = 720;
static int g_WorkloadPasses = 8;
static int g_VSync = 1;
static int g_Fullscreen = 1;
static bool g_Running = true;
static auto g_StartTime = std::chrono::high_resolution_clock::now();

static IDirectDraw4* g_DirectDraw = nullptr;
static IDirectDrawSurface4* g_PrimarySurface = nullptr;
static IDirectDrawSurface4* g_RenderSurface = nullptr;
static IDirectDrawClipper* g_Clipper = nullptr;
static IDirect3D3* g_D3D = nullptr;
static IDirect3DDevice3* g_Device = nullptr;
static IDirect3DViewport3* g_Viewport = nullptr;

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

static RECT GetClientScreenRect(HWND hwnd) {
    RECT rect = {};
    GetClientRect(hwnd, &rect);
    POINT topLeft = {rect.left, rect.top};
    POINT bottomRight = {rect.right, rect.bottom};
    ClientToScreen(hwnd, &topLeft);
    ClientToScreen(hwnd, &bottomRight);
    rect.left = topLeft.x;
    rect.top = topLeft.y;
    rect.right = bottomRight.x;
    rect.bottom = bottomRight.y;
    return rect;
}

struct TLVertex {
    float x;
    float y;
    float z;
    float rhw;
    DWORD color;
};

static constexpr DWORD kVertexFormat = D3DFVF_XYZRHW | D3DFVF_DIFFUSE;

static DWORD MakeColor(DWORD red, DWORD green, DWORD blue) {
    return 0xFF000000u | ((red & 0xFFu) << 16) | ((green & 0xFFu) << 8) | (blue & 0xFFu);
}

static bool CreateRenderSurface() {
    DDSURFACEDESC2 desc = {};
    desc.dwSize = sizeof(desc);
    desc.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT;
    desc.dwWidth = static_cast<DWORD>(g_WindowWidth);
    desc.dwHeight = static_cast<DWORD>(g_WindowHeight);
    desc.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_3DDEVICE | DDSCAPS_VIDEOMEMORY;

    HRESULT hr = g_DirectDraw->CreateSurface(&desc, &g_RenderSurface, nullptr);
    if (FAILED(hr)) {
        desc.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_3DDEVICE;
        hr = g_DirectDraw->CreateSurface(&desc, &g_RenderSurface, nullptr);
    }

    return SUCCEEDED(hr) && g_RenderSurface != nullptr;
}

static bool InitDX6(HWND hwnd) {
    HRESULT hr = DirectDrawCreateEx(nullptr, reinterpret_cast<void**>(&g_DirectDraw), IID_IDirectDraw4, nullptr);
    if (FAILED(hr) || !g_DirectDraw) {
        return false;
    }

    hr = g_DirectDraw->SetCooperativeLevel(hwnd, DDSCL_NORMAL);
    if (FAILED(hr)) {
        return false;
    }

    DDSURFACEDESC2 primaryDesc = {};
    primaryDesc.dwSize = sizeof(primaryDesc);
    primaryDesc.dwFlags = DDSD_CAPS;
    primaryDesc.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE;
    hr = g_DirectDraw->CreateSurface(&primaryDesc, &g_PrimarySurface, nullptr);
    if (FAILED(hr) || !g_PrimarySurface) {
        return false;
    }

    if (!g_Fullscreen) {
        hr = g_DirectDraw->CreateClipper(0, &g_Clipper, nullptr);
        if (FAILED(hr) || !g_Clipper) {
            return false;
        }
        g_Clipper->SetHWnd(0, hwnd);
        g_PrimarySurface->SetClipper(g_Clipper);
    }

    if (!CreateRenderSurface()) {
        return false;
    }

    hr = g_DirectDraw->QueryInterface(IID_IDirect3D3, reinterpret_cast<void**>(&g_D3D));
    if (FAILED(hr) || !g_D3D) {
        return false;
    }

    const GUID* deviceTypes[] = {&IID_IDirect3DHALDevice, &IID_IDirect3DRGBDevice};
    for (const GUID* deviceType : deviceTypes) {
        hr = g_D3D->CreateDevice(*deviceType, g_RenderSurface, &g_Device, nullptr);
        if (SUCCEEDED(hr) && g_Device) {
            break;
        }
    }
    if (!g_Device) {
        return false;
    }

    hr = g_D3D->CreateViewport(&g_Viewport, nullptr);
    if (FAILED(hr) || !g_Viewport) {
        return false;
    }

    D3DVIEWPORT2 viewport = {};
    viewport.dwSize = sizeof(viewport);
    viewport.dwWidth = static_cast<DWORD>(g_WindowWidth);
    viewport.dwHeight = static_cast<DWORD>(g_WindowHeight);
    viewport.dvClipX = -1.0f;
    viewport.dvClipY = 1.0f;
    viewport.dvClipWidth = 2.0f;
    viewport.dvClipHeight = 2.0f;
    viewport.dvMinZ = 0.0f;
    viewport.dvMaxZ = 1.0f;
    g_Viewport->SetViewport2(&viewport);

    g_Device->AddViewport(g_Viewport);
    g_Device->SetCurrentViewport(g_Viewport);
    g_Device->SetRenderState(D3DRENDERSTATE_LIGHTING, FALSE);
    g_Device->SetRenderState(D3DRENDERSTATE_ZENABLE, FALSE);
    g_Device->SetRenderState(D3DRENDERSTATE_CULLMODE, D3DCULL_NONE);
    g_Device->SetRenderState(D3DRENDERSTATE_SHADEMODE, D3DSHADE_GOURAUD);
    return true;
}

static void RestoreSurfaces() {
    if (g_PrimarySurface) {
        g_PrimarySurface->Restore();
    }
    if (g_RenderSurface) {
        g_RenderSurface->Restore();
    }
}

static void PresentFrame(HWND hwnd) {
    if (!g_PrimarySurface || !g_RenderSurface) {
        return;
    }

    if (g_VSync && g_DirectDraw) {
        g_DirectDraw->WaitForVerticalBlank(DDWAITVB_BLOCKBEGIN, nullptr);
    }

    RECT destRect = GetClientScreenRect(hwnd);
    HRESULT hr = g_PrimarySurface->Blt(&destRect, g_RenderSurface, nullptr, DDBLT_WAIT, nullptr);
    if (hr == DDERR_SURFACELOST) {
        RestoreSurfaces();
    }
}

static void RenderFrame(HWND hwnd) {
    if (!g_Device || !g_Viewport) {
        return;
    }

    const auto now = std::chrono::high_resolution_clock::now();
    const float t = std::chrono::duration<float>(now - g_StartTime).count();
    g_Viewport->Clear(0, nullptr, D3DCLEAR_TARGET);

    if (SUCCEEDED(g_Device->BeginScene())) {
        TLVertex background[6] = {};
        background[0] = {0.0f, 0.0f, 0.5f, 1.0f, MakeColor(24, 40, 92)};
        background[1] = {static_cast<float>(g_WindowWidth), 0.0f, 0.5f, 1.0f, MakeColor(12, 28, 72)};
        background[2] = {static_cast<float>(g_WindowWidth), static_cast<float>(g_WindowHeight), 0.5f, 1.0f,
                 MakeColor(10, 18, 46)};
        background[3] = {0.0f, 0.0f, 0.5f, 1.0f, MakeColor(24, 40, 92)};
        background[4] = {static_cast<float>(g_WindowWidth), static_cast<float>(g_WindowHeight), 0.5f, 1.0f,
                 MakeColor(10, 18, 46)};
        background[5] = {0.0f, static_cast<float>(g_WindowHeight), 0.5f, 1.0f, MakeColor(20, 26, 58)};
        g_Device->DrawPrimitive(D3DPT_TRIANGLELIST, kVertexFormat, background, 6, 0);

        const float cx = g_WindowWidth * 0.5f;
        const float cy = g_WindowHeight * 0.5f;
        const float radius = std::min(g_WindowWidth, g_WindowHeight) * 0.2f;
        const int passes = std::max(1, g_WorkloadPasses);

        for (int pass = 0; pass < passes; ++pass) {
            const float passAngle = t * 1.4f + static_cast<float>(pass) * 0.2f;
            const float offsetX = std::sin(t * 0.5f + pass * 0.7f) * g_WindowWidth * 0.17f;
            const float offsetY = std::cos(t * 0.45f + pass * 0.45f) * g_WindowHeight * 0.13f;

            TLVertex triangle[3] = {};
            for (int i = 0; i < 3; ++i) {
                const float angle = passAngle + static_cast<float>(i) * 2.0943951f;
                triangle[i].x = cx + offsetX + std::cos(angle) * radius;
                triangle[i].y = cy + offsetY + std::sin(angle) * radius;
                triangle[i].z = 0.5f;
                triangle[i].rhw = 1.0f;
            }
            triangle[0].color = MakeColor(255, 110, 96);
            triangle[1].color = MakeColor(96, 255, 170);
            triangle[2].color = MakeColor(96, 170, 255);
            g_Device->DrawPrimitive(D3DPT_TRIANGLELIST, kVertexFormat, triangle, 3, 0);
        }

        g_Device->EndScene();
    }

    PresentFrame(hwnd);
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
    wc.lpszClassName = L"DX6Test";
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

    HWND hwnd = CreateWindowW(L"DX6Test", L"DX6 Test", winStyle, posX, posY, winW, winH, nullptr, nullptr,
                              wc.hInstance, nullptr);
    if (!hwnd) {
        return 1;
    }

    if (!InitDX6(hwnd)) {
        SafeRelease(g_Viewport);
        SafeRelease(g_Device);
        SafeRelease(g_D3D);
        SafeRelease(g_Clipper);
        SafeRelease(g_RenderSurface);
        SafeRelease(g_PrimarySurface);
        SafeRelease(g_DirectDraw);
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
        if (!g_Running)
            break;
        RenderFrame(hwnd);
    }

    SafeRelease(g_Viewport);
    SafeRelease(g_Device);
    SafeRelease(g_D3D);
    SafeRelease(g_Clipper);
    SafeRelease(g_RenderSurface);
    SafeRelease(g_PrimarySurface);
    SafeRelease(g_DirectDraw);
    return 0;
}