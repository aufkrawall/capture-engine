// DirectDraw 7 Test App for legacy surface-flip capture validation
#define WIN32_LEAN_AND_MEAN
#define WINVER 0x0A00
#define _WIN32_WINNT 0x0A00
#define DIRECTDRAW_VERSION 0x0700
// clang-format off
#include <windows.h>
#include <avrt.h>
// clang-format on
#include <ddraw.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

static int g_WindowWidth = 1024;
static int g_WindowHeight = 768;
static int g_PresentWidth = 1024;
static int g_PresentHeight = 768;
static int g_WorkloadPasses = 8;
static int g_VSync = 0;
static int g_Fullscreen = 1;
static bool g_ExclusiveFullscreen = false;
static bool g_DesktopBlitFullscreen = false;
static bool g_WindowActive = true;
static bool g_ResetPending = false;
static bool g_Running = true;
static float g_BarPosition = 0.0f;
static auto g_StartTime = std::chrono::high_resolution_clock::now();

static IDirectDraw7* g_DirectDraw = nullptr;
static IDirectDrawSurface7* g_PrimarySurface = nullptr;
static IDirectDrawSurface7* g_BackBuffer = nullptr;
static IDirectDrawSurface7* g_RenderSurface = nullptr;
static IDirectDrawClipper* g_Clipper = nullptr;

template <typename T>
static void SafeRelease(T*& ptr) {
    if (ptr) {
        ptr->Release();
        ptr = nullptr;
    }
}

static void LoadConfig() {
    char path[MAX_PATH];
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    std::string configPath = path;
    size_t pos = configPath.find_last_of("\\/");
    if (pos != std::string::npos)
        configPath = configPath.substr(0, pos + 1) + "testappconfig.ini";

    g_WindowWidth = GetPrivateProfileIntA("Display", "width", g_WindowWidth, configPath.c_str());
    g_WindowHeight = GetPrivateProfileIntA("Display", "height", g_WindowHeight, configPath.c_str());
    g_WorkloadPasses = GetPrivateProfileIntA("Performance", "gpu_load", g_WorkloadPasses, configPath.c_str());
    g_VSync = GetPrivateProfileIntA("Rendering", "vsync", g_VSync, configPath.c_str());
    g_Fullscreen = GetPrivateProfileIntA("Display", "fullscreen", g_Fullscreen, configPath.c_str());
}

static bool NeedsDirectDrawReset(HRESULT hr) {
    return hr == DDERR_SURFACELOST || hr == DDERR_WRONGMODE || hr == DDERR_INVALIDMODE || hr == DDERR_NOEXCLUSIVEMODE;
}

static bool IsDirectDrawBusy(HRESULT hr) {
    return hr == DDERR_WASSTILLDRAWING || hr == DDERR_SURFACEBUSY;
}

static void PumpStartupMessagesForMs(DWORD durationMs) {
    const uint64_t deadline = GetTickCount64() + durationMs;
    MSG msg = {};
    while (g_Running && GetTickCount64() < deadline) {
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
            if (!g_Running)
                return;
        }
        Sleep(10);
    }
}

static void QueueDirectDrawReset(const char* reason) {
    if (!g_ResetPending) {
        printf("DirectDraw7: queued presentation reset (%s) [active=%d exclusive=%d desktopBlit=%d fullscreen=%d]\n",
               reason ? reason : "unknown", g_WindowActive ? 1 : 0, g_ExclusiveFullscreen ? 1 : 0,
               g_DesktopBlitFullscreen ? 1 : 0, g_Fullscreen ? 1 : 0);
    }
    g_ResetPending = true;
}

static void UpdateWindowActiveState(bool active, const char* reason) {
    const bool wasActive = g_WindowActive;
    g_WindowActive = active;

    if (!g_Fullscreen || wasActive == active) {
        return;
    }

    if (!active) {
        if (g_ExclusiveFullscreen) {
            QueueDirectDrawReset(reason);
        }
        return;
    }

    if (!g_ExclusiveFullscreen) {
        QueueDirectDrawReset(reason);
    }
}

