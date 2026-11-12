// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall
// A scenario's Present-chain overlays (GameOptions::presentChainOverlays): the stand-ins (fakes/overlay/
// present_chain_layer.cpp) are copied into the scenario's folder and loaded there - an overlay loaded from anywhere else
// would be reported as one that leaked into the run - and wrapped around the swapchain that reaches the screen, the
// first listed outermost, as the overlays a game's own swapchain or AMD's real swapchain passes through.

#include <filesystem>
#include <string>
#include <vector>

#include "tests/flow/flow_host.h"

namespace {

using ce::flow::ComPtr;
using ce::flow::OverlayStandIn;

using WrapSwapchain = IDXGISwapChain4* (*)(IDXGISwapChain4*, ID3D12CommandQueue*, uint32_t);
using WrapQueue = ID3D12CommandQueue* (*)(ID3D12CommandQueue*);

struct LoadedOverlay {
    WrapSwapchain wrapSwapchain = nullptr;
    uint32_t drawPresents = 0;
};

// Set before any swapchain exists; read later on the runtime's presenter thread. One scenario runs per process.
std::vector<LoadedOverlay> g_overlays;  // outermost first
WrapQueue g_wrapQueue = nullptr;        // ReShade's

struct StandInPaths {
    const char* built;     // relative to the executable
    const char* scenario;  // relative to the scenario's folder
};

StandInPaths PathsOf(OverlayStandIn overlay) {
    switch (overlay) {
        case OverlayStandIn::kReShade:
            return {"present_chain\\ReShade64.dll", "ReShade64.dll"};
        case OverlayStandIn::kSteam:
            return {"present_chain\\GameOverlayRenderer64.dll", "GameOverlayRenderer64.dll"};
        case OverlayStandIn::kEpic:
            return {"present_chain\\EOSOVH-Win64-Shipping.dll", "EOSOVH-Win64-Shipping.dll"};
        case OverlayStandIn::kUbisoft:
            // CE takes an overlay64.dll for Ubisoft Connect's only inside its launcher's folder.
            return {"present_chain\\ubisoft\\overlay64.dll", "Ubisoft Game Launcher\\overlay64.dll"};
        case OverlayStandIn::kRockstar:
            return {"present_chain\\socialclub.dll", "socialclub.dll"};
    }
    return {"", ""};
}

ComPtr<IDXGISwapChain4> WrapInPresentChain(IDXGISwapChain4* swapchain, ID3D12CommandQueue* queue) {
    ComPtr<IDXGISwapChain4> current = swapchain;
    for (auto overlay = g_overlays.rbegin(); overlay != g_overlays.rend(); ++overlay) {
        ComPtr<IDXGISwapChain4> next;
        next.Attach(overlay->wrapSwapchain(current.Get(), queue, overlay->drawPresents));
        if (!next)
            return nullptr;
        current = next;
    }
    return current;
}

}  // namespace

// The fake FidelityFX runtime's real swapchain and present queue as the scenario's overlays hand them out: the
// swapchain to present through and the queue to submit to. Returns new references; without overlays the originals.
extern "C" __declspec(dllexport) void CEFlowGame_WrapRuntimePresentation(IDXGISwapChain4* real,
                                                                        ID3D12CommandQueue* nativeQueue,
                                                                        IDXGISwapChain4** presentThrough,
                                                                        ID3D12CommandQueue** submitThrough) {
    const ComPtr<IDXGISwapChain4> probed = ce::flow::WrapRuntimeOutputProbe(real, nativeQueue);
    *presentThrough = WrapInPresentChain(probed.Get(), nativeQueue).Detach();
    if (g_wrapQueue) {
        *submitThrough = g_wrapQueue(nativeQueue);
    } else {
        nativeQueue->AddRef();
        *submitThrough = nativeQueue;
    }
}

// A runtime queue as ReShade hands it out (its own object when ReShade is loaded): the queue of a runtime whose
// swapchain another presenter wraps (GameOptions::fsrNestedPresenter). Returns a new reference.
extern "C" __declspec(dllexport) ID3D12CommandQueue* CEFlowGame_WrapRuntimeQueue(ID3D12CommandQueue* nativeQueue) {
    if (g_wrapQueue)
        return g_wrapQueue(nativeQueue);
    nativeQueue->AddRef();
    return nativeQueue;
}

namespace ce::flow {

bool FlowGame::LoadPresentChainOverlays(const std::vector<PresentChainOverlay>& overlays) {
    char path[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    const std::filesystem::path executableDirectory = std::filesystem::path(path).parent_path();
    for (const PresentChainOverlay& overlay : overlays) {
        const StandInPaths paths = PathsOf(overlay.overlay);
        const std::filesystem::path target = std::filesystem::path(logDirectory_) / paths.scenario;
        std::error_code error;
        std::filesystem::create_directories(target.parent_path(), error);
        if (!std::filesystem::exists(target) &&
            !std::filesystem::copy_file(executableDirectory / paths.built, target, error))
            return Fail(("copying the overlay stand-in " + std::string(paths.built)).c_str(), E_FAIL);
        const HMODULE module = LoadLibraryW(target.c_str());
        if (!module)
            return Fail(("LoadLibrary(" + std::string(paths.scenario) + ")").c_str(),
                        HRESULT_FROM_WIN32(GetLastError()));
        LoadedOverlay loaded;
        loaded.wrapSwapchain = reinterpret_cast<WrapSwapchain>(GetProcAddress(module, "CEFakeOverlay_WrapSwapchain"));
        loaded.drawPresents = overlay.drawPresents;
        if (!loaded.wrapSwapchain)
            return Fail("resolving CEFakeOverlay_WrapSwapchain", E_NOINTERFACE);
        if (overlay.overlay == OverlayStandIn::kReShade) {
            g_wrapQueue = reinterpret_cast<WrapQueue>(GetProcAddress(module, "CEFakeOverlay_WrapQueue"));
            if (!g_wrapQueue)
                return Fail("resolving CEFakeOverlay_WrapQueue", E_NOINTERFACE);
        }
        g_overlays.push_back(loaded);
    }
    return RefreshPresentChainQueue();
}

bool FlowGame::RefreshPresentChainQueue() {
    // ReShade hands the game its own object for every new queue, including a replacement after FG is disabled.
    submitQueue_.Reset();
    if (g_wrapQueue) {
        submitQueue_.Attach(g_wrapQueue(queue_.Get()));
        if (!submitQueue_)
            return Fail("wrapping the game's command queue", E_FAIL);
    }
    return true;
}

bool FlowGame::WrapGameSwapchain(ComPtr<IDXGISwapChain1>& swapchain) {
    if (g_overlays.empty())
        return true;
    ComPtr<IDXGISwapChain4> real;
    HRESULT hr = swapchain.As(&real);
    if (FAILED(hr))
        return Fail("QueryInterface(IDXGISwapChain4) for the Present-chain overlays", hr);
    const ComPtr<IDXGISwapChain4> wrapped = WrapInPresentChain(real.Get(), queue_.Get());
    if (!wrapped)
        return Fail("wrapping the game's swapchain in its Present-chain overlays", E_FAIL);
    hr = wrapped.As(&swapchain);
    if (FAILED(hr))
        return Fail("QueryInterface(IDXGISwapChain1) on the Present-chain overlays", hr);
    return true;
}

}  // namespace ce::flow
