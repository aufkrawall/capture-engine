// Entry point of the flow-test build of the hook DLL (build/tests/flow/capture_hook_x64.dll); it replaces
// hook/runtime/main_dllmain.cpp and the hook thread. The test host loads the DLL like an injection and runs
// CE's own hook installation and hook-thread service passes synchronously, so a scenario is one thread of
// deterministic calls. The DLL never reaches a running CaptureEngine: no IPC, no discovery mapping, its own
// log directory, and the host's never-activated window stands in for the foreground window.

#include "common/logging/log_meter.h"
#include "hook/d3d12/dx12_hook.h"
#include "hook/d3d12/dx12_hook_internal.h"
#include "hook/d3d12/dx12_hook_ecl_forward.h"
#include "hook/d3d12/dx12_device_trace.h"
#include "hook/overlay/custom_overlay_dx12.h"
#include "hook/present/dxgi_shared_internal.h"
#include "hook/runtime/hook_clock.h"
#include "hook/runtime/main_internal.h"
#include "hook/wrappers/wrapper_hooks.h"
#include "hook/wrappers/dxgi_swapchain_wrap.h"
#include "tests/flow/flow_api.h"
#include "tests/flow/runtime_output_frame_tracker.h"

#include <wrl/client.h>

#include <cstring>
#include <mutex>
#include <vector>

extern "C" BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        // Before anything reads the time: CE runs on the game's frame clock from its first reading.
        ce::hook_clock::EnableVirtual();
        g_hModule = module;
        DisableThreadLibraryCalls(module);
        char logsDirectory[MAX_PATH] = {};
        if (!GetEnvironmentVariableA(kCEFlowLogDirectoryVariable, logsDirectory, MAX_PATH)) {
            GetModuleFileNameA(module, logsDirectory, MAX_PATH);
            if (char* slash = strrchr(logsDirectory, '\\'))
                strcpy_s(slash, logsDirectory + MAX_PATH - slash, "\\logs");
        }
        CreateDirectoryA(logsDirectory, nullptr);
        IsolateHookFromCaptureEngineHost(logsDirectory);
        char exePath[MAX_PATH] = {};
        if (GetModuleFileNameA(nullptr, exePath, MAX_PATH)) {
            const char* slash = strrchr(exePath, '\\');
            strncpy_s(g_ProcessName, slash ? slash + 1 : exePath, _TRUNCATE);
        }
    } else if (reason == DLL_PROCESS_DETACH && reserved != nullptr) {
        // As main_dllmain.cpp: at process exit every entry point turns into a pass-through.
        g_ProcessTerminating.store(true, std::memory_order_release);
        RequestHookShutdown();
    }
    return TRUE;
}

// The host's shared memory attached to CE's IPC client as main_hookthread.cpp attaches a connected one.
void AttachIsolatedHost(SharedMemoryLayout* memory) {
    g_IPC = new IPCClient();
    g_IPC->AttachIsolatedHostMemory(memory);
    g_pSharedMem = memory;
    g_pSharedMem->SetSourcePid(GetCurrentProcessId());
    ce::CreateHookContext();
    if (auto* context = ce::GetHookContext()) {
        context->hookModule = g_hModule;
        ce::SyncWithLegacyGlobals();
        context->hookLifecycle.TransitionTo(ce::HookState::Connected);
    }
}

// What DllMain and the hook thread do before the hook thread's service loop, minus threads and the IPC
// connection (main_dllmain.cpp, then main_hookthread.cpp up to InstallHookThreadHooks).
extern "C" __declspec(dllexport) bool CEFlow_Init(const char* configPath, SharedMemoryLayout* hostMemory) {
    if (!hostMemory)
        return false;
    InitializeHookLifecycleControl();
    EnsureLocalConfigAllocated();
    LoadConfig(configPath, *g_pLocalConfig);
    g_pLocalConfig->graphics.postProcessDisplayGamma = "srgb";
    g_LocalConfigLoaded.store(true, std::memory_order_release);
    GetActiveGraphicsConfig();
    AttachIsolatedHost(hostMemory);
    InstallKernel32LoaderHooks("DllMain");
    InitializeWrapperHooks();
    InstallGlobalVTableHooks();
    InitializeThirdPartyOverlayDetection();
    InstallHookThreadHooks();
    HookLogImportant("CEFlow: hook initialized for an isolated test host (config=%s)", configPath);
    return true;
}