static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
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
    if (msg == WM_ACTIVATEAPP) {
        UpdateWindowActiveState(wParam != FALSE, (wParam != FALSE) ? "activateapp gain" : "activateapp loss");
        return 0;
    }
    if (msg == WM_ACTIVATE) {
        UpdateWindowActiveState(LOWORD(wParam) != WA_INACTIVE,
                                (LOWORD(wParam) != WA_INACTIVE) ? "activate gain" : "activate loss");
        return 0;
    }
    if (msg == WM_KILLFOCUS) {
        UpdateWindowActiveState(false, "kill focus");
        return 0;
    }
    if (msg == WM_SETFOCUS) {
        UpdateWindowActiveState(true, "set focus");
        return 0;
    }
    if (msg == WM_SIZE) {
        if (wParam == SIZE_MINIMIZED) {
            UpdateWindowActiveState(false, "minimized");
        } else {
            g_WindowActive = true;
        }
        return 0;
    }
    if (msg == WM_DISPLAYCHANGE && g_Fullscreen) {
        QueueDirectDrawReset("display change");
        return 0;
    }
    return DefWindowProc(hWnd, msg, wParam, lParam);
}

static uint32_t PackChannel(uint32_t mask, uint8_t value) {
    if (mask == 0)
        return 0;

    uint32_t shift = 0;
    while (((mask >> shift) & 1U) == 0U)
        ++shift;

    uint32_t maxValue = mask >> shift;
    uint32_t scaled = (static_cast<uint32_t>(value) * maxValue + 127U) / 255U;
    return (scaled << shift) & mask;
}

static uint32_t PackColor(const DDPIXELFORMAT& pixelFormat, uint8_t red, uint8_t green, uint8_t blue) {
    return PackChannel(pixelFormat.dwRBitMask, red) | PackChannel(pixelFormat.dwGBitMask, green) |
           PackChannel(pixelFormat.dwBBitMask, blue);
}

static void StorePixel(uint8_t* dest, int bytesPerPixel, uint32_t packedColor) {
    switch (bytesPerPixel) {
        case 2:
            *reinterpret_cast<uint16_t*>(dest) = static_cast<uint16_t>(packedColor);
            break;
        case 3:
            dest[0] = static_cast<uint8_t>(packedColor & 0xFF);
            dest[1] = static_cast<uint8_t>((packedColor >> 8) & 0xFF);
            dest[2] = static_cast<uint8_t>((packedColor >> 16) & 0xFF);
            break;
        default:
            *reinterpret_cast<uint32_t*>(dest) = packedColor;
            break;
    }
}

static void FillSurfaceRect(DDSURFACEDESC2& desc, const RECT& rect, uint32_t packedColor) {
    const int bytesPerPixel = std::max(1, static_cast<int>(desc.ddpfPixelFormat.dwRGBBitCount / 8U));
    const int width = static_cast<int>(desc.dwWidth);
    const int height = static_cast<int>(desc.dwHeight);
    const int left = std::clamp(static_cast<int>(rect.left), 0, width);
    const int right = std::clamp(static_cast<int>(rect.right), 0, width);
    const int top = std::clamp(static_cast<int>(rect.top), 0, height);
    const int bottom = std::clamp(static_cast<int>(rect.bottom), 0, height);

    if (left >= right || top >= bottom)
        return;

    for (int y = top; y < bottom; ++y) {
        uint8_t* row = static_cast<uint8_t*>(desc.lpSurface) + static_cast<ptrdiff_t>(y) * desc.lPitch;
        if (bytesPerPixel == 4) {
            auto* pixels = reinterpret_cast<uint32_t*>(row) + left;
            std::fill(pixels, pixels + (right - left), packedColor);
            continue;
        }
        if (bytesPerPixel == 2) {
            auto* pixels = reinterpret_cast<uint16_t*>(row) + left;
            std::fill(pixels, pixels + (right - left), static_cast<uint16_t>(packedColor));
            continue;
        }

        for (int x = left; x < right; ++x)
            StorePixel(row + x * bytesPerPixel, bytesPerPixel, packedColor);
    }
}

