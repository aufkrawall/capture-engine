#include <gtest/gtest.h>

#include <windows.h>

#include <d3d10.h>
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>

#include <cstddef>
#include <filesystem>
#include <string>

#include "hook/d3d11/d3d10_vtable_slots.h"
#include "source_fragment_reader.h"

// DX11Hook::Init discovers d3d11/d3d10/dxgi vtables from throwaway objects it
// creates on CE's hook thread. Those objects are WARP: a hardware device there
// re-enters vendor driver initialization concurrently with the game's own
// device creation, and NVIDIA Smooth Motion's NvPresent64 breaks into int 3 when
// two of its Detours transactions collide (Witcher 3 DX12, session
// 20261001_032227). These tests pin the source rule and prove the premise it
// rests on: the harvested entries are the same ones a hardware device carries.
namespace {

using Microsoft::WRL::ComPtr;

// Method counts of each interface, i.e. every vtable slot CE could hook.
constexpr size_t kD3D11DeviceSlots = 43;
constexpr size_t kD3D11DeviceContextSlots = 115;
constexpr size_t kDXGISwapChainSlots = 18;
constexpr size_t kD3D10DeviceSlots = 98;

std::string ReadSource(const std::filesystem::path& relativePath) {
    return ce::test_source::ReadLogicalSource(std::filesystem::current_path() / relativePath);
}

void** VTableOf(IUnknown* object) {
    return *reinterpret_cast<void***>(object);
}

void ExpectSameSlots(IUnknown* hardware, IUnknown* warp, size_t slots, const char* what) {
    void** hardwareVTable = VTableOf(hardware);
    void** warpVTable = VTableOf(warp);
    for (size_t slot = 0; slot < slots; ++slot) {
        EXPECT_EQ(hardwareVTable[slot], warpVTable[slot]) << what << " slot " << slot;
    }
}

class TempWindow {
public:
    TempWindow()
        : hwnd_(CreateWindowExA(0, "STATIC", "CEProbeTest", WS_OVERLAPPEDWINDOW, 0, 0, 100, 100, nullptr, nullptr,
                                GetModuleHandleA(nullptr), nullptr)) {}
    ~TempWindow() {
        if (hwnd_) {
            DestroyWindow(hwnd_);
        }
    }
    TempWindow(const TempWindow&) = delete;
    TempWindow& operator=(const TempWindow&) = delete;
    HWND get() const { return hwnd_; }

private:
    HWND hwnd_;
};

struct D3D11Probe {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGISwapChain> swapChain;
};

// Same arguments as the DX11Hook::Init temp probe, differing only in driver type.
HRESULT CreateD3D11Probe(D3D_DRIVER_TYPE driverType, HWND hwnd, D3D11Probe& out) {
    DXGI_SWAP_CHAIN_DESC scd = {};
    scd.BufferCount = 1;
    scd.BufferDesc.Width = 100;
    scd.BufferDesc.Height = 100;
    scd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.OutputWindow = hwnd;
    scd.SampleDesc.Count = 1;
    scd.Windowed = TRUE;
    scd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0};
    D3D_FEATURE_LEVEL selected = D3D_FEATURE_LEVEL_11_0;
    return D3D11CreateDeviceAndSwapChain(nullptr, driverType, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 1,
                                         D3D11_SDK_VERSION, &scd, &out.swapChain, &out.device, &selected,
                                         &out.context);
}

}  // namespace

TEST(DX11HookDiscoveryProbeTest, TempProbeNeverCreatesAHardwareDevice) {
    const std::string source = ReadSource("hook/d3d11/dx11_hook.cpp");
    ASSERT_FALSE(source.empty());

    const size_t probeBegin = source.find("DX11: Scanning for pre-existing swapchains...");
    const size_t probeEnd = source.find("void DX11Hook::Shutdown()", probeBegin);
    ASSERT_NE(probeBegin, std::string::npos);
    ASSERT_NE(probeEnd, std::string::npos);
    const std::string probe = source.substr(probeBegin, probeEnd - probeBegin);

    EXPECT_EQ(probe.find("DRIVER_TYPE_HARDWARE"), std::string::npos);
    EXPECT_NE(probe.find("pD3D10CD(NULL, D3D10_DRIVER_TYPE_WARP"), std::string::npos);
    EXPECT_NE(probe.find("pTempCreate(nullptr, D3D_DRIVER_TYPE_WARP"), std::string::npos);
}

