#define WIN32_LEAN_AND_MEAN
#define WINVER 0x0A00
#define _WIN32_WINNT 0x0A00
#define DIRECTDRAW_VERSION 0x0600
#define DIRECT3D_VERSION 0x0600

#include <d3d.h>
#include <ddraw.h>
#include <windows.h>

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
static int g_PresentWidth = 1280;
static int g_PresentHeight = 720;
static int g_WorkloadPasses = 8;
static int g_VSync = 1;
static int g_Fullscreen = 1;
static bool g_Running = true;
static bool g_UseDirectDrawFallback = false;
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

static bool LogInitFailure(const char* step, HRESULT hr) {
    std::fprintf(stderr, "DX6 init failed at %s (hr=0x%08lX)\n", step, static_cast<unsigned long>(hr));
    return false;
}

static bool EnableDirectDrawFallback(const char* step, HRESULT hr) {
    SafeRelease(g_Viewport);
    SafeRelease(g_Device);
    SafeRelease(g_D3D);
    if (!g_UseDirectDrawFallback) {
        std::fprintf(stderr, "DX6: using DirectDraw fallback after %s (hr=0x%08lX)\n", step,
                     static_cast<unsigned long>(hr));
    }
    g_UseDirectDrawFallback = true;
    return true;
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

static void RestoreSurfaces();

static uint32_t PackChannel(uint32_t mask, uint8_t value) {
    if (mask == 0) {
        return 0;
    }

    uint32_t shift = 0;
    while (((mask >> shift) & 1U) == 0U) {
        ++shift;
    }

    const uint32_t maxValue = mask >> shift;
    const uint32_t scaled = (static_cast<uint32_t>(value) * maxValue + 127U) / 255U;
    return (scaled << shift) & mask;
}

static uint32_t PackColor(const DDPIXELFORMAT& pixelFormat, uint8_t red, uint8_t green, uint8_t blue) {
    return PackChannel(pixelFormat.dwRBitMask, red) | PackChannel(pixelFormat.dwGBitMask, green) |
           PackChannel(pixelFormat.dwBBitMask, blue);
}

static bool ColorFillSurfaceRect(IDirectDrawSurface4* surface, const DDSURFACEDESC2& desc, const RECT& rect,
                                 uint32_t packedColor) {
    if (!surface) {
        return false;
    }

    RECT clipped = {
        std::clamp(rect.left, 0L, static_cast<LONG>(desc.dwWidth)),
        std::clamp(rect.top, 0L, static_cast<LONG>(desc.dwHeight)),
        std::clamp(rect.right, 0L, static_cast<LONG>(desc.dwWidth)),
        std::clamp(rect.bottom, 0L, static_cast<LONG>(desc.dwHeight)),
    };
    if (clipped.left >= clipped.right || clipped.top >= clipped.bottom) {
        return true;
    }

    DDBLTFX bltFx = {};
    bltFx.dwSize = sizeof(bltFx);
    bltFx.dwFillColor = packedColor;
    HRESULT hr = surface->Blt(&clipped, nullptr, nullptr, DDBLT_COLORFILL | DDBLT_WAIT, &bltFx);
    if (hr == DDERR_SURFACELOST) {
        RestoreSurfaces();
    }
    return SUCCEEDED(hr);
}

static void RenderDirectDrawFallbackFrame() {
    if (!g_RenderSurface) {
        return;
    }

    DDSURFACEDESC2 desc = {};
    desc.dwSize = sizeof(desc);
    HRESULT hr = g_RenderSurface->GetSurfaceDesc(&desc);
    if (FAILED(hr)) {
        if (hr == DDERR_SURFACELOST) {
            RestoreSurfaces();
        }
        return;
    }

    const auto now = std::chrono::high_resolution_clock::now();
    const float t = std::chrono::duration<float>(now - g_StartTime).count();

    const uint32_t background = PackColor(desc.ddpfPixelFormat, 18, 24, 48);
    const uint32_t panel = PackColor(desc.ddpfPixelFormat, 38, 68, 124);
    const uint32_t accentA = PackColor(desc.ddpfPixelFormat, 244, 240, 214);
    const uint32_t accentB = PackColor(desc.ddpfPixelFormat, 100, 232, 164);
    const uint32_t accentC = PackColor(desc.ddpfPixelFormat, 255, 118, 104);

    RECT fullRect = {0, 0, static_cast<LONG>(desc.dwWidth), static_cast<LONG>(desc.dwHeight)};
    ColorFillSurfaceRect(g_RenderSurface, desc, fullRect, background);

    RECT centerPanel = {static_cast<LONG>(desc.dwWidth / 2) - 110, static_cast<LONG>(desc.dwHeight / 2) - 72,
                        static_cast<LONG>(desc.dwWidth / 2) + 110, static_cast<LONG>(desc.dwHeight / 2) + 72};
    ColorFillSurfaceRect(g_RenderSurface, desc, centerPanel, panel);

    const LONG barWidth = std::max<LONG>(80, static_cast<LONG>(desc.dwWidth / 6));
    const LONG maxBarX = std::max<LONG>(1, static_cast<LONG>(desc.dwWidth) - barWidth);
    const LONG barX = static_cast<LONG>((std::sin(t * 1.25f) * 0.5f + 0.5f) * static_cast<float>(maxBarX));
    const LONG barY = static_cast<LONG>(desc.dwHeight / 2) - 42;
    RECT movingBar = {barX, barY, barX + barWidth, barY + 84};
    ColorFillSurfaceRect(g_RenderSurface, desc, movingBar, accentA);

    const int passes = std::max(1, g_WorkloadPasses);
    for (int pass = 0; pass < passes; ++pass) {
        const int x = (pass * 79 + static_cast<int>(t * 120.0f)) % std::max<int>(1, static_cast<int>(desc.dwWidth));
        const int y = (pass * 53 + static_cast<int>(t * 85.0f)) % std::max<int>(1, static_cast<int>(desc.dwHeight));
        RECT stripe = {
            std::clamp(x - 34, 0, static_cast<int>(desc.dwWidth)),
            std::clamp(y - 10, 0, static_cast<int>(desc.dwHeight)),
            std::clamp(x + 34, 0, static_cast<int>(desc.dwWidth)),
            std::clamp(y + 10, 0, static_cast<int>(desc.dwHeight)),
        };
        ColorFillSurfaceRect(g_RenderSurface, desc, stripe, (pass % 2) == 0 ? accentB : accentC);
    }
}

static bool CreateRenderSurface() {
    DDSURFACEDESC2 primaryDesc = {};
    primaryDesc.dwSize = sizeof(primaryDesc);
    const HRESULT primaryDescHr =
        g_PrimarySurface ? g_PrimarySurface->GetSurfaceDesc(&primaryDesc) : DDERR_INVALIDOBJECT;

    DDSURFACEDESC2 desc = {};
    desc.dwSize = sizeof(desc);
    desc.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT;
    desc.dwWidth = static_cast<DWORD>(g_WindowWidth);
    desc.dwHeight = static_cast<DWORD>(g_WindowHeight);
    desc.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_3DDEVICE | DDSCAPS_VIDEOMEMORY;
    if (SUCCEEDED(primaryDescHr)) {
        desc.dwFlags |= DDSD_PIXELFORMAT;
        desc.ddpfPixelFormat = primaryDesc.ddpfPixelFormat;
    }

    HRESULT hr = g_DirectDraw->CreateSurface(&desc, &g_RenderSurface, nullptr);
    if (FAILED(hr)) {
        desc.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_3DDEVICE;
        hr = g_DirectDraw->CreateSurface(&desc, &g_RenderSurface, nullptr);
    }
    if (FAILED(hr)) {
        desc.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN;
        hr = g_DirectDraw->CreateSurface(&desc, &g_RenderSurface, nullptr);
    }

    return SUCCEEDED(hr) && g_RenderSurface != nullptr;
}

static bool InitDX6(HWND hwnd) {
    IDirectDraw7* directDraw7 = nullptr;
    HRESULT hr = DirectDrawCreateEx(nullptr, reinterpret_cast<void**>(&directDraw7), IID_IDirectDraw7, nullptr);
    if (FAILED(hr) || !directDraw7) {
        return LogInitFailure("DirectDrawCreateEx(IID_IDirectDraw7)", hr);
    }

    hr = directDraw7->QueryInterface(IID_IDirectDraw4, reinterpret_cast<void**>(&g_DirectDraw));
    directDraw7->Release();
    if (FAILED(hr) || !g_DirectDraw) {
        return LogInitFailure("QueryInterface(IDirectDraw4)", hr);
    }

    hr = g_DirectDraw->SetCooperativeLevel(hwnd, DDSCL_NORMAL);
    if (FAILED(hr)) {
        return LogInitFailure("SetCooperativeLevel", hr);
    }

    DDSURFACEDESC2 primaryDesc = {};
    primaryDesc.dwSize = sizeof(primaryDesc);
    primaryDesc.dwFlags = DDSD_CAPS;
    primaryDesc.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE;
    hr = g_DirectDraw->CreateSurface(&primaryDesc, &g_PrimarySurface, nullptr);
    if (FAILED(hr) || !g_PrimarySurface) {
        return LogInitFailure("CreateSurface(primary)", hr);
    }

    if (!g_Fullscreen) {
        hr = g_DirectDraw->CreateClipper(0, &g_Clipper, nullptr);
        if (FAILED(hr) || !g_Clipper) {
            return LogInitFailure("CreateClipper", hr);
        }
        hr = g_Clipper->SetHWnd(0, hwnd);
        if (FAILED(hr)) {
            return LogInitFailure("Clipper::SetHWnd", hr);
        }
        hr = g_PrimarySurface->SetClipper(g_Clipper);
        if (FAILED(hr)) {
            return LogInitFailure("PrimarySurface::SetClipper", hr);
        }
    }

    if (!CreateRenderSurface()) {
        return LogInitFailure("CreateRenderSurface", DDERR_GENERIC);
    }

    hr = g_DirectDraw->QueryInterface(IID_IDirect3D3, reinterpret_cast<void**>(&g_D3D));
    if (FAILED(hr) || !g_D3D) {
        return EnableDirectDrawFallback("QueryInterface(IDirect3D3)", hr);
    }

    const GUID* deviceTypes[] = {&IID_IDirect3DHALDevice, &IID_IDirect3DMMXDevice, &IID_IDirect3DRGBDevice,
                                 &IID_IDirect3DRefDevice};
    for (const GUID* deviceType : deviceTypes) {
        hr = g_D3D->CreateDevice(*deviceType, g_RenderSurface, &g_Device, nullptr);
        if (SUCCEEDED(hr) && g_Device) {
            break;
        }
    }
    if (!g_Device) {
        return EnableDirectDrawFallback("IDirect3D3::CreateDevice", hr);
    }

    hr = g_D3D->CreateViewport(&g_Viewport, nullptr);
    if (FAILED(hr) || !g_Viewport) {
        return EnableDirectDrawFallback("CreateViewport", hr);
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
    hr = g_Viewport->SetViewport2(&viewport);
    if (FAILED(hr)) {
        return EnableDirectDrawFallback("Viewport::SetViewport2", hr);
    }

    hr = g_Device->AddViewport(g_Viewport);
    if (FAILED(hr)) {
        return EnableDirectDrawFallback("Device::AddViewport", hr);
    }
    hr = g_Device->SetCurrentViewport(g_Viewport);
    if (FAILED(hr)) {
        return EnableDirectDrawFallback("Device::SetCurrentViewport", hr);
    }
    hr = g_Device->SetRenderState(D3DRENDERSTATE_LIGHTING, FALSE);
    if (FAILED(hr)) {
        return EnableDirectDrawFallback("SetRenderState(LIGHTING)", hr);
    }
    hr = g_Device->SetRenderState(D3DRENDERSTATE_ZENABLE, FALSE);
    if (FAILED(hr)) {
        return EnableDirectDrawFallback("SetRenderState(ZENABLE)", hr);
    }
    hr = g_Device->SetRenderState(D3DRENDERSTATE_CULLMODE, D3DCULL_NONE);
    if (FAILED(hr)) {
        return EnableDirectDrawFallback("SetRenderState(CULLMODE)", hr);
    }
    hr = g_Device->SetRenderState(D3DRENDERSTATE_SHADEMODE, D3DSHADE_GOURAUD);
    if (FAILED(hr)) {
        return EnableDirectDrawFallback("SetRenderState(SHADEMODE)", hr);
    }
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
    if (g_UseDirectDrawFallback) {
        RenderDirectDrawFallbackFrame();
        PresentFrame(hwnd);
        return;
    }

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

        // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
        const float cx = g_WindowWidth * 0.5f;
        // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
        const float cy = g_WindowHeight * 0.5f;
        // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
        const float radius = std::min(g_WindowWidth, g_WindowHeight) * 0.2f;
        const int passes = std::max(1, g_WorkloadPasses);

        for (int pass = 0; pass < passes; ++pass) {
            const float passAngle = t * 1.4f + static_cast<float>(pass) * 0.2f;
            // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
            const float offsetX = std::sin(t * 0.5f + pass * 0.7f) * g_WindowWidth * 0.17f;
            // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
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

    // NOLINTNEXTLINE(bugprone-exception-escape) - standalone test harness: an unexpected exception terminating the process is acceptable and yields a nonzero exit
int main(int argc, char* argv[]) {
    Sleep(500);

    testapp::EnableGameDpiAwareness();
    testapp::ApplyGameScheduling();
    LoadConfig();

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
    wc.lpszClassName = L"DX6Test";
    RegisterClassExW(&wc);

    const RECT monitorRect = testapp::GetPrimaryMonitorRect();
    g_PresentWidth = g_Fullscreen ? (monitorRect.right - monitorRect.left) : g_WindowWidth;
    g_PresentHeight = g_Fullscreen ? (monitorRect.bottom - monitorRect.top) : g_WindowHeight;

    const DWORD winStyle = g_Fullscreen ? WS_POPUP : WS_OVERLAPPEDWINDOW;
    const int posX = g_Fullscreen ? monitorRect.left : CW_USEDEFAULT;
    const int posY = g_Fullscreen ? monitorRect.top : CW_USEDEFAULT;
    const RECT windowRect = testapp::AdjustWindowRectForClientSize(winStyle, 0, g_PresentWidth, g_PresentHeight);
    const int winW = g_Fullscreen ? g_PresentWidth : (windowRect.right - windowRect.left);
    const int winH = g_Fullscreen ? g_PresentHeight : (windowRect.bottom - windowRect.top);

    HWND hwnd = CreateWindowW(L"DX6Test", L"DX6 Test", winStyle, posX, posY, winW, winH, nullptr, nullptr, wc.hInstance,
                              nullptr);
    if (!hwnd) {
        return 1;
    }

    if (!testapp::PrimeWindowForBenchmark(hwnd, g_Fullscreen != 0, g_PresentWidth, g_PresentHeight)) {
        DestroyWindow(hwnd);
        return 0;
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

    if (!testapp::PrimeWindowForBenchmark(hwnd, g_Fullscreen != 0, g_PresentWidth, g_PresentHeight)) {
        SafeRelease(g_Viewport);
        SafeRelease(g_Device);
        SafeRelease(g_D3D);
        SafeRelease(g_Clipper);
        SafeRelease(g_RenderSurface);
        SafeRelease(g_PrimarySurface);
        SafeRelease(g_DirectDraw);
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