static void DrawFrameToSurface(IDirectDrawSurface7* surface) {
    if (!surface)
        return;

    DDSURFACEDESC2 desc = {};
    desc.dwSize = sizeof(desc);
    HRESULT lockHr = surface->Lock(nullptr, &desc, DDLOCK_SURFACEMEMORYPTR, nullptr);
    if (FAILED(lockHr)) {
        if (IsDirectDrawBusy(lockHr))
            return;
        if (NeedsDirectDrawReset(lockHr))
            g_ResetPending = true;
        return;
    }

    auto now = std::chrono::high_resolution_clock::now();
    float elapsed = std::chrono::duration<float>(now - g_StartTime).count();
    g_BarPosition = static_cast<float>(std::fmod(elapsed * 0.30f, 1.0f));

    const uint32_t background = PackColor(desc.ddpfPixelFormat, 22, 24, 40);
    const uint32_t border = PackColor(desc.ddpfPixelFormat, 186, 184, 210);
    const uint32_t highlight = PackColor(desc.ddpfPixelFormat, 248, 242, 218);
    const uint32_t accentA = PackColor(desc.ddpfPixelFormat, 92, 170, 255);
    const uint32_t accentB = PackColor(desc.ddpfPixelFormat, 120, 255, 148);
    const uint32_t accentC = PackColor(desc.ddpfPixelFormat, 255, 110, 110);

    RECT fullRect = {0, 0, static_cast<LONG>(desc.dwWidth), static_cast<LONG>(desc.dwHeight)};
    FillSurfaceRect(desc, fullRect, background);

    RECT borderTop = {16, 16, static_cast<LONG>(desc.dwWidth) - 16, 20};
    RECT borderBottom = {16, static_cast<LONG>(desc.dwHeight) - 20, static_cast<LONG>(desc.dwWidth) - 16,
                         static_cast<LONG>(desc.dwHeight) - 16};
    RECT borderLeft = {16, 16, 20, static_cast<LONG>(desc.dwHeight) - 16};
    RECT borderRight = {static_cast<LONG>(desc.dwWidth) - 20, 16, static_cast<LONG>(desc.dwWidth) - 16,
                        static_cast<LONG>(desc.dwHeight) - 16};
    FillSurfaceRect(desc, borderTop, border);
    FillSurfaceRect(desc, borderBottom, border);
    FillSurfaceRect(desc, borderLeft, border);
    FillSurfaceRect(desc, borderRight, border);

    const LONG barWidth = 180;
    const LONG barX = static_cast<LONG>(g_BarPosition * static_cast<float>(std::max<int>(1, desc.dwWidth - barWidth)));
    const LONG barY = static_cast<LONG>(desc.dwHeight / 2) - 44;
    RECT movingBar = {barX, barY, barX + barWidth, barY + 88};
    FillSurfaceRect(desc, movingBar, highlight);

    RECT centerBox = {static_cast<LONG>(desc.dwWidth / 2) - 96, static_cast<LONG>(desc.dwHeight / 3) - 64,
                      static_cast<LONG>(desc.dwWidth / 2) + 96, static_cast<LONG>(desc.dwHeight / 3) + 64};
    FillSurfaceRect(desc, centerBox, accentA);

    for (int pass = 0; pass < g_WorkloadPasses; ++pass) {
        int x = (pass * 97 + static_cast<int>(elapsed * 120.0f)) % std::max<int>(1, desc.dwWidth);
        int y = (pass * 53 + static_cast<int>(elapsed * 90.0f)) % std::max<int>(1, desc.dwHeight);
        RECT stripe = {
            std::clamp(x - 36, 0, static_cast<int>(desc.dwWidth)),
            std::clamp(y - 10, 0, static_cast<int>(desc.dwHeight)),
            std::clamp(x + 36, 0, static_cast<int>(desc.dwWidth)),
            std::clamp(y + 10, 0, static_cast<int>(desc.dwHeight)),
        };
        FillSurfaceRect(desc, stripe, (pass % 2) == 0 ? accentB : accentC);
    }

    surface->Unlock(nullptr);

    static auto lastLog = now;
    static int frames = 0;
    ++frames;
    if (std::chrono::duration<float>(now - lastLog).count() >= 2.0f) {
        printf("DirectDraw7 FPS: %.2f\n", frames / 2.0f);
        frames = 0;
        lastLog = now;
    }
}

static void CleanupDirectDraw(HWND hwnd) {
    if (g_DirectDraw && g_ExclusiveFullscreen) {
        g_DirectDraw->FlipToGDISurface();
        g_DirectDraw->RestoreDisplayMode();
        if (hwnd)
            g_DirectDraw->SetCooperativeLevel(hwnd, DDSCL_NORMAL);
    }

    SafeRelease(g_Clipper);
    SafeRelease(g_RenderSurface);
    SafeRelease(g_BackBuffer);
    SafeRelease(g_PrimarySurface);
    SafeRelease(g_DirectDraw);
}

