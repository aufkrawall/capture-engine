// The FidelityFX side of a flow-test game, integrated like testapp/dx12_fg_switch_fsr.cpp: the runtime loaded on
// demand, the frame generation swapchain created through ffxCreateContext(FOR_HWND) with the system factory and
// the game queue, a frame generation context, and every frame ffxConfigure (frame ID, enable state, callbacks),
// the UI resource registration while enabled (GameOptions::fsrUi) and ffxDispatch(PREPARE_V2). The present
// callback copies the presented image into the swapchain buffer.

#include <string>

#include <ffx_api.h>
#include <dx12/ffx_api_dx12.h>
#include <ffx_framegeneration.h>
#include <dx12/ffx_api_framegeneration_dx12.h>

#include "tests/flow/flow_host.h"

namespace ce::flow {

struct FidelityFXGame {
    HMODULE module = nullptr;
    PfnFfxCreateContext createContext = nullptr;
    PfnFfxDestroyContext destroyContext = nullptr;
    PfnFfxConfigure configure = nullptr;
    PfnFfxDispatch dispatch = nullptr;
    ffxContext swapchainContext = nullptr;
    ffxContext frameGenerationContext = nullptr;
    bool enabled = false;
    bool presentCallback = true;
    uint64_t frameId = 1;
    ComPtr<ID3D12Resource> ui;  // GameOptions::fsrUi; read-only for AMD, like a game's HUD texture
};

namespace {

D3D12_RESOURCE_STATES ToD3D12State(uint32_t ffxState) {
    switch (ffxState) {
        case FFX_API_RESOURCE_STATE_UNORDERED_ACCESS:
            return D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        case FFX_API_RESOURCE_STATE_COMPUTE_READ:
            return D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        case FFX_API_RESOURCE_STATE_COPY_SRC:
            return D3D12_RESOURCE_STATE_COPY_SOURCE;
        case FFX_API_RESOURCE_STATE_COPY_DEST:
            return D3D12_RESOURCE_STATE_COPY_DEST;
        case FFX_API_RESOURCE_STATE_RENDER_TARGET:
            return D3D12_RESOURCE_STATE_RENDER_TARGET;
        default:
            return D3D12_RESOURCE_STATE_COMMON;
    }
}

void Transition(ID3D12GraphicsCommandList* list, ID3D12Resource* resource, D3D12_RESOURCE_STATES before,
                D3D12_RESOURCE_STATES after) {
    if (before == after)
        return;
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    list->ResourceBarrier(1, &barrier);
}

ffxReturnCode_t GamePresentCallback(ffxCallbackDescFrameGenerationPresent* params, void*) {
    auto* list = static_cast<ID3D12GraphicsCommandList*>(params->commandList);
    auto* source = static_cast<ID3D12Resource*>(params->currentBackBuffer.resource);
    auto* output = static_cast<ID3D12Resource*>(params->outputSwapChainBuffer.resource);
    if (!list || !source || !output || source == output)
        return FFX_API_RETURN_OK;
    const D3D12_RESOURCE_STATES sourceState = ToD3D12State(params->currentBackBuffer.state);
    const D3D12_RESOURCE_STATES outputState = ToD3D12State(params->outputSwapChainBuffer.state);
    Transition(list, source, sourceState, D3D12_RESOURCE_STATE_COPY_SOURCE);
    Transition(list, output, outputState, D3D12_RESOURCE_STATE_COPY_DEST);
    list->CopyResource(output, source);
    Transition(list, output, D3D12_RESOURCE_STATE_COPY_DEST, outputState);
    Transition(list, source, D3D12_RESOURCE_STATE_COPY_SOURCE, sourceState);
    return FFX_API_RETURN_OK;
}

ffxReturnCode_t GameFrameGenerationCallback(ffxDispatchDescFrameGeneration* params, void* userContext) {
    auto* game = static_cast<FidelityFXGame*>(userContext);
    return game->dispatch(&game->frameGenerationContext, &params->header);
}

// As testapp/dx12_fg_switch_fsr.cpp declares its HUD: read-only, in shader-resource state, double-buffered by
// the swapchain because the game rewrites it every frame.
void RegisterUiResource(FidelityFXGame& ffx) {
    if (!ffx.ui || !ffx.enabled)
        return;
    ffxConfigureDescFrameGenerationSwapChainRegisterUiResourceDX12 ui{};
    ui.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_REGISTERUIRESOURCE_DX12;
    ui.uiResource = ffxApiGetResourceDX12(ffx.ui.Get(), FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ,
                                          FFX_API_RESOURCE_USAGE_READ_ONLY);
    ui.flags = FFX_FRAMEGENERATION_UI_COMPOSITION_FLAG_ENABLE_INTERNAL_UI_DOUBLE_BUFFERING;
    ffx.configure(&ffx.swapchainContext, &ui.header);
}

void ConfigureFrameGeneration(FidelityFXGame& ffx, IDXGISwapChain3* swapchain) {
    ffxConfigureDescFrameGeneration configure{};
    configure.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
    configure.swapChain = swapchain;
    configure.presentCallback = ffx.presentCallback ? GamePresentCallback : nullptr;
    configure.frameGenerationCallback = GameFrameGenerationCallback;
    configure.frameGenerationCallbackUserContext = &ffx;
    configure.frameGenerationEnabled = ffx.enabled;
    configure.frameID = ffx.frameId;
    ffx.configure(&ffx.frameGenerationContext, &configure.header);
}

}  // namespace

bool FlowGame::CreateFidelityFXSwapchain(const DXGI_SWAP_CHAIN_DESC1& desc, ComPtr<IDXGISwapChain1>* swapchain) {
    if (!fidelityfx_) {
        fidelityfx_ = new FidelityFXGame();
        char path[MAX_PATH] = {};
        GetModuleFileNameA(nullptr, path, MAX_PATH);
        std::string directory(path);
        directory = directory.substr(0, directory.find_last_of('\\'));
        fidelityfx_->module = LoadLibraryA((directory + "\\amd_fidelityfx_framegeneration_dx12.dll").c_str());
        if (!fidelityfx_->module)
            return Fail("LoadLibrary(amd_fidelityfx_framegeneration_dx12.dll)", HRESULT_FROM_WIN32(GetLastError()));
        // Through GetProcAddress, as games resolve them (CE routes official AMD modules' exports there).
        fidelityfx_->createContext =
            reinterpret_cast<PfnFfxCreateContext>(GetProcAddress(fidelityfx_->module, "ffxCreateContext"));
        fidelityfx_->destroyContext =
            reinterpret_cast<PfnFfxDestroyContext>(GetProcAddress(fidelityfx_->module, "ffxDestroyContext"));
        fidelityfx_->configure = reinterpret_cast<PfnFfxConfigure>(GetProcAddress(fidelityfx_->module, "ffxConfigure"));
        fidelityfx_->dispatch = reinterpret_cast<PfnFfxDispatch>(GetProcAddress(fidelityfx_->module, "ffxDispatch"));
        if (!fidelityfx_->createContext || !fidelityfx_->destroyContext || !fidelityfx_->configure ||
            !fidelityfx_->dispatch)
            return Fail("resolving the FidelityFX exports", E_NOINTERFACE);
    }
    FidelityFXGame& ffx = *fidelityfx_;
    DXGI_SWAP_CHAIN_DESC1 swapchainDesc = desc;
    IDXGISwapChain4* ffxSwapchain = nullptr;
    ffxCreateContextDescFrameGenerationSwapChainForHwndDX12 create{};
    create.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_FOR_HWND_DX12;
    create.swapchain = &ffxSwapchain;
    create.hwnd = window_;
    create.desc = &swapchainDesc;
    create.dxgiFactory = nativeFactory_.Get();
    create.gameQueue = queue_.Get();
    if (ffx.createContext(&ffx.swapchainContext, &create.header, nullptr) != FFX_API_RETURN_OK || !ffxSwapchain)
        return Fail("ffxCreateContext(frame generation swapchain)", E_FAIL);
    swapchain->Attach(ffxSwapchain);

    ffxCreateContextDescFrameGeneration frameGeneration{};
    frameGeneration.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
    frameGeneration.displaySize = {width_, height_};
    frameGeneration.maxRenderSize = {width_, height_};
    frameGeneration.backBufferFormat = FFX_API_SURFACE_FORMAT_R8G8B8A8_UNORM;
    if (ffx.createContext(&ffx.frameGenerationContext, &frameGeneration.header, nullptr) != FFX_API_RETURN_OK)
        return Fail("ffxCreateContext(frame generation)", E_FAIL);
    ffx.enabled = false;
    if (fsrUi_ != FSRUiResource::kNone && !ffx.ui) {
        const bool placeholder = fsrUi_ == FSRUiResource::kPlaceholder;
        ffx.ui = CreateFlowTexture(device_.Get(), placeholder ? 1 : width_, placeholder ? 1 : height_,
                                   DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
                                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
    return true;
}

bool FlowGame::SetFSRFrameGeneration(bool enabled, bool presentCallback) {
    if (!fidelityfx_ || !fidelityfx_->frameGenerationContext || kind_ != SwapchainKind::kFidelityFX)
        return Fail("SetFSRFrameGeneration without a FidelityFX swapchain", E_NOINTERFACE);
    fidelityfx_->enabled = enabled;
    fidelityfx_->presentCallback = presentCallback;
    ConfigureFrameGeneration(*fidelityfx_, swapchain_.Get());
    return true;
}

void FlowGame::SubmitFidelityFXFrame() {
    FidelityFXGame& ffx = *fidelityfx_;
    ++ffx.frameId;
    ConfigureFrameGeneration(ffx, swapchain_.Get());
    RegisterUiResource(ffx);
    ffxDispatchDescFrameGenerationPrepareV2 prepare{};
    prepare.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_V2;
    prepare.frameID = ffx.frameId;
    prepare.commandList = list_.Get();
    prepare.renderSize = {width_, height_};
    prepare.motionVectorScale = {1.0f, 1.0f};
    prepare.frameTimeDelta = 6.9f;
    prepare.cameraNear = 0.1f;
    prepare.cameraFar = 1000.0f;
    prepare.cameraFovAngleVertical = 1.0f;
    prepare.viewSpaceToMetersFactor = 1.0f;
    prepare.depth = ffxApiGetResourceDX12(depth_.Get(), FFX_API_RESOURCE_STATE_COMMON);
    prepare.motionVectors = ffxApiGetResourceDX12(motionVectors_.Get(), FFX_API_RESOURCE_STATE_COMMON);
    ffx.dispatch(&ffx.frameGenerationContext, &prepare.header);
}

void FlowGame::DestroyFidelityFXContexts() {
    if (!fidelityfx_)
        return;
    if (fidelityfx_->frameGenerationContext)
        fidelityfx_->destroyContext(&fidelityfx_->frameGenerationContext, nullptr);
    if (fidelityfx_->swapchainContext)
        fidelityfx_->destroyContext(&fidelityfx_->swapchainContext, nullptr);
    fidelityfx_->enabled = false;
}

// The runtime module stays loaded with the hook, as a game keeps it until exit.
void FlowGame::EndFidelityFX() {
    DestroyFidelityFXContexts();
    delete fidelityfx_;
    fidelityfx_ = nullptr;
}

}  // namespace ce::flow