// One pass of the hook thread's service loop (main_hookthread.cpp) that matters to a D3D12 game.
extern "C" __declspec(dllexport) void CEFlow_PumpHookThread() {
    RefreshThirdPartyOverlayIdentityCache();
    CheckAndInstallHooks();
    if (g_DX12Hook)
        g_DX12Hook->ServicePendingPresentHooks();
    CustomOverlay::CollectRetiredDX12Backends();
}

extern "C" __declspec(dllexport) void CEFlow_SetForegroundWindow(HWND window) {
    SetIsolatedHostForegroundWindow(window);
}

namespace {

// The game frame of each frame generation runtime output against the frame CE attributed it to: one constant
// offset per presenter lifetime; allocation addresses alone do not identify a lifetime.
struct OutputFrameCheck {
    std::mutex mutex;
    ce::flow::RuntimeOutputFrameTracker tracker;
    ce::log_meter::ChangeGate mismatchLog;
    uint64_t checks = 0;
    uint64_t mismatches = 0;
    uint64_t ownerViolations = 0;
} g_OutputFrames;

}  // namespace

extern "C" __declspec(dllexport) void CEFlow_NoteRuntimeOutputFrame(const void* presenter, uint64_t lifetime,
                                                                       uint64_t frame) {
    DX12FFXOutputAttribution output;
    if (!DX12_PeekFFXOutputAttribution(&output)) {
        // Composed without passing CE's no-callback route at all - while AMD composes a frame only that route
        // draws (its UI baseline is retired), so the output shows no overlay.
        if (DX12_IsFFXComposingFrameOwnedByTopmost()) {
            std::lock_guard<std::mutex> lock(g_OutputFrames.mutex);
            ++g_OutputFrames.checks;
            ++g_OutputFrames.ownerViolations;
            HookLogImportant("CEFlow: runtime output of game frame %llu composed outside the final-batch route while "
                             "its frame is topmost-owned",
                             static_cast<unsigned long long>(frame));
        }
        return;
    }
    std::lock_guard<std::mutex> lock(g_OutputFrames.mutex);
    const int64_t offset = static_cast<int64_t>(output.frame) - static_cast<int64_t>(frame);
    ++g_OutputFrames.checks;
    if (output.record.owner != ce::dx12_overlay_policy::FFXFrameOverlayOwner::kUnknown) {
        const auto verdict = ce::dx12_overlay_policy::JudgeFrameExactFFXOutput(output.record.owner, output.topmostDrawn);
        if (!verdict.covered || verdict.doubleDrawn) {
            ++g_OutputFrames.ownerViolations;
            HookLogImportant("CEFlow: runtime output of game frame %llu (CE frame %llu, owner=%s) %s",
                             static_cast<unsigned long long>(frame), static_cast<unsigned long long>(output.frame),
                             ce::dx12_overlay_policy::FFXFrameOverlayOwnerName(output.record.owner),
                             verdict.doubleDrawn ? "drawn by the final-batch route over its UI baseline"
                                                 : "without its owner's draw");
        }
    }
    const auto observation = g_OutputFrames.tracker.Observe(presenter, lifetime, offset);
    const auto logVerdict = g_OutputFrames.mismatchLog.Observe(
        ce::log_meter::FieldKey(lifetime, offset, g_OutputFrames.tracker.Offset()));
    if (observation == ce::flow::RuntimeOutputFrameTracker::Observation::kFirstInLifetime) {
        HookLogImportant("CEFlow: runtime presenter lifetime=%llu presenter=%p initialOffset=%lld",
                         static_cast<unsigned long long>(lifetime), presenter, static_cast<long long>(offset));
    } else if (observation == ce::flow::RuntimeOutputFrameTracker::Observation::kMismatch) {
        ++g_OutputFrames.mismatches;
        if (logVerdict.log) {
            HookLogImportant("CEFlow: runtime output of game frame %llu attributed to CE frame %llu (offset %lld, "
                             "earlier outputs %lld; owner=%s topmostDrawn=%d lifetime=%llu mismatches=%llu)%s",
                             static_cast<unsigned long long>(frame), static_cast<unsigned long long>(output.frame),
                             static_cast<long long>(offset), static_cast<long long>(g_OutputFrames.tracker.Offset()),
                             ce::dx12_overlay_policy::FFXFrameOverlayOwnerName(output.record.owner),
                             output.topmostDrawn ? 1 : 0, static_cast<unsigned long long>(lifetime),
                             static_cast<unsigned long long>(g_OutputFrames.mismatches),
                             ce::log_meter::SuppressedNote(logVerdict.suppressed).c_str());
        }
    }
}