static bool InitWindowedPrimary(HWND hwnd) {
    DDSURFACEDESC2 primaryDesc = {};
    primaryDesc.dwSize = sizeof(primaryDesc);
    primaryDesc.dwFlags = DDSD_CAPS;
    primaryDesc.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE;
    if (FAILED(g_DirectDraw->CreateSurface(&primaryDesc, &g_PrimarySurface, nullptr)))
        return false;

    if (FAILED(g_DirectDraw->CreateClipper(0, &g_Clipper, nullptr)))
        return false;
    if (FAILED(g_Clipper->SetHWnd(0, hwnd)))
        return false;
    if (FAILED(g_PrimarySurface->SetClipper(g_Clipper)))
        return false;

    g_BackBuffer = g_PrimarySurface;
    g_BackBuffer->AddRef();
    return true;
}

static bool InitFullscreenPrimary() {
    if (FAILED(g_DirectDraw->SetDisplayMode(g_WindowWidth, g_WindowHeight, 32, 0, 0)))
        return false;

    g_PresentWidth = g_WindowWidth;
    g_PresentHeight = g_WindowHeight;
    g_ExclusiveFullscreen = true;

    DDSURFACEDESC2 primaryDesc = {};
    primaryDesc.dwSize = sizeof(primaryDesc);
    primaryDesc.dwFlags = DDSD_CAPS | DDSD_BACKBUFFERCOUNT;
    primaryDesc.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE | DDSCAPS_FLIP | DDSCAPS_COMPLEX;
    primaryDesc.dwBackBufferCount = 1;
    if (FAILED(g_DirectDraw->CreateSurface(&primaryDesc, &g_PrimarySurface, nullptr)))
        return false;

    DDSCAPS2 backBufferCaps = {};
    backBufferCaps.dwCaps = DDSCAPS_BACKBUFFER;
    return SUCCEEDED(g_PrimarySurface->GetAttachedSurface(&backBufferCaps, &g_BackBuffer));
}

static bool InitDesktopFullscreenPrimary(HWND hwnd) {
    SafeRelease(g_Clipper);
    SafeRelease(g_BackBuffer);
    SafeRelease(g_PrimarySurface);
    g_DirectDraw->FlipToGDISurface();
    g_DirectDraw->RestoreDisplayMode();
    if (FAILED(g_DirectDraw->SetCooperativeLevel(hwnd, DDSCL_NORMAL)))
        return false;

    g_ExclusiveFullscreen = false;
    g_DesktopBlitFullscreen = true;
    g_PresentWidth = GetSystemMetrics(SM_CXSCREEN);
    g_PresentHeight = GetSystemMetrics(SM_CYSCREEN);
    SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, g_PresentWidth, g_PresentHeight, SWP_SHOWWINDOW);
    return InitWindowedPrimary(hwnd);
}

static bool InitRenderSurface() {
    DDSURFACEDESC2 renderDesc = {};
    renderDesc.dwSize = sizeof(renderDesc);
    renderDesc.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
    renderDesc.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_SYSTEMMEMORY;
    renderDesc.dwWidth = g_WindowWidth;
    renderDesc.dwHeight = g_WindowHeight;
    renderDesc.ddpfPixelFormat.dwSize = sizeof(renderDesc.ddpfPixelFormat);
    renderDesc.ddpfPixelFormat.dwFlags = DDPF_RGB;
    renderDesc.ddpfPixelFormat.dwRGBBitCount = 32;
    renderDesc.ddpfPixelFormat.dwRBitMask = 0x00FF0000;
    renderDesc.ddpfPixelFormat.dwGBitMask = 0x0000FF00;
    renderDesc.ddpfPixelFormat.dwBBitMask = 0x000000FF;
    if (SUCCEEDED(g_DirectDraw->CreateSurface(&renderDesc, &g_RenderSurface, nullptr)))
        return true;

    renderDesc = {};
    renderDesc.dwSize = sizeof(renderDesc);
    renderDesc.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT;
    renderDesc.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_SYSTEMMEMORY;
    renderDesc.dwWidth = g_WindowWidth;
    renderDesc.dwHeight = g_WindowHeight;
    return SUCCEEDED(g_DirectDraw->CreateSurface(&renderDesc, &g_RenderSurface, nullptr));
}

