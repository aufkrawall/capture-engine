// The Streamline side of a flow-test game, integrated like testapp/dx12_fg_switch_streamline.cpp: slInit
// before the device, the DXGI factory proxy, slSetD3DDevice, slDLSSGSetOptions/slDLSSGGetState through
// slGetFeatureFunction, and per frame a frame token, the frame-generation tags and an slDLSSGGetState poll.

#include <string>

#include "sl.h"
#include "sl_dlss_g.h"
#include "sl_pcl.h"
#include "sl_reflex.h"
#include "tests/flow/flow_host.h"

namespace ce::flow {

struct StreamlineGame {
    HMODULE module = nullptr;
    PFun_slInit* init = nullptr;
    PFun_slShutdown* shutdown = nullptr;
    PFun_slSetD3DDevice* setDevice = nullptr;
    PFun_slGetFeatureFunction* getFeatureFunction = nullptr;
    PFun_slGetNewFrameToken* newFrameToken = nullptr;
    PFun_slSetTagForFrame* setTagForFrame = nullptr;
    PFun_slDLSSGSetOptions* dlssgSetOptions = nullptr;
    PFun_slDLSSGGetState* dlssgGetState = nullptr;
    PFun_slReflexSetOptions* reflexSetOptions = nullptr;
    PFun_slReflexSleep* reflexSleep = nullptr;
    PFun_slPCLSetMarker* pclSetMarker = nullptr;
    sl::FrameToken* frameToken = nullptr;
    bool reflexLowLatency = false;
    HRESULT(WINAPI* createFactory1)(REFIID, void**) = nullptr;
    HRESULT(WINAPI* createDevice)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**) = nullptr;
    sl::ViewportHandle viewport{0u};
    uint32_t frameIndex = 0;
    ComPtr<ID3D12Resource> hudless;
    ComPtr<ID3D12Resource> ui;
};

namespace {

template <typename Function>
Function* Export(HMODULE module, const char* name) {
    return reinterpret_cast<Function*>(GetProcAddress(module, name));
}

}  // namespace

bool FlowGame::BeginStreamline() {
    streamline_ = new StreamlineGame();
    char directory[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, directory, MAX_PATH);
    std::string path(directory);
    path = path.substr(0, path.find_last_of('\\')) + "\\sl.interposer.dll";
    HMODULE module = LoadLibraryA(path.c_str());
    if (!module)
        return Fail("LoadLibrary(sl.interposer.dll)", HRESULT_FROM_WIN32(GetLastError()));
    StreamlineGame& sl = *streamline_;
    sl.module = module;
    sl.init = Export<PFun_slInit>(module, "slInit");
    sl.shutdown = Export<PFun_slShutdown>(module, "slShutdown");
    sl.setDevice = Export<PFun_slSetD3DDevice>(module, "slSetD3DDevice");
    sl.getFeatureFunction = Export<PFun_slGetFeatureFunction>(module, "slGetFeatureFunction");
    sl.newFrameToken = Export<PFun_slGetNewFrameToken>(module, "slGetNewFrameToken");
    sl.setTagForFrame = Export<PFun_slSetTagForFrame>(module, "slSetTagForFrame");
    sl.createFactory1 = reinterpret_cast<decltype(sl.createFactory1)>(GetProcAddress(module, "CreateDXGIFactory1"));
    sl.createDevice = reinterpret_cast<decltype(sl.createDevice)>(GetProcAddress(module, "D3D12CreateDevice"));
    if (!sl.init || !sl.shutdown || !sl.setDevice || !sl.getFeatureFunction || !sl.newFrameToken ||
        !sl.setTagForFrame || !sl.createFactory1 || !sl.createDevice)
        return Fail("resolving the Streamline exports", E_NOINTERFACE);
    sl::Feature features[] = {sl::kFeatureDLSS_G};
    sl::Preferences preferences{};
    preferences.flags = sl::PreferenceFlags::eDisableCLStateTracking |
                        sl::PreferenceFlags::eUseFrameBasedResourceTagging | sl::PreferenceFlags::eUseDXGIFactoryProxy;
    preferences.featuresToLoad = features;
    preferences.numFeaturesToLoad = 1;
    preferences.renderAPI = sl::RenderAPI::eD3D12;
    if (sl.init(preferences, sl::kSDKVersion) != sl::Result::eOk)
        return Fail("slInit", E_FAIL);
    return true;
}

