// Fake sl.interposer.dll: the Streamline 2.x core API with the export set CE classifies (2.x-only markers
// slSetTagForFrame, slGetNewFrameToken, slSetD3DDevice, slGetFeatureFunction ...), a DXGI factory proxy
// (sl::PreferenceFlags::eUseDXGIFactoryProxy) whose CreateSwapChainForHwnd lets sl.dlss_g.dll create the real
// swapchain, and plugin loading at slInit like the real interposer (sl.common.dll, then the DLSS-G, Reflex and
// PCL plugins from the interposer's own directory). Other features report eErrorFeatureMissing.

#define SL_INTERPOSER

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <atomic>
#include <string>

#include "sl.h"
#include "sl_dlss_g.h"
#include "tests/flow/fakes/fake_dxgi_forwarders.h"
#include "tests/flow/fakes/fake_runtime_log.h"

namespace ce::flow::fake {
namespace {

using Microsoft::WRL::ComPtr;
using CreateSwapChainForHwnd_t = HRESULT (*)(IDXGIFactory2*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*,
                                             const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*,
                                             IDXGISwapChain1**);
using GetPluginFunction_t = void* (*)(const char*);

constexpr const char* kModule = "sl.interposer";

HMODULE g_common = nullptr;
HMODULE g_dlssg = nullptr;
HMODULE g_reflex = nullptr;
HMODULE g_pcl = nullptr;
GetPluginFunction_t g_dlssgFunction = nullptr;
GetPluginFunction_t g_reflexFunction = nullptr;
GetPluginFunction_t g_pclFunction = nullptr;

GetPluginFunction_t PluginFunctions(HMODULE plugin) {
    return plugin ? reinterpret_cast<GetPluginFunction_t>(GetProcAddress(plugin, "slGetPluginFunction")) : nullptr;
}
std::atomic<bool> g_initialized{false};
void* g_device = nullptr;

class FrameTokenImpl final : public sl::FrameToken {
public:
    operator uint32_t() const override { return index; }
    uint32_t index = 0;
};
FrameTokenImpl g_frameTokens[8];
std::atomic<uint32_t> g_nextFrame{0};

std::wstring OwnDirectory() {
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&OwnDirectory), &self);
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(self, path, MAX_PATH);
    std::wstring directory(path);
    return directory.substr(0, directory.find_last_of(L'\\'));
}

class StreamlineFactoryProxy final : public ForwardingFactory {
public:
    using ForwardingFactory::ForwardingFactory;

    HRESULT STDMETHODCALLTYPE CreateSwapChainForHwnd(IUnknown* device, HWND window, const DXGI_SWAP_CHAIN_DESC1* desc,
                                                     const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreenDesc,
                                                     IDXGIOutput* output, IDXGISwapChain1** swapchain) override {
        auto create = g_dlssgFunction ? reinterpret_cast<CreateSwapChainForHwnd_t>(
                                            g_dlssgFunction("flowCreateSwapChainForHwnd"))
                                      : nullptr;
        if (!create)
            return real_->CreateSwapChainForHwnd(device, window, desc, fullscreenDesc, output, swapchain);
        return create(real_, device, window, desc, fullscreenDesc, output, swapchain);
    }
};

// The real entry points, by address: this module exports functions under the same names (sl.interposer.def).
template <typename Function>
Function SystemExport(const wchar_t* module, const char* name) {
    return reinterpret_cast<Function>(GetProcAddress(LoadLibraryW(module), name));
}

HRESULT CreateFactoryProxy(REFIID riid, void** factory) {
    using CreateDXGIFactory2_t = HRESULT(WINAPI*)(UINT, REFIID, void**);
    static const auto createFactory = SystemExport<CreateDXGIFactory2_t>(L"dxgi.dll", "CreateDXGIFactory2");
    ComPtr<IDXGIFactory7> real;
    HRESULT hr = createFactory(0, IID_PPV_ARGS(&real));
    if (FAILED(hr))
        return hr;
    auto* proxy = new StreamlineFactoryProxy(real.Get());
    hr = proxy->QueryInterface(riid, factory);
    proxy->Release();
    Log(kModule, "DXGI factory proxy %p over real %p hr=0x%08lX", proxy, real.Get(), static_cast<unsigned long>(hr));
    return hr;
}

}  // namespace
}  // namespace ce::flow::fake

using namespace ce::flow::fake;

sl::Result slInit(const sl::Preferences&, uint64_t) {
    const std::wstring directory = OwnDirectory();
    g_common = LoadLibraryW((directory + L"\\sl.common.dll").c_str());
    g_dlssg = LoadLibraryW((directory + L"\\sl.dlss_g.dll").c_str());
    g_reflex = LoadLibraryW((directory + L"\\sl.reflex.dll").c_str());
    g_pcl = LoadLibraryW((directory + L"\\sl.pcl.dll").c_str());
    g_dlssgFunction = PluginFunctions(g_dlssg);
    g_reflexFunction = PluginFunctions(g_reflex);
    g_pclFunction = PluginFunctions(g_pcl);
    g_initialized = g_common && g_dlssgFunction && g_reflexFunction && g_pclFunction;
    Log(kModule, "slInit common=%p dlss_g=%p reflex=%p pcl=%p", g_common, g_dlssg, g_reflex, g_pcl);
    return g_initialized ? sl::Result::eOk : sl::Result::eErrorInitNotCalled;
}

