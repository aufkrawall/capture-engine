// DX11 Test App for Capture + FPS Limiter Testing
#define WIN32_LEAN_AND_MEAN
#define WINVER 0x0A00
#define _WIN32_WINNT 0x0A00
// clang-format off
#include <windows.h>
#include <avrt.h>
// clang-format on
#include <d3d11.h>
#include <d3d11_1.h>
#include <dxgi.h>
#include <dxgi1_3.h>
#include <shellscalingapi.h>
#include <wrl/client.h>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

#include "testapp_common.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "shcore.lib")

using Microsoft::WRL::ComPtr;

static int g_WindowWidth = 1920;
static int g_WindowHeight = 1080;
static int g_GpuLoadPasses = 10;
static int g_VSync = 0;
static int g_Fullscreen = 1;

void LoadConfig() {
    char path[MAX_PATH];
    GetModuleFileNameA(NULL, path, MAX_PATH);
    std::string configPath = path;
    size_t pos = configPath.find_last_of("\\/");
    if (pos != std::string::npos)
        configPath = configPath.substr(0, pos + 1) + "testappconfig.ini";

    g_WindowWidth = GetPrivateProfileIntA("Display", "width", g_WindowWidth, configPath.c_str());
    g_WindowHeight = GetPrivateProfileIntA("Display", "height", g_WindowHeight, configPath.c_str());
    g_GpuLoadPasses = GetPrivateProfileIntA("Performance", "gpu_load", g_GpuLoadPasses, configPath.c_str());
    g_VSync = GetPrivateProfileIntA("Rendering", "vsync", g_VSync, configPath.c_str());
    g_Fullscreen = GetPrivateProfileIntA("Display", "fullscreen", g_Fullscreen, configPath.c_str());
}

const wchar_t* WINDOW_CLASS = L"CaptureTestDX11";
ComPtr<ID3D11Device> g_Device;
ComPtr<ID3D11DeviceContext> g_Context;
ComPtr<ID3D11DeviceContext1> g_Context1;
ComPtr<IDXGISwapChain> g_SwapChain;
ComPtr<ID3D11RenderTargetView> g_Rtv;

float g_BarPosition = 0.0f;
auto g_StartTime = std::chrono::high_resolution_clock::now();
bool g_Running = true;

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

bool InitDX11(HWND hwnd) {
    D3D_FEATURE_LEVEL featureLevels[] = {D3D_FEATURE_LEVEL_11_0};
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, featureLevels, 1, D3D11_SDK_VERSION,
                                 &g_Device, nullptr, &g_Context)))
        return false;

    // Get IDXGIFactory2 from the device for flip-model swap chain creation
    ComPtr<IDXGIDevice> dxgiDevice;
    g_Device.As(&dxgiDevice);
    ComPtr<IDXGIAdapter> adapter;
    dxgiDevice->GetAdapter(&adapter);
    ComPtr<IDXGIFactory2> factory2;
    adapter->GetParent(IID_PPV_ARGS(&factory2));

    DXGI_SWAP_CHAIN_DESC1 sd = {};
    sd.BufferCount = 2;
    sd.Width = g_WindowWidth;
    sd.Height = g_WindowHeight;
    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.SampleDesc.Count = 1;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    ComPtr<IDXGISwapChain1> swapChain1;
    if (FAILED(factory2->CreateSwapChainForHwnd(g_Device.Get(), hwnd, &sd, nullptr, nullptr, &swapChain1)))
        return false;
    factory2->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
    swapChain1.As(&g_SwapChain);

    ComPtr<IDXGISwapChain2> swapChain2;
    swapChain1.As(&swapChain2);
    swapChain2->SetMaximumFrameLatency(1);

    // Optional: Use D3D11.1 ClearView to clear a sub-rect (needed for moving bar)
    g_Context.As(&g_Context1);

    ComPtr<ID3D11Texture2D> backBuffer;
    g_SwapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
    g_Device->CreateRenderTargetView(backBuffer.Get(), nullptr, &g_Rtv);
    return true;
}

void Render() {
    auto now = std::chrono::high_resolution_clock::now();
    float elapsed = std::chrono::duration<float>(now - g_StartTime).count();
    g_BarPosition = (float)std::fmod((double)(elapsed * 0.5f), 1.0);

    ID3D11RenderTargetView* rtvs[] = {g_Rtv.Get()};
    g_Context->OMSetRenderTargets(1, rtvs, nullptr);

    float clearColor[] = {0.1f, 0.1f, 0.1f, 1.0f};
    g_Context->ClearRenderTargetView(g_Rtv.Get(), clearColor);

    // GPU Load
    for (int i = 0; i < g_GpuLoadPasses; i++) {
        float loadColor[] = {0.1f + (i % 2) * 0.01f, 0.1f, 0.1f, 1.0f};
        g_Context->ClearRenderTargetView(g_Rtv.Get(), loadColor);
    }
    g_Context->ClearRenderTargetView(g_Rtv.Get(), clearColor);

    D3D11_RECT rect = {(LONG)(g_BarPosition * (g_WindowWidth - 100)), g_WindowHeight / 2 - 50,
                       (LONG)(g_BarPosition * (g_WindowWidth - 100) + 100), g_WindowHeight / 2 + 50};
    float barColor[] = {1.0f, 1.0f, 1.0f, 1.0f};
    if (g_Context1) {
        // Clear only the rectangle region (this creates the visible moving bar)
        g_Context1->ClearView(g_Rtv.Get(), barColor, &rect, 1);
    } else {
        // Fallback: without ClearView we can't clear sub-rects with
        // ClearRenderTargetView. Keep a full-screen clear so the app still displays
        // something.
        g_Context->ClearRenderTargetView(g_Rtv.Get(), barColor);
    }

    g_SwapChain->Present(g_VSync, 0);
}

int main(int argc, char* argv[]) {
    testapp::EnableGameDpiAwareness();
    testapp::ApplyGameScheduling();

    LoadConfig();
    if (argc >= 3) {
        g_WindowWidth = atoi(argv[1]);
        g_WindowHeight = atoi(argv[2]);
    }
    if (argc >= 4) {
        g_GpuLoadPasses = atoi(argv[3]);
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
                      WINDOW_CLASS,
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
    HWND hwnd = CreateWindowW(WINDOW_CLASS, L"DX11 Test", winStyle, posX, posY, winW, winH, nullptr, nullptr,
                              wc.hInstance, nullptr);
    if (!testapp::PrimeWindowForBenchmark(hwnd, g_Fullscreen != 0, g_WindowWidth, g_WindowHeight))
        return 0;
    if (!InitDX11(hwnd))
        return 1;
    if (!testapp::PrimeWindowForBenchmark(hwnd, g_Fullscreen != 0, g_WindowWidth, g_WindowHeight))
        return 0;

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
    return 0;
}
