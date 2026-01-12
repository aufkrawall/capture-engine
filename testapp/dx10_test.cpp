#define WIN32_LEAN_AND_MEAN
#define WINVER 0x0A00
#define _WIN32_WINNT 0x0A00

#include <chrono>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <d3d10.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <shellscalingapi.h>
#include <windows.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

static int g_WindowWidth = 1920;
static int g_WindowHeight = 1080;
static int g_GpuLoadPasses = 10;
static int g_VSync = 0;

static const wchar_t* WINDOW_CLASS = L"CaptureTestDX10";

static ComPtr<ID3D10Device> g_Device;
static ComPtr<IDXGISwapChain> g_SwapChain;
static ComPtr<ID3D10RenderTargetView> g_Rtv;
static ComPtr<ID3D10VertexShader> g_VS;
static ComPtr<ID3D10PixelShader> g_PS;
static ComPtr<ID3D10InputLayout> g_InputLayout;
static ComPtr<ID3D10Buffer> g_VB;

static float g_BarPosition = 0.0f;
static auto g_StartTime = std::chrono::high_resolution_clock::now();
static bool g_Running = true;

struct Vertex {
    float pos[2];
    float color[4];
};

static void LoadConfig() {
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
}

static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_DESTROY) { g_Running = false; PostQuitMessage(0); return 0; }
    if (msg == WM_KEYDOWN && wParam == VK_ESCAPE) { g_Running = false; DestroyWindow(hWnd); return 0; }
    return DefWindowProc(hWnd, msg, wParam, lParam);
}

static bool InitDX10(HWND hwnd) {
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 2;
    sd.BufferDesc.Width = g_WindowWidth;
    sd.BufferDesc.Height = g_WindowHeight;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    UINT flags = D3D10_CREATE_DEVICE_BGRA_SUPPORT;

    HRESULT hr = D3D10CreateDeviceAndSwapChain(
        nullptr,
        D3D10_DRIVER_TYPE_HARDWARE,
        nullptr,
        flags,
        D3D10_SDK_VERSION,
        &sd,
        &g_SwapChain,
        &g_Device);

    if (FAILED(hr)) return false;

    ComPtr<ID3D10Texture2D> backBuffer;
    hr = g_SwapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
    if (FAILED(hr)) return false;

    hr = g_Device->CreateRenderTargetView(backBuffer.Get(), nullptr, &g_Rtv);
    if (FAILED(hr)) return false;

    const char* vsSrc =
        "struct VSIn { float2 pos : POSITION; float4 col : COLOR; };"
        "struct VSOut { float4 pos : SV_POSITION; float4 col : COLOR; };"
        "VSOut main(VSIn i){ VSOut o; o.pos=float4(i.pos,0,1); o.col=i.col; return o; }";

    const char* psSrc =
        "struct PSIn { float4 pos : SV_POSITION; float4 col : COLOR; };"
        "float4 main(PSIn i) : SV_Target { return i.col; }";

    ComPtr<ID3DBlob> vsBlob;
    ComPtr<ID3DBlob> psBlob;
    ComPtr<ID3DBlob> err;

    hr = D3DCompile(vsSrc, strlen(vsSrc), nullptr, nullptr, nullptr, "main", "vs_4_0", 0, 0, &vsBlob, &err);
    if (FAILED(hr)) return false;

    hr = D3DCompile(psSrc, strlen(psSrc), nullptr, nullptr, nullptr, "main", "ps_4_0", 0, 0, &psBlob, &err);
    if (FAILED(hr)) return false;

    hr = g_Device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), &g_VS);
    if (FAILED(hr)) return false;

    hr = g_Device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), &g_PS);
    if (FAILED(hr)) return false;

    D3D10_INPUT_ELEMENT_DESC layout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D10_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 8, D3D10_INPUT_PER_VERTEX_DATA, 0},
    };

    hr = g_Device->CreateInputLayout(layout, 2, vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), &g_InputLayout);
    if (FAILED(hr)) return false;

    D3D10_BUFFER_DESC vbDesc = {};
    vbDesc.Usage = D3D10_USAGE_DYNAMIC;
    vbDesc.ByteWidth = sizeof(Vertex) * 6;
    vbDesc.BindFlags = D3D10_BIND_VERTEX_BUFFER;
    vbDesc.CPUAccessFlags = D3D10_CPU_ACCESS_WRITE;

    hr = g_Device->CreateBuffer(&vbDesc, nullptr, &g_VB);
    if (FAILED(hr)) return false;

    return true;
}