static bool InitDirectDraw(HWND hwnd) {
    if (FAILED(DirectDrawCreateEx(nullptr, reinterpret_cast<void**>(&g_DirectDraw), IID_IDirectDraw7, nullptr)))
        return false;

    g_ExclusiveFullscreen = false;
    g_DesktopBlitFullscreen = false;
    g_PresentWidth = g_WindowWidth;
    g_PresentHeight = g_WindowHeight;

    if (g_Fullscreen) {
        if (!g_WindowActive) {
            if (!InitDesktopFullscreenPrimary(hwnd))
                return false;
        } else {
            DWORD coopFlags = DDSCL_FULLSCREEN | DDSCL_EXCLUSIVE | DDSCL_ALLOWREBOOT;
            if (FAILED(g_DirectDraw->SetCooperativeLevel(hwnd, coopFlags)))
                return false;
            if (!InitFullscreenPrimary()) {
                printf("DirectDraw7: Exclusive %dx%d mode unavailable, using desktop blit fallback for render %dx%d\n",
                       g_WindowWidth, g_WindowHeight, g_WindowWidth, g_WindowHeight);
                if (!InitDesktopFullscreenPrimary(hwnd))
                    return false;
            }
        }
    } else {
        if (FAILED(g_DirectDraw->SetCooperativeLevel(hwnd, DDSCL_NORMAL)))
            return false;
        if (!InitWindowedPrimary(hwnd))
            return false;
    }

    return InitRenderSurface();
}

static bool ResetDirectDraw(HWND hwnd) {
    CleanupDirectDraw(hwnd);
    if (!InitDirectDraw(hwnd))
        return false;

    printf("DirectDraw7: reset presentation surfaces after focus/display change (%s)\n",
           g_ExclusiveFullscreen ? "fullscreen flip chain"
                                 : (g_DesktopBlitFullscreen ? "desktop fullscreen blit" : "windowed blit"));
    return true;
}

static void RefreshWindowActivity(HWND hwnd) {
    const bool wasActive = g_WindowActive;
    if (!IsWindow(hwnd) || !IsWindowVisible(hwnd) || IsIconic(hwnd)) {
        g_WindowActive = false;
    } else {
        HWND foreground = GetForegroundWindow();
        if (!foreground) {
            g_WindowActive = false;
        } else if (foreground == hwnd) {
            g_WindowActive = true;
        } else {
            DWORD foregroundPid = 0;
            GetWindowThreadProcessId(foreground, &foregroundPid);
            g_WindowActive = (foregroundPid == GetCurrentProcessId());
        }
    }

    if (!g_Fullscreen || wasActive == g_WindowActive) {
        return;
    }

    if (!g_WindowActive) {
        if (g_ExclusiveFullscreen) {
            QueueDirectDrawReset("foreground change loss");
        }
        return;
    }

    if (!g_ExclusiveFullscreen) {
        QueueDirectDrawReset("foreground change gain");
    }
}

static void PresentFrame(HWND hwnd) {
    if (!g_WindowActive || g_ResetPending || !g_RenderSurface || !g_BackBuffer || !g_PrimarySurface)
        return;

    DrawFrameToSurface(g_RenderSurface);

    if (g_ExclusiveFullscreen) {
        HRESULT hr = g_BackBuffer->Blt(nullptr, g_RenderSurface, nullptr, 0, nullptr);
        if (IsDirectDrawBusy(hr))
            return;
        if (NeedsDirectDrawReset(hr)) {
            g_ResetPending = true;
            return;
        }
        if (FAILED(hr))
            return;
        DWORD flipFlags = 0;
        if (!g_VSync)
            flipFlags |= DDFLIP_NOVSYNC;
        hr = g_PrimarySurface->Flip(nullptr, flipFlags);
        if (IsDirectDrawBusy(hr))
            return;
        if (NeedsDirectDrawReset(hr))
            g_ResetPending = true;
        return;
    }

    RECT clientRect = {};
    GetClientRect(hwnd, &clientRect);
    POINT topLeft = {clientRect.left, clientRect.top};
    POINT bottomRight = {clientRect.right, clientRect.bottom};
    ClientToScreen(hwnd, &topLeft);
    ClientToScreen(hwnd, &bottomRight);
    RECT destRect = {topLeft.x, topLeft.y, bottomRight.x, bottomRight.y};
    RECT sourceRect = {0, 0, g_WindowWidth, g_WindowHeight};
    HRESULT hr = g_PrimarySurface->Blt(&destRect, g_RenderSurface, &sourceRect, 0, nullptr);
    if (IsDirectDrawBusy(hr))
        return;
    if (NeedsDirectDrawReset(hr))
        g_ResetPending = true;
}

