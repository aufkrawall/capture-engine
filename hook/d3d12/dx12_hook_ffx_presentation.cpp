#include "dx12_ffx_presentation.h"

#include "dx12_hook_internal.h"
#include "dx12_queue_identity.h"
#include "hook/fg/fg_presenter.h"

#include <psapi.h>

#include <atomic>
#include <cstdio>
#include <cstring>

namespace {

using ce::dx12_overlay_policy::PresenterFrame;
using ce::dx12_overlay_policy::PresenterImages;

struct ThreadPresentation {
    DX12FFXPresentation view;
    uint64_t registrationEpoch = 0;
    const void* originatorFrame = nullptr;
    uintptr_t runtimeBase = 0;
    bool valid = false;
};

thread_local ThreadPresentation t_presentation;

HMODULE ModuleOf(const void* address) {
    HMODULE module = nullptr;
    if (!address ||
        !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(address), &module)) {
        return nullptr;
    }
    return module;
}

bool ImageRange(HMODULE module, uintptr_t* base, uintptr_t* end) {
    MODULEINFO info = {};
    if (!module || !GetModuleInformation(GetCurrentProcess(), module, &info, sizeof(info)) ||
        info.lpBaseOfDll != module || info.SizeOfImage == 0) {
        return false;
    }
    *base = reinterpret_cast<uintptr_t>(module);
    *end = *base + info.SizeOfImage;
    return true;
}

void LogPresentationChange(const DX12FFXPresentation& view, const ce::fg_presenter::Binding& binding,
                           const void* originatorFrame) {
    static std::atomic<uint32_t> s_logs{0};
    if (!ce::log_meter::ShouldLogCadence(s_logs.fetch_add(1, std::memory_order_relaxed) + 1, 20, 300)) {
        return;
    }
    char originator[MAX_PATH + 32] = {};
    char creator[MAX_PATH + 32] = {};
    DX12_DescribeCodeAddress(reinterpret_cast<uintptr_t>(originatorFrame), originator, sizeof(originator));
    DX12_DescribeCodeAddress(reinterpret_cast<uintptr_t>(binding.creator), creator, sizeof(creator));
    HookLogImportant(
        "DX12: No-callback FSR presented swapchain %p owns its buffers on queue %p (registered=%p tid=0x%04lX "
        "originator=%s creator=%s)%s",
        view.swapChain, view.ownerQueue, binding.queue, static_cast<unsigned long>(GetCurrentThreadId()), originator,
        creator,
        !view.ownerQueue ? "; unknown owner: no batch can carry CE's draw, the UI baseline keeps the overlay"
        : view.nestedBase != 0
            ? "; its creator presents it from inside the runtime's Present: that module's own batches on the queue "
              "compose the back buffers"
            : "; only the runtime's batches on that queue can carry CE's draw");
}

}  // namespace

const DX12FFXPresentation& DX12_RefreshFFXPresentation(IDXGISwapChain* swapChain, const void* presentOriginatorFrame) {
    const uint64_t epoch = ce::fg_presenter::RegistrationEpoch();
    uintptr_t runtimeBase = 0;
    uintptr_t runtimeEnd = 0;
    const bool runtimeKnown = DX12_GetPresentingFFXRuntimeImage(&runtimeBase, &runtimeEnd);
    ThreadPresentation& cached = t_presentation;
    if (cached.valid && cached.view.swapChain == swapChain && cached.registrationEpoch == epoch &&
        cached.originatorFrame == presentOriginatorFrame && cached.runtimeBase == runtimeBase) {
        return cached.view;
    }
    const DX12FFXPresentation previous = cached.view;
    const ce::fg_presenter::Binding binding =
        swapChain ? ce::fg_presenter::BindingFor(swapChain) : ce::fg_presenter::Binding{};
    // A proxy stack may sit between the registered create argument and the queue whose ECL CE observes.
    const auto nativeQueue = ce::dx12_queue_identity::AcquireNativeQueue(binding.queue);
    DX12FFXPresentation view;
    view.swapChain = swapChain;
    view.ownerQueue = nativeQueue.Get();
    const auto originator = reinterpret_cast<uintptr_t>(presentOriginatorFrame);
    const HMODULE originatorModule = ModuleOf(presentOriginatorFrame);
    if (runtimeKnown && ce::dx12_overlay_policy::IsNestedSwapchainPresenter(
                            reinterpret_cast<uintptr_t>(originatorModule), reinterpret_cast<uintptr_t>(binding.creator),
                            originator >= runtimeBase && originator < runtimeEnd) &&
        ImageRange(originatorModule, &view.nestedBase, &view.nestedEnd)) {
        view.nestedPresentSite = originator;
    }
    cached = {view, epoch, presentOriginatorFrame, runtimeBase, true};
    if (previous.swapChain != view.swapChain || previous.ownerQueue != view.ownerQueue ||
        previous.nestedBase != view.nestedBase) {
        LogPresentationChange(view, binding, presentOriginatorFrame);
    }
    return cached.view;
}

const DX12FFXPresentation& DX12_CurrentFFXPresentation() {
    return t_presentation.view;
}

bool DX12_CurrentFFXPresenterImages(bool includeNested, PresenterImages* images) {
    PresenterImages current;
    if (!images || !DX12_GetPresentingFFXRuntimeImage(&current.runtimeBase, &current.runtimeEnd)) {
        return false;
    }
    if (includeNested) {
        current.nestedBase = t_presentation.view.nestedBase;
        current.nestedEnd = t_presentation.view.nestedEnd;
    }
    *images = current;
    return true;
}

PresenterFrame DX12_FindNearestFFXPresenterFrame(const PresenterImages& images) {
    constexpr USHORT kNearFrames = 16;
    constexpr USHORT kFarFrames = 112;
    void* frames[kFarFrames] = {};
    uintptr_t addresses[kFarFrames] = {};
    const auto search = [&](DWORD skip, USHORT capacity, USHORT* captured) {
        *captured = CaptureStackBackTrace(skip, capacity, frames, nullptr);
        for (USHORT i = 0; i < *captured; ++i) {
            addresses[i] = reinterpret_cast<uintptr_t>(frames[i]);
        }
        return ce::dx12_overlay_policy::NearestPresenterFrame(addresses, *captured, images);
    };
    USHORT captured = 0;
    const PresenterFrame nearby = search(1, kNearFrames, &captured);
    if (nearby.image != ce::dx12_overlay_policy::PresenterImage::kNone || captured < kNearFrames) {
        return nearby;  // found, or the stack ended
    }
    return search(1 + kNearFrames, kFarFrames, &captured);
}

void DX12_DescribeCodeAddress(uintptr_t address, char* out, size_t outSize) {
    if (!out || outSize == 0) {
        return;
    }
    const HMODULE module = ModuleOf(reinterpret_cast<const void*>(address));
    char path[MAX_PATH] = {};
    if (module && GetModuleFileNameA(module, path, MAX_PATH) > 0) {
        const char* baseName = std::strrchr(path, '\\');
        std::snprintf(out, outSize, "%s+0x%llX", baseName ? baseName + 1 : path,
                      static_cast<unsigned long long>(address - reinterpret_cast<uintptr_t>(module)));
        return;
    }
    std::snprintf(out, outSize, "%p", reinterpret_cast<void*>(address));
}