sl::Result slShutdown() {
    Log(kModule, "slShutdown");
    g_initialized = false;
    g_dlssgFunction = g_reflexFunction = g_pclFunction = nullptr;
    g_device = nullptr;
    for (HMODULE plugin : {g_pcl, g_reflex, g_dlssg, g_common}) {
        if (plugin)
            FreeLibrary(plugin);
    }
    g_pcl = g_reflex = g_dlssg = g_common = nullptr;
    return sl::Result::eOk;
}

GetPluginFunction_t FeaturePlugin(sl::Feature feature) {
    if (feature == sl::kFeatureDLSS_G)
        return g_dlssgFunction;
    if (feature == sl::kFeatureReflex)
        return g_reflexFunction;
    if (feature == sl::kFeaturePCL)
        return g_pclFunction;
    return nullptr;
}

sl::Result slIsFeatureSupported(sl::Feature feature, const sl::AdapterInfo&) {
    return FeaturePlugin(feature) ? sl::Result::eOk : sl::Result::eErrorFeatureMissing;
}

sl::Result slIsFeatureLoaded(sl::Feature feature, bool& loaded) {
    loaded = FeaturePlugin(feature) != nullptr;
    return sl::Result::eOk;
}

sl::Result slSetFeatureLoaded(sl::Feature, bool) {
    return sl::Result::eOk;
}

sl::Result slEvaluateFeature(sl::Feature, const sl::FrameToken&, const sl::BaseStructure**, uint32_t,
                             sl::CommandBuffer*) {
    return sl::Result::eOk;
}

sl::Result slAllocateResources(sl::CommandBuffer*, sl::Feature, const sl::ViewportHandle&) {
    return sl::Result::eOk;
}

sl::Result slFreeResources(sl::Feature, const sl::ViewportHandle&) {
    return sl::Result::eOk;
}

sl::Result slSetTag(const sl::ViewportHandle&, const sl::ResourceTag*, uint32_t, sl::CommandBuffer*) {
    return sl::Result::eOk;
}

sl::Result slSetTagForFrame(const sl::FrameToken&, const sl::ViewportHandle&, const sl::ResourceTag*, uint32_t,
                            sl::CommandBuffer*) {
    return sl::Result::eOk;
}

sl::Result slGetFeatureRequirements(sl::Feature, sl::FeatureRequirements& requirements) {
    requirements.flags = sl::FeatureRequirementFlags::eD3D12Supported;
    return sl::Result::eOk;
}

sl::Result slGetFeatureVersion(sl::Feature, sl::FeatureVersion& version) {
    version.versionSL = sl::Version(2, 7, 32);
    version.versionNGX = sl::Version(3, 10, 0);
    return sl::Result::eOk;
}

sl::Result slUpgradeInterface(void**) {
    return sl::Result::eOk;
}

sl::Result slSetConstants(const sl::Constants&, const sl::FrameToken&, const sl::ViewportHandle&) {
    return sl::Result::eOk;
}

sl::Result slGetNativeInterface(void*, void**) {
    return sl::Result::eErrorInvalidParameter;
}

sl::Result slGetFeatureFunction(sl::Feature feature, const char* functionName, void*& function) {
    const GetPluginFunction_t plugin = FeaturePlugin(feature);
    function = plugin ? plugin(functionName) : nullptr;
    return function ? sl::Result::eOk : sl::Result::eErrorFeatureMissing;
}

sl::Result slGetNewFrameToken(sl::FrameToken*& token, const uint32_t* frameIndex) {
    const uint32_t index = frameIndex ? *frameIndex : g_nextFrame.fetch_add(1);
    FrameTokenImpl& slot = g_frameTokens[index % 8];
    slot.index = index;
    token = &slot;
    return sl::Result::eOk;
}

sl::Result slSetD3DDevice(void* d3dDevice) {
    if (g_device && g_device != d3dDevice)
        return sl::Result::eErrorInvalidIntegration;
    g_device = d3dDevice;
    Log(kModule, "slSetD3DDevice %p", d3dDevice);
    return sl::Result::eOk;
}

// Exported as CreateDXGIFactory*, D3D12CreateDevice and vkCreateSwapchainKHR (sl.interposer.def).
extern "C" HRESULT WINAPI flowCreateDXGIFactory(REFIID riid, void** factory) {
    return CreateFactoryProxy(riid, factory);
}

extern "C" HRESULT WINAPI flowCreateDXGIFactory1(REFIID riid, void** factory) {
    return CreateFactoryProxy(riid, factory);
}

extern "C" HRESULT WINAPI flowCreateDXGIFactory2(UINT, REFIID riid, void** factory) {
    return CreateFactoryProxy(riid, factory);
}

// The real interposer wraps the device; the fake hands out the native one (eDisableCLStateTracking games
// see the same native command lists either way).
extern "C" HRESULT WINAPI flowD3D12CreateDevice(IUnknown* adapter, D3D_FEATURE_LEVEL level, REFIID riid,
                                                void** device) {
    using D3D12CreateDevice_t = HRESULT(WINAPI*)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
    static const auto createDevice = SystemExport<D3D12CreateDevice_t>(L"d3d12.dll", "D3D12CreateDevice");
    return createDevice(adapter, level, riid, device);
}

// In the real interposer's export table; CE hooks it there. Never called by a D3D12 game.
extern "C" int32_t flowVkCreateSwapchainKHR(void*, const void*, const void*, void*) {
    return -1;
}