TEST(DX11HookDiscoveryProbeTest, WarpProbeHarvestsTheHardwareD3D11VTableEntries) {
    TempWindow warpWindow;
    TempWindow hardwareWindow;
    ASSERT_NE(warpWindow.get(), nullptr);
    ASSERT_NE(hardwareWindow.get(), nullptr);

    D3D11Probe warp;
    ASSERT_TRUE(SUCCEEDED(CreateD3D11Probe(D3D_DRIVER_TYPE_WARP, warpWindow.get(), warp)));
    D3D11Probe hardware;
    if (FAILED(CreateD3D11Probe(D3D_DRIVER_TYPE_HARDWARE, hardwareWindow.get(), hardware))) {
        GTEST_SKIP() << "No hardware D3D11 adapter to compare against";
    }

    ExpectSameSlots(hardware.device.Get(), warp.device.Get(), kD3D11DeviceSlots, "ID3D11Device");
    ExpectSameSlots(hardware.context.Get(), warp.context.Get(), kD3D11DeviceContextSlots, "ID3D11DeviceContext");
    ExpectSameSlots(hardware.swapChain.Get(), warp.swapChain.Get(), kDXGISwapChainSlots, "IDXGISwapChain");
}

TEST(DX11HookDiscoveryProbeTest, WarpProbeHarvestsTheHardwareD3D10VTableEntries) {
    ComPtr<ID3D10Device> warp;
    ASSERT_TRUE(SUCCEEDED(D3D10CreateDevice(nullptr, D3D10_DRIVER_TYPE_WARP, nullptr, 0, D3D10_SDK_VERSION, &warp)));
    ComPtr<ID3D10Device> hardware;
    if (FAILED(D3D10CreateDevice(nullptr, D3D10_DRIVER_TYPE_HARDWARE, nullptr, 0, D3D10_SDK_VERSION, &hardware))) {
        GTEST_SKIP() << "No hardware D3D10 adapter to compare against";
    }

    ExpectSameSlots(hardware.Get(), warp.Get(), kD3D10DeviceSlots, "ID3D10Device");
}

TEST(DX11HookDiscoveryProbeTest, D3D10SamplerHookSlotIsCreateSamplerState) {
    static_assert(ce::d3d10_vtable_slots::kDeviceCreateSamplerState < kD3D10DeviceSlots);
    ComPtr<ID3D10Device> device;
    ASSERT_TRUE(SUCCEEDED(D3D10CreateDevice(nullptr, D3D10_DRIVER_TYPE_WARP, nullptr, 0, D3D10_SDK_VERSION, &device)));

    // Call the slot exactly the way DetourCreateSamplerState10's saved original is called.
    using CreateSamplerStateFn =
        HRESULT(STDMETHODCALLTYPE*)(ID3D10Device*, const D3D10_SAMPLER_DESC*, ID3D10SamplerState**);
    auto createSamplerState = reinterpret_cast<CreateSamplerStateFn>(
        VTableOf(device.Get())[ce::d3d10_vtable_slots::kDeviceCreateSamplerState]);

    D3D10_SAMPLER_DESC desc = {};
    desc.Filter = D3D10_FILTER_ANISOTROPIC;
    desc.AddressU = D3D10_TEXTURE_ADDRESS_WRAP;
    desc.AddressV = D3D10_TEXTURE_ADDRESS_CLAMP;
    desc.AddressW = D3D10_TEXTURE_ADDRESS_MIRROR;
    desc.MaxAnisotropy = 7;
    desc.ComparisonFunc = D3D10_COMPARISON_NEVER;
    desc.MaxLOD = D3D10_FLOAT32_MAX;
    ComPtr<ID3D10SamplerState> sampler;
    ASSERT_TRUE(SUCCEEDED(createSamplerState(device.Get(), &desc, &sampler)));
    ASSERT_NE(sampler.Get(), nullptr);

    D3D10_SAMPLER_DESC created = {};
    sampler->GetDesc(&created);
    EXPECT_EQ(created.Filter, desc.Filter);
    EXPECT_EQ(created.AddressV, desc.AddressV);
    EXPECT_EQ(created.AddressW, desc.AddressW);
    EXPECT_EQ(created.MaxAnisotropy, desc.MaxAnisotropy);
}