bool FlowGame::SetDLSSFrameGeneration(bool enabled, uint32_t framesToGenerate) {
    if (!streamline_ || !streamline_->dlssgSetOptions)
        return Fail("SetDLSSFrameGeneration without Streamline", E_NOINTERFACE);
    sl::DLSSGOptions options{};
    options.mode = enabled ? sl::DLSSGMode::eOn : sl::DLSSGMode::eOff;
    options.numFramesToGenerate = framesToGenerate;
    options.numBackBuffers = 3;
    if (streamline_->dlssgSetOptions(streamline_->viewport, options) != sl::Result::eOk)
        return Fail("slDLSSGSetOptions", E_FAIL);
    // As the switch test app: Reflex low latency from the first enable while the proxy lives (DLSS-G's pacer
    // needs it, also while suspended).
    if (enabled && !streamline_->reflexLowLatency) {
        sl::ReflexOptions reflex{};
        reflex.mode = sl::ReflexMode::eLowLatency;
        streamline_->reflexSetOptions(reflex);
        streamline_->reflexLowLatency = true;
    }
    return true;
}

// Called once the device exists: binds it and resolves the DLSS-G feature functions (through CE's hooks,
// which wrap what slGetFeatureFunction returns).
bool FlowGame::BindStreamlineDevice() {
    StreamlineGame& sl = *streamline_;
    if (sl.setDevice(device_.Get()) != sl::Result::eOk)
        return Fail("slSetD3DDevice", E_FAIL);
    void* function = nullptr;
    if (sl.getFeatureFunction(sl::kFeatureDLSS_G, "slDLSSGSetOptions", function) == sl::Result::eOk)
        sl.dlssgSetOptions = reinterpret_cast<PFun_slDLSSGSetOptions*>(function);
    if (sl.getFeatureFunction(sl::kFeatureDLSS_G, "slDLSSGGetState", function) == sl::Result::eOk)
        sl.dlssgGetState = reinterpret_cast<PFun_slDLSSGGetState*>(function);
    if (sl.getFeatureFunction(sl::kFeatureReflex, "slReflexSetOptions", function) == sl::Result::eOk)
        sl.reflexSetOptions = reinterpret_cast<PFun_slReflexSetOptions*>(function);
    if (sl.getFeatureFunction(sl::kFeatureReflex, "slReflexSleep", function) == sl::Result::eOk)
        sl.reflexSleep = reinterpret_cast<PFun_slReflexSleep*>(function);
    if (sl.getFeatureFunction(sl::kFeaturePCL, "slPCLSetMarker", function) == sl::Result::eOk)
        sl.pclSetMarker = reinterpret_cast<PFun_slPCLSetMarker*>(function);
    if (!sl.dlssgSetOptions || !sl.dlssgGetState || !sl.reflexSetOptions || !sl.reflexSleep || !sl.pclSetMarker)
        return Fail("resolving the DLSS-G, Reflex and PCL functions", E_NOINTERFACE);
    sl.hudless = CreateFlowTexture(device_.Get(), width_, height_, DXGI_FORMAT_R8G8B8A8_UNORM);
    sl.ui = CreateFlowTexture(device_.Get(), width_, height_, DXGI_FORMAT_R8G8B8A8_UNORM);
    return true;
}