extern "C" __declspec(dllexport) void CEFlow_GetOverlayCoverage(CEFlowOverlayCoverage* out) {
    const DX12OverlayCoverageSnapshot snapshot = GetOverlayCoverageSnapshot();
    out->presents = snapshot.totalPresents;
    out->uncovered = snapshot.uncoveredPresents;
    out->currentUncoveredStreak = snapshot.currentStreak;
    out->longestUncoveredStreak = snapshot.longestStreak;
    out->doubleDraws = snapshot.doubleDraws;
    std::lock_guard<std::mutex> lock(g_OutputFrames.mutex);
    out->outputFrameChecks = g_OutputFrames.checks;
    out->outputFrameMismatches = g_OutputFrames.mismatches;
    out->outputOwnerViolations = g_OutputFrames.ownerViolations;
}

extern "C" __declspec(dllexport) void CEFlow_GetPostProcess(CEFlowPostProcess* out) {
    const DX12PostProcessSnapshot snapshot = GetPostProcessSnapshot();
    out->frames = snapshot.frames;
    out->corrected = snapshot.corrected;
    out->covered = snapshot.covered;
    out->uncorrected = snapshot.uncorrected;
    out->gapRuns = snapshot.gapRuns;
    out->failed = snapshot.failed;
    out->unclassified = snapshot.unclassified;
    out->leftBeforeDecision = snapshot.leftBeforeDecision;
    out->routeApplied = snapshot.routeApplied;
    out->routeFailed = snapshot.routeFailed;
}

extern "C" __declspec(dllexport) void CEFlow_GetPublishedFG(CEFlowPublishedFG* out) {
    *out = {};
    if (PerformanceMetrics* metrics = DXGIShared::GetPerformanceMetrics()) {
        out->type = metrics->GetFGType();
        out->multiplier = metrics->GetFGMultiplier();
    }
}