int main(int argc, char* argv[]) {
    LoadConfig();

    PROCESS_POWER_THROTTLING_STATE pts = {};
    pts.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    pts.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
    pts.StateMask = 0;
    SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &pts, sizeof(pts));
    SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS);
    DWORD mmcssTaskIndex = 0;
    HANDLE mmcssHandle = AvSetMmThreadCharacteristics(TEXT("Games"), &mmcssTaskIndex);
    if (mmcssHandle)
        AvSetMmThreadPriority(mmcssHandle, AVRT_PRIORITY_HIGH);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);

    if (argc >= 3) {
        g_WindowWidth = atoi(argv[1]);
        g_WindowHeight = atoi(argv[2]);
    }
    if (argc >= 4) {
        g_WorkloadPasses = atoi(argv[3]);
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
                      L"CaptureTestDirectDraw7",
                      nullptr};
    RegisterClassExW(&wc);

    g_PresentWidth = g_Fullscreen ? GetSystemMetrics(SM_CXSCREEN) : g_WindowWidth;
    g_PresentHeight = g_Fullscreen ? GetSystemMetrics(SM_CYSCREEN) : g_WindowHeight;

    DWORD winStyle = g_Fullscreen ? (WS_POPUP | WS_VISIBLE) : (WS_OVERLAPPEDWINDOW | WS_VISIBLE);
    int posX = g_Fullscreen ? 0 : CW_USEDEFAULT;
    int posY = g_Fullscreen ? 0 : CW_USEDEFAULT;
    int winW = g_PresentWidth;
    int winH = g_PresentHeight;
    if (!g_Fullscreen) {
        RECT wr = {0, 0, g_WindowWidth, g_WindowHeight};
        AdjustWindowRect(&wr, WS_OVERLAPPEDWINDOW, FALSE);
        winW = wr.right - wr.left;
        winH = wr.bottom - wr.top;
    }

    HWND hwnd = CreateWindowW(L"CaptureTestDirectDraw7", L"DirectDraw7 Test", winStyle, posX, posY, winW, winH, nullptr,
                              nullptr, wc.hInstance, nullptr);
    if (!hwnd)
        return 1;

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    printf("DirectDraw7: startup warmup before DirectDraw init (750 ms)\n");
    PumpStartupMessagesForMs(750);
    if (!g_Running) {
        DestroyWindow(hwnd);
        return 0;
    }

    if (!InitDirectDraw(hwnd)) {
        CleanupDirectDraw(hwnd);
        DestroyWindow(hwnd);
        return 1;
    }
    g_WindowActive = true;
    g_ResetPending = false;

    const char* presentMode = g_ExclusiveFullscreen
                                  ? "fullscreen flip chain"
                                  : (g_DesktopBlitFullscreen ? "desktop fullscreen blit" : "windowed blit");
    printf("Running DirectDraw7 test render=%dx%d present=%dx%d (%s)\n", g_WindowWidth, g_WindowHeight, g_PresentWidth,
           g_PresentHeight, presentMode);

    MSG msg = {};
    while (g_Running) {
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        if (!g_Running)
            break;
        RefreshWindowActivity(hwnd);
        if (g_ResetPending) {
            if (!ResetDirectDraw(hwnd)) {
                fprintf(stderr, "DirectDraw7: failed to recover after focus/display change\n");
                g_Running = false;
                break;
            }
            g_ResetPending = false;
        }
        if (!g_WindowActive) {
            Sleep(50);
            continue;
        }
        PresentFrame(hwnd);
    }

    CleanupDirectDraw(hwnd);
    DestroyWindow(hwnd);
    return 0;
}
