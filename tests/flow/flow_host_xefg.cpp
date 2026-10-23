// The XeSS frame generation side of a flow-test game, integrated like a game that links the XeFG SDK: libxess_fg.dll
// and libxell.dll loaded on demand, an XeLL context linked to the XeFG context (XeFG refuses to initialize without
// one), the native swapchain made by XeFG from a description or from the game's own swapchain, the proxy taken with
// GetSwapChainPtr, generation off until enabled, and every frame XeLL's sleep and latency markers, then a present ID
// before the proxy Present.

#include <cstdint>
#include <string>

#include "hook/xefg/xefg_api.h"
#include "tests/flow/flow_host.h"

namespace ce::flow {

struct XeFGGame {
    HMODULE xefg = nullptr;
    HMODULE xell = nullptr;
    xefg_api::PFN_D3D12CreateContext createContext = nullptr;
    xefg_api::PFN_D3D12InitFromSwapChainDesc initFromDesc = nullptr;
    xefg_api::PFN_D3D12InitFromSwapChain initFromSwapchain = nullptr;
    xefg_api::PFN_D3D12GetSwapChainPtr getSwapchain = nullptr;
    xefg_api::PFN_SetEnabled setEnabled = nullptr;
    xefg_api::PFN_SetNumInterpolatedFrames setInterpolatedFrames = nullptr;
    xefg_api::PFN_SetPresentId setPresentId = nullptr;
    xefg_api::PFN_Destroy destroy = nullptr;
    xefg_api::PFN_SetLatencyReduction setLatencyReduction = nullptr;
    using XeLLCreate = xell_api::Result (*)(ID3D12Device*, xell_api::Handle*);
    using XeLLDestroy = xell_api::Result (*)(xell_api::Handle);
    XeLLCreate xellCreate = nullptr;
    XeLLDestroy xellDestroy = nullptr;
    xell_api::PFN_SetSleepMode xellSetSleepMode = nullptr;
    xell_api::PFN_Sleep xellSleep = nullptr;
    xell_api::PFN_AddMarkerData xellAddMarker = nullptr;
    xefg_api::Handle context = nullptr;
    xell_api::Handle latency = nullptr;
    uint32_t presentId = 0;
};

namespace {

// The SDK's init parameters: the prefix CE and the fake read, then room for the fields the SDK appends.
struct InitParams {
    xefg_api::D3D12InitParamsPrefix prefix{};
    uint8_t rest[64] = {};
};

template <typename Fn>
Fn Resolve(HMODULE module, const char* name) {
    return module ? reinterpret_cast<Fn>(GetProcAddress(module, name)) : nullptr;
}

}  // namespace

bool FlowGame::CreateXeFGSwapchain(const DXGI_SWAP_CHAIN_DESC1& desc, ComPtr<IDXGISwapChain1>* swapchain) {
    if (!xefg_) {
        xefg_ = new XeFGGame();
        char path[MAX_PATH] = {};
        GetModuleFileNameA(nullptr, path, MAX_PATH);
        std::string directory(path);
        directory = directory.substr(0, directory.find_last_of('\\'));
        xefg_->xell = LoadLibraryA((directory + "\\libxell.dll").c_str());
        xefg_->xefg = LoadLibraryA((directory + "\\libxess_fg.dll").c_str());
        if (!xefg_->xefg || !xefg_->xell)
            return Fail("LoadLibrary(libxess_fg.dll / libxell.dll)", HRESULT_FROM_WIN32(GetLastError()));
        XeFGGame& x = *xefg_;
        x.createContext = Resolve<xefg_api::PFN_D3D12CreateContext>(x.xefg, "xefgSwapChainD3D12CreateContext");
        x.initFromDesc =
            Resolve<xefg_api::PFN_D3D12InitFromSwapChainDesc>(x.xefg, "xefgSwapChainD3D12InitFromSwapChainDesc");
        x.initFromSwapchain = Resolve<xefg_api::PFN_D3D12InitFromSwapChain>(x.xefg, "xefgSwapChainD3D12InitFromSwapChain");
        x.getSwapchain = Resolve<xefg_api::PFN_D3D12GetSwapChainPtr>(x.xefg, "xefgSwapChainD3D12GetSwapChainPtr");
        x.setEnabled = Resolve<xefg_api::PFN_SetEnabled>(x.xefg, "xefgSwapChainSetEnabled");
        x.setInterpolatedFrames =
            Resolve<xefg_api::PFN_SetNumInterpolatedFrames>(x.xefg, "xefgSwapChainSetNumInterpolatedFrames");
        x.setPresentId = Resolve<xefg_api::PFN_SetPresentId>(x.xefg, "xefgSwapChainSetPresentId");
        x.destroy = Resolve<xefg_api::PFN_Destroy>(x.xefg, "xefgSwapChainDestroy");
        x.setLatencyReduction = Resolve<xefg_api::PFN_SetLatencyReduction>(x.xefg, "xefgSwapChainSetLatencyReduction");
        x.xellCreate = Resolve<XeFGGame::XeLLCreate>(x.xell, "xellD3D12CreateContext");
        x.xellDestroy = Resolve<XeFGGame::XeLLDestroy>(x.xell, "xellDestroyContext");
        x.xellSetSleepMode = Resolve<xell_api::PFN_SetSleepMode>(x.xell, "xellSetSleepMode");
        x.xellSleep = Resolve<xell_api::PFN_Sleep>(x.xell, "xellSleep");
        x.xellAddMarker = Resolve<xell_api::PFN_AddMarkerData>(x.xell, "xellAddMarkerData");
        if (!x.createContext || !x.initFromDesc || !x.initFromSwapchain || !x.getSwapchain || !x.setEnabled ||
            !x.setPresentId || !x.destroy || !x.setLatencyReduction || !x.xellCreate || !x.xellDestroy ||
            !x.xellSetSleepMode || !x.xellSleep || !x.xellAddMarker)
            return Fail("resolving the XeFG/XeLL exports", E_NOINTERFACE);
        if (x.xellCreate(device_.Get(), &x.latency) != 0)
            return Fail("xellD3D12CreateContext", E_FAIL);
    }
    XeFGGame& x = *xefg_;
    if (x.createContext(device_.Get(), &x.context) != xefg_api::kSuccess)
        return Fail("xefgSwapChainD3D12CreateContext", E_FAIL);
    if (x.setLatencyReduction(x.context, x.latency) != xefg_api::kSuccess)
        return Fail("xefgSwapChainSetLatencyReduction", E_FAIL);
    InitParams params;
    params.prefix.maxInterpolatedFrames = 3;
    xefg_api::Result result = xefg_api::kSuccess;
    if (xefgFromSwapchain_) {
        // The game's own swapchain, handed over with its last reference (XeFG releases it).
        ComPtr<IDXGISwapChain1> application;
        const HRESULT hr =
            nativeFactory_->CreateSwapChainForHwnd(queue_.Get(), window_, &desc, nullptr, nullptr, &application);
        if (FAILED(hr))
            return Fail("CreateSwapChainForHwnd (application swapchain for XeFG)", hr);
        params.prefix.applicationSwapChain = application.Detach();
        result = x.initFromSwapchain(x.context, queue_.Get(), &params);
    } else {
        ComPtr<IDXGIFactory2> factory;
        nativeFactory_.As(&factory);
        result = x.initFromDesc(x.context, window_, &desc, nullptr, queue_.Get(), factory.Get(), &params);
    }
    if (result != xefg_api::kSuccess)
        return Fail(xefgFromSwapchain_ ? "xefgSwapChainD3D12InitFromSwapChain" : "xefgSwapChainD3D12InitFromSwapChainDesc",
                    static_cast<HRESULT>(result));
    void* proxy = nullptr;
    if (x.getSwapchain(x.context, __uuidof(IDXGISwapChain1), &proxy) != xefg_api::kSuccess || !proxy)
        return Fail("xefgSwapChainD3D12GetSwapChainPtr", E_NOINTERFACE);
    swapchain->Attach(static_cast<IDXGISwapChain1*>(proxy));
    x.setEnabled(x.context, 0);
    return true;
}

bool FlowGame::SetXeFGFrameGeneration(bool enabled, uint32_t interpolatedFrames) {
    if (!xefg_ || !xefg_->context || kind_ != SwapchainKind::kXeFG)
        return Fail("SetXeFGFrameGeneration without an XeFG swapchain", E_NOINTERFACE);
    if (xefg_->setInterpolatedFrames &&
        xefg_->setInterpolatedFrames(xefg_->context, interpolatedFrames) != xefg_api::kSuccess)
        return Fail("xefgSwapChainSetNumInterpolatedFrames", E_INVALIDARG);
    if (xefg_->setEnabled(xefg_->context, enabled ? 1 : 0) != xefg_api::kSuccess)
        return Fail("xefgSwapChainSetEnabled", E_FAIL);
    return true;
}

bool FlowGame::SetXeLLFrameCap(uint32_t minimumIntervalUs) {
    if (!xefg_ || !xefg_->latency)
        return Fail("SetXeLLFrameCap without an XeLL context", E_NOINTERFACE);
    xell_api::SleepParams params{};
    params.minimumIntervalUs = minimumIntervalUs;
    params.lowLatencyMode = 1;
    if (xefg_->xellSetSleepMode(xefg_->latency, &params) != 0)
        return Fail("xellSetSleepMode", E_INVALIDARG);
    return true;
}

// The frame's XeLL sleep and its simulation start, then its present start and present ID right before Present.
void FlowGame::SubmitXeFGFrame() {
    if (!xefg_ || !xefg_->context)
        return;
    const uint32_t frame = ++xefg_->presentId;
    xefg_->xellSleep(xefg_->latency, frame);
    xefg_->xellAddMarker(xefg_->latency, frame, xell_api::kSimulationStart);
    xefg_->xellAddMarker(xefg_->latency, frame, xell_api::kPresentStart);
    xefg_->setPresentId(xefg_->context, frame);
}

void FlowGame::DestroyXeFGContext() {
    if (!xefg_ || !xefg_->context)
        return;
    xefg_->destroy(xefg_->context);
    xefg_->context = nullptr;
}

// The runtime modules stay loaded with the hook, as a game keeps them until exit.
void FlowGame::EndXeFG() {
    if (!xefg_)
        return;
    DestroyXeFGContext();
    if (xefg_->latency && xefg_->xellDestroy)
        xefg_->xellDestroy(xefg_->latency);
    delete xefg_;
    xefg_ = nullptr;
}

}  // namespace ce::flow