// The descriptor-free overlay backend (the one an x64 game's overlay draws with) drawing one opaque red quad into a
// fresh render target of each requested format on the game's device. The backend is built for formats[0] and then
// follows every other format by SetTargetFormat, as EnsureDescFreeBackendForDeviceAndFormat makes it follow a game
// that changes its back buffers' format. The host's debug layer validates that the pipelines agree with every
// render target; `firstPixels` receives each target's first texel as stored.
extern "C" __declspec(dllexport) bool CEFlow_ProbeDescFreeTargetFormats(ID3D12Device* device, ID3D12CommandQueue* queue,
                                                                          const int* formats, uint32_t count,
                                                                          uint32_t* firstPixels,
                                                                          uint32_t* pipelineFormats) {
    constexpr UINT kSize = 16;
    constexpr uint32_t kMaxTargets = 8;  // one upload-ring slot each, so no slot is reused
    constexpr uint32_t kRed = 0xFF0000FF;  // ABGR
    if (!device || !queue || !formats || !firstPixels || !pipelineFormats || count == 0 ||
        count > kMaxTargets) {
        return false;
    }
    ID3D12Fence* const savedFence = dx12_hook_s_descFreeSlotFence;
    const UINT64 savedGuard = dx12_hook_s_descFreeSlotGuardValue;
    dx12_hook_s_descFreeSlotFence = nullptr;
    dx12_hook_s_descFreeSlotGuardValue = 0;
    bool ok = false;
    {
        DX12DescFreeBackend backend;
        Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> rtvHeap;
        Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
        Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list;
        Microsoft::WRL::ComPtr<ID3D12Fence> fence;
        Microsoft::WRL::ComPtr<ID3D12Resource> readback;
        const D3D12_DESCRIPTOR_HEAP_DESC heapDesc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
        D3D12_HEAP_PROPERTIES readbackHeap{};
        readbackHeap.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC readbackDesc{};
        readbackDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        readbackDesc.Width = D3D12_TEXTURE_DATA_PITCH_ALIGNMENT * kSize;
        readbackDesc.Height = 1;
        readbackDesc.DepthOrArraySize = 1;
        readbackDesc.MipLevels = 1;
        readbackDesc.SampleDesc.Count = 1;
        readbackDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        const uint8_t font[8 * 8 * 4] = {};
        HANDLE done = CreateEventA(nullptr, FALSE, FALSE, nullptr);
        bool setup = done && backend.InitDevice(device, static_cast<DXGI_FORMAT>(formats[0])) &&
                     backend.Initialize(8, 8, font) &&
                     SUCCEEDED(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&rtvHeap))) &&
                     SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) &&
                     SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                                         IID_PPV_ARGS(&list))) &&
                     SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))) &&
                     SUCCEEDED(device->CreateCommittedResource(&readbackHeap, D3D12_HEAP_FLAG_NONE, &readbackDesc,
                                                               D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                               IID_PPV_ARGS(&readback)));
        if (setup) {
            list->Close();
            const std::vector<CustomOverlay::DrawVertex> vertices = {{0, 0, 0, 0, kRed},
                                                                      {float(kSize), 0, 0, 0, kRed},
                                                                      {float(kSize), float(kSize), 0, 0, kRed},
                                                                      {0, float(kSize), 0, 0, kRed}};
            const std::vector<uint16_t> indices = {0, 1, 2, 0, 2, 3};
            const std::vector<CustomOverlay::DrawCommand> commands = {{0, 4, 0, 6, false}};
            const D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvHeap->GetCPUDescriptorHandleForHeapStart();
            UINT64 fenceValue = 0;
            ok = true;
            for (uint32_t i = 0; ok && i < count; ++i) {
                const DXGI_FORMAT format = static_cast<DXGI_FORMAT>(formats[i]);
                D3D12_HEAP_PROPERTIES defaultHeap{};
                defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
                D3D12_RESOURCE_DESC targetDesc{};
                targetDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
                targetDesc.Width = kSize;
                targetDesc.Height = kSize;
                targetDesc.DepthOrArraySize = 1;
                targetDesc.MipLevels = 1;
                targetDesc.Format = format;
                targetDesc.SampleDesc.Count = 1;
                targetDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
                Microsoft::WRL::ComPtr<ID3D12Resource> target;
                ok = SUCCEEDED(device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &targetDesc,
                                                               D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr,
                                                               IID_PPV_ARGS(&target))) &&
                     (i == 0 || backend.SetTargetFormat(format));
                if (!ok)
                    break;
                device->CreateRenderTargetView(target.Get(), nullptr, rtv);
                allocator->Reset();
                list->Reset(allocator.Get(), nullptr);
                dx12_hook_s_descFreeCmdList = list.Get();
                dx12_hook_s_descFreeRtv = rtv;
                backend.SetNextUploadSlot(static_cast<int>(i));
                backend.Render(vertices, indices, commands, kSize, kSize);
                dx12_hook_s_descFreeCmdList = nullptr;
                D3D12_RESOURCE_BARRIER toCopy{};
                toCopy.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                toCopy.Transition.pResource = target.Get();
                toCopy.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
                toCopy.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
                toCopy.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                list->ResourceBarrier(1, &toCopy);
                D3D12_TEXTURE_COPY_LOCATION source{};
                source.pResource = target.Get();
                source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                D3D12_TEXTURE_COPY_LOCATION destination{};
                destination.pResource = readback.Get();
                destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                destination.PlacedFootprint.Footprint = {format, kSize, kSize, 1, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT};
                list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
                ok = SUCCEEDED(list->Close());
                if (!ok)
                    break;
                ID3D12CommandList* lists[] = {list.Get()};
                queue->ExecuteCommandLists(1, lists);
                ok = SUCCEEDED(queue->Signal(fence.Get(), ++fenceValue)) &&
                     SUCCEEDED(fence->SetEventOnCompletion(fenceValue, done)) &&
                     WaitForSingleObject(done, 10000) == WAIT_OBJECT_0;
                void* mapped = nullptr;
                const D3D12_RANGE readRange{0, sizeof(uint32_t)};
                if (ok && SUCCEEDED(readback->Map(0, &readRange, &mapped)) && mapped) {
                    std::memcpy(&firstPixels[i], mapped, sizeof(uint32_t));
                    const D3D12_RANGE written{0, 0};
                    readback->Unmap(0, &written);
                } else {
                    ok = false;
                }
            }
            *pipelineFormats = static_cast<uint32_t>(backend.PipelineFormatCount());
            HookLogImportant("CEFlow: descriptor-free target format probe %s (targets=%u pipelineFormats=%u)",
                             ok ? "ok" : "FAILED", count, *pipelineFormats);
        }
        if (done)
            CloseHandle(done);
    }
    dx12_hook_s_descFreeCmdList = nullptr;
    dx12_hook_s_descFreeSlotFence = savedFence;
    dx12_hook_s_descFreeSlotGuardValue = savedGuard;
    return ok;
}