void FlowGame::SubmitStreamlineFrame(UINT backBufferIndex) {
    StreamlineGame& sl = *streamline_;
    sl::FrameToken* token = nullptr;
    const uint32_t frameIndex = sl.frameIndex++;
    sl.frameToken = nullptr;
    if (sl.newFrameToken(token, &frameIndex) != sl::Result::eOk || !token)
        return;
    sl.frameToken = token;
    sl.reflexSleep(*token);
    sl.pclSetMarker(sl::PCLMarker::eSimulationStart, *token);
    sl.pclSetMarker(sl::PCLMarker::eSimulationEnd, *token);
    sl.pclSetMarker(sl::PCLMarker::eRenderSubmitStart, *token);
    sl::Extent extent{0, 0, width_, height_};
    sl::Resource depth(sl::ResourceType::eTex2d, depth_.Get(), D3D12_RESOURCE_STATE_COMMON);
    sl::Resource motion(sl::ResourceType::eTex2d, motionVectors_.Get(), D3D12_RESOURCE_STATE_COMMON);
    sl::Resource hudless(sl::ResourceType::eTex2d, sl.hudless.Get(), D3D12_RESOURCE_STATE_COMMON);
    sl::Resource ui(sl::ResourceType::eTex2d, sl.ui.Get(), D3D12_RESOURCE_STATE_COMMON);
    sl::Resource backBuffer(sl::ResourceType::eTex2d, backBuffers_[backBufferIndex].Get(),
                            D3D12_RESOURCE_STATE_PRESENT);
    sl::ResourceTag tags[] = {
        sl::ResourceTag(&depth, sl::kBufferTypeDepth, sl::eValidUntilPresent, &extent),
        sl::ResourceTag(&motion, sl::kBufferTypeMotionVectors, sl::eValidUntilPresent, &extent),
        sl::ResourceTag(&hudless, sl::kBufferTypeHUDLessColor, sl::eValidUntilPresent, &extent),
        sl::ResourceTag(&ui, sl::kBufferTypeUIColorAndAlpha, sl::eValidUntilPresent, &extent),
        sl::ResourceTag(&backBuffer, sl::kBufferTypeBackbuffer, sl::eValidUntilPresent, &extent),
    };
    sl.setTagForFrame(*token, sl.viewport, tags, static_cast<uint32_t>(std::size(tags)), list_.Get());
    sl.pclSetMarker(sl::PCLMarker::eRenderSubmitEnd, *token);
}

void FlowGame::MarkStreamlinePresent(bool start) {
    if (streamline_ && streamline_->frameToken)
        streamline_->pclSetMarker(start ? sl::PCLMarker::ePresentStart : sl::PCLMarker::ePresentEnd,
                                  *streamline_->frameToken);
}

void FlowGame::PollStreamlineState() {
    if (!streamline_ || !streamline_->dlssgGetState)
        return;
    sl::DLSSGState state{};
    streamline_->dlssgGetState(streamline_->viewport, state, nullptr);
}

bool FlowGame::DLSSFrameGenerationRunning() {
    if (!streamline_)
        return false;
    void* function = nullptr;
    if (streamline_->getFeatureFunction(sl::kFeatureDLSS_G, "flowCurrentMode", function) != sl::Result::eOk)
        return false;
    return reinterpret_cast<uint32_t (*)()>(function)() == static_cast<uint32_t>(sl::DLSSGMode::eOn);
}

HRESULT FlowGame::StreamlineCreateFactory(REFIID riid, void** factory) {
    return streamline_->createFactory1(riid, factory);
}

HRESULT FlowGame::StreamlineCreateDevice(IUnknown* adapter, REFIID riid, void** device) {
    return streamline_->createDevice(adapter, D3D_FEATURE_LEVEL_11_0, riid, device);
}

void FlowGame::EndStreamline() {
    if (!streamline_)
        return;
    if (streamline_->shutdown && streamline_->dlssgSetOptions)
        streamline_->shutdown();
    // The interposer stays loaded, as games keep it until exit.
    delete streamline_;
    streamline_ = nullptr;
}

}  // namespace ce::flow