static void UpdateGeometry() {
    auto now = std::chrono::high_resolution_clock::now();
    float elapsed = std::chrono::duration<float>(now - g_StartTime).count();
    g_BarPosition = (float)std::fmod((double)(elapsed * 0.5f), 1.0);

    float w = 100.0f;
    float xPx = g_BarPosition * (g_WindowWidth - w);

    float left = (xPx / (float)g_WindowWidth) * 2.0f - 1.0f;
    float right = ((xPx + w) / (float)g_WindowWidth) * 2.0f - 1.0f;
    float top = 0.2f;
    float bottom = -0.2f;

    Vertex v[6] = {
        {{left,  top}, {1,1,1,1}},
        {{right, top}, {1,1,1,1}},
        {{right, bottom}, {1,1,1,1}},

        {{left,  top}, {1,1,1,1}},
        {{right, bottom}, {1,1,1,1}},
        {{left,  bottom}, {1,1,1,1}},
    };

    void* mappedData = nullptr;
    if (SUCCEEDED(g_VB->Map(D3D10_MAP_WRITE_DISCARD, 0, &mappedData)) && mappedData) {
        memcpy(mappedData, v, sizeof(v));
        g_VB->Unmap();
    }
}

static void Render() {
    UpdateGeometry();

    float clearColor[4] = {0.1f, 0.1f, 0.1f, 1.0f};
    g_Device->ClearRenderTargetView(g_Rtv.Get(), clearColor);

    for (int i = 0; i < g_GpuLoadPasses; i++) {
        float loadColor[4] = {0.1f + (i % 2) * 0.01f, 0.1f, 0.1f, 1.0f};
        g_Device->ClearRenderTargetView(g_Rtv.Get(), loadColor);
    }
    g_Device->ClearRenderTargetView(g_Rtv.Get(), clearColor);

    UINT stride = sizeof(Vertex);
    UINT offset = 0;
    ID3D10Buffer* vbs[] = { g_VB.Get() };

    g_Device->OMSetRenderTargets(1, g_Rtv.GetAddressOf(), nullptr);

    D3D10_VIEWPORT vp = {};
    vp.Width = (UINT)g_WindowWidth;
    vp.Height = (UINT)g_WindowHeight;
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    vp.TopLeftX = 0;
    vp.TopLeftY = 0;
    g_Device->RSSetViewports(1, &vp);

    g_Device->IASetInputLayout(g_InputLayout.Get());
    g_Device->IASetVertexBuffers(0, 1, vbs, &stride, &offset);
    g_Device->IASetPrimitiveTopology(D3D10_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    g_Device->VSSetShader(g_VS.Get());
    g_Device->PSSetShader(g_PS.Get());

    g_Device->Draw(6, 0);

    g_SwapChain->Present(g_VSync, 0);
}

int main(int argc, char* argv[]) {
    SetProcessDPIAware();
    LoadConfig();
    if (argc >= 3) { g_WindowWidth = atoi(argv[1]); g_WindowHeight = atoi(argv[2]); }
    if (argc >= 4) { g_GpuLoadPasses = atoi(argv[3]); }

    WNDCLASSEXW wc = { sizeof(WNDCLASSEXW), CS_HREDRAW | CS_VREDRAW, WndProc, 0, 0, GetModuleHandle(nullptr), nullptr, LoadCursor(nullptr, IDC_ARROW), nullptr, nullptr, WINDOW_CLASS, nullptr };
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowW(WINDOW_CLASS, L"DX10 Test", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, g_WindowWidth, g_WindowHeight, nullptr, nullptr, wc.hInstance, nullptr);
    if (!InitDX10(hwnd)) return 1;

    ShowWindow(hwnd, SW_SHOW);

    MSG msg = {};
    while (g_Running) {
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessage(&msg); }
        Render();
    }

    return 0;
}