extern "C" __declspec(dllexport) void CEFlow_AdvanceClock(int64_t microseconds) {
    ce::hook_clock::AdvanceMicroseconds(microseconds);
}

extern "C" __declspec(dllexport) int64_t CEFlow_ClockMicroseconds() {
    LARGE_INTEGER counter{};
    LARGE_INTEGER frequency{};
    ce::hook_clock::QueryCounter(&counter);
    QueryPerformanceFrequency(&frequency);
    return counter.QuadPart / frequency.QuadPart * 1000000 + counter.QuadPart % frequency.QuadPart * 1000000 /
                                                                  frequency.QuadPart;
}

extern "C" __declspec(dllexport) void CEFlow_Shutdown() {
    RequestHookShutdown();
}

// Evidence-only flow API; no mapped state or mutable atomics escape the hook.
extern "C" __declspec(dllexport) void CEFlow_GetPostSLLifecycle(CEFlowPostSLLifecycle* out) {
    if (!out) return;
    *out = {g_PostSLLifecycle.Epoch(), g_PostSLLifecycle.CallbacksInFlight(),
            g_PostSLLifecycle.CallbacksEnabled(), g_PostSLLifecycle.ConfirmedInCurrentEpoch()};
}
extern "C" __declspec(dllexport) bool CEFlow_TryConfirmPostSLEpoch(uint32_t epoch) {
    return g_PostSLLifecycle.ConfirmRender(epoch, [] {});
}

extern "C" __declspec(dllexport) void CEFlow_TrackQueue(ID3D12CommandQueue* queue) {
    DX12_HookQueueVTable(queue);
}
extern "C" __declspec(dllexport) void* CEFlow_QueueOriginal(ID3D12CommandQueue* queue) {
    return reinterpret_cast<void*>(GetOriginalExecuteCommandLists(queue));
}
extern "C" __declspec(dllexport) void CEFlow_ForwardQueue(ID3D12CommandQueue* queue) {
    ce::dx12_ecl_forward::TransparentNativeFSRCallback(queue, 0, nullptr);
}

extern "C" __declspec(dllexport) void CEFlow_ResetQueueBindings() {
    ce::dx12_queue_dispatch::Reset();
}

extern "C" __declspec(dllexport) void CEFlow_ResetDeviceTrace() {
    ce::dx12_device_trace::Reset();
}

extern "C" __declspec(dllexport) void CEFlow_TrackSignalQueue(ID3D12CommandQueue* queue) {
    DX12_HookQueueSignalVTable(queue);
}
extern "C" __declspec(dllexport) void* CEFlow_SignalOriginal(ID3D12CommandQueue* queue) {
    return reinterpret_cast<void*>(GetOriginalCommandQueueSignal(queue));
}
extern "C" __declspec(dllexport) HRESULT CEFlow_ForwardSignal(ID3D12CommandQueue* queue, UINT64 value) {
    return DetourTraceCommandQueueSignal(queue, nullptr, value);
}

extern "C" __declspec(dllexport) bool CEFlow_RemoveSignalQueue(ID3D12CommandQueue* queue) {
    void** vtable = queue ? *reinterpret_cast<void***>(queue) : nullptr;
    const auto original = GetOriginalCommandQueueSignal(queue);
    return vtable && original &&
           VTableHook::Remove(&vtable[14], reinterpret_cast<void*>(original)) == VTableHook::Success;
}

extern "C" __declspec(dllexport) void CEFlow_RepairPresentHooks() {
    DXGIShared::RepairVTableHooksIfNeeded();
}

extern "C" __declspec(dllexport) void CEFlow_ReleasePresentVTableHooks() {
    DXGIShared::ReleaseSwapchainPresentVTableHooksForRuntimeHandoff("flow vtable release");
}
extern "C" __declspec(dllexport) bool CEFlow_InstallPresentVTableHooks(IDXGISwapChain* swapchain) {
    return DXGIShared::InstallHooks(swapchain, true);
}

extern "C" __declspec(dllexport) IDXGISwapChain* CEFlow_RetainRealSwapchain(IDXGISwapChain* swapchain) {
    if (!swapchain)
        return nullptr;
    void* wrapper = nullptr;
    if (SUCCEEDED(swapchain->QueryInterface(IID_CWrapDXGISwapChain, &wrapper))) {
        auto* typed = static_cast<CWrapDXGISwapChain*>(wrapper);
        IDXGISwapChain* real = typed->GetReal();
        if (real)
            real->AddRef();
        typed->Release();
        return real;
    }
    swapchain->AddRef();
    return swapchain;
}
