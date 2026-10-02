#include "dx12_hook_internal.h"

namespace {

bool IsReadableRange(const void* address, size_t size) {
    if (!address || size == 0) {
        return false;
    }

    MEMORY_BASIC_INFORMATION memory = {};
    if (VirtualQuery(address, &memory, sizeof(memory)) != sizeof(memory) || memory.State != MEM_COMMIT ||
        (memory.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) {
        return false;
    }

    const uintptr_t start = reinterpret_cast<uintptr_t>(address);
    const uintptr_t end = start + size;
    const uintptr_t regionEnd = reinterpret_cast<uintptr_t>(memory.BaseAddress) + memory.RegionSize;
    return end >= start && end <= regionEnd;
}

bool GetCodeModulePath(const void* address, char (&path)[MAX_PATH]) {
    path[0] = '\0';
    HMODULE module = nullptr;
    if (!address ||
        !GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCSTR>(address), &module) ||
        !module) {
        return false;
    }
    return GetModuleFileNameA(module, path, MAX_PATH) != 0;
}

bool IsNativeD3D12CodeAddress(const void* address, char (&path)[MAX_PATH]) {
    if (!GetCodeModulePath(address, path)) {
        return false;
    }

    const char* basename = strrchr(path, '\\');
    basename = basename ? basename + 1 : path;
    return _stricmp(basename, "d3d12.dll") == 0 || _stricmp(basename, "d3d12core.dll") == 0;
}

void* FollowDirectJump(const void* address) {
    constexpr size_t kInstructionBytes = 16;
    if (!IsReadableRange(address, kInstructionBytes)) {
        return nullptr;
    }

    const auto* code = static_cast<const uint8_t*>(address);
    if (code[0] == 0xE9) {
        int32_t displacement = 0;
        memcpy(&displacement, code + 1, sizeof(displacement));
        const uintptr_t nextInstruction = reinterpret_cast<uintptr_t>(code) + 5;
        return reinterpret_cast<void*>(nextInstruction + static_cast<intptr_t>(displacement));
    }

    size_t instructionSize = 0;
    size_t displacementOffset = 0;
    if (code[0] == 0xFF && code[1] == 0x25) {
        instructionSize = 6;
        displacementOffset = 2;
    } else if (code[0] == 0x48 && code[1] == 0xFF && code[2] == 0x25) {
        instructionSize = 7;
        displacementOffset = 3;
    } else {
        return nullptr;
    }

    int32_t displacement = 0;
    memcpy(&displacement, code + displacementOffset, sizeof(displacement));
    const uintptr_t nextInstruction = reinterpret_cast<uintptr_t>(code) + instructionSize;
    const void* pointerAddress =
        reinterpret_cast<const void*>(nextInstruction + static_cast<intptr_t>(displacement));
    if (!IsReadableRange(pointerAddress, sizeof(void*))) {
        return nullptr;
    }

    void* target = nullptr;
    memcpy(static_cast<void*>(&target), pointerAddress, sizeof(target));
    return target;
}

void TryResolveMethodsFromVTable(void** vtable, const char* source) {
    constexpr size_t kRequiredSlots = 15;
    if (!IsReadableRange(static_cast<const void*>(vtable), kRequiredSlots * sizeof(void*))) {
        return;
    }

    if (!dx12_hook_g_RealD3D12ECL.load(std::memory_order_acquire)) {
        TryPublishRealD3D12ECLCandidate(reinterpret_cast<ExecuteCommandListsPtr>(vtable[10]), source);
    }
    TryPublishRealD3D12SignalCandidate(reinterpret_cast<SignalPtr>(vtable[14]), source);
}

}  // namespace

bool TryPublishRealD3D12SignalCandidate(SignalPtr candidate, const char* source) {
    if (!candidate || dx12_hook_g_RealD3D12Signal.load(std::memory_order_acquire)) {
        return dx12_hook_g_RealD3D12Signal.load(std::memory_order_acquire) != nullptr;
    }

    char modulePath[MAX_PATH] = {};
    if (!IsNativeD3D12CodeAddress(reinterpret_cast<void*>(candidate), modulePath)) {
        return false;
    }

    SignalPtr expected = nullptr;
    if (dx12_hook_g_RealD3D12Signal.compare_exchange_strong(expected, candidate, std::memory_order_acq_rel,
                                                            std::memory_order_acquire)) {
        HookLogImportant("DX12: Real D3D12 Signal resolved passively from %s: %p (%s)", source,
                         reinterpret_cast<void*>(candidate), modulePath);
    }
    return dx12_hook_g_RealD3D12Signal.load(std::memory_order_acquire) != nullptr;
}

bool TryPublishRealD3D12ECLCandidate(ExecuteCommandListsPtr candidate, const char* source) {
    if (!candidate) {
        return false;
    }

    void* selected = reinterpret_cast<void*>(candidate);
    char modulePath[MAX_PATH] = {};
    void* jumpTarget = FollowDirectJump(selected);
    if (jumpTarget) {
        char jumpModulePath[MAX_PATH] = {};
        if (!IsNativeD3D12CodeAddress(jumpTarget, jumpModulePath)) {
            return false;
        }
        selected = jumpTarget;
        memcpy(modulePath, jumpModulePath, sizeof(modulePath));
    } else if (!IsNativeD3D12CodeAddress(selected, modulePath)) {
        return false;
    }

    auto resolved = reinterpret_cast<ExecuteCommandListsPtr>(selected);
    ExecuteCommandListsPtr expected = nullptr;
    if (dx12_hook_g_RealD3D12ECL.compare_exchange_strong(expected, resolved, std::memory_order_acq_rel,
                                                         std::memory_order_acquire)) {
        HookLogImportant("DX12: Real D3D12 ECL resolved passively from %s: %p (%s)", source, selected, modulePath);
    }
    return dx12_hook_g_RealD3D12ECL.load(std::memory_order_acquire) != nullptr;
}

void ProbeRealD3D12ECL(ID3D12Device* device) {
    static std::atomic<uint64_t> s_lastUnresolvedCaptureGeneration{UINT64_MAX};
    const uint64_t captureGeneration =
        dx12_hook_g_ExecuteCommandListsCaptureGeneration.load(std::memory_order_acquire);
    if (!dx12_hook_g_RealD3D12ECL.load(std::memory_order_acquire) &&
        s_lastUnresolvedCaptureGeneration.load(std::memory_order_acquire) == captureGeneration) {
        return;
    }

    size_t trackedVtableCount = 0;
    {
        std::lock_guard<std::recursive_mutex> lock(dx12_hook_g_ExecuteCommandListsHookStateMutex);
        trackedVtableCount = dx12_hook_g_ExecuteCommandListsOriginalByVTable.size();

        if (!dx12_hook_g_RealD3D12ECL.load(std::memory_order_acquire)) {
            TryPublishRealD3D12ECLCandidate(oExecuteCommandLists, "captured primary queue original");
            TryPublishRealD3D12ECLCandidate(
                dx12_hook_g_LastExecuteCommandListsOriginal.load(std::memory_order_acquire),
                "last tracked queue original");
            for (const auto& [vtable, original] : dx12_hook_g_ExecuteCommandListsOriginalByVTable) {
                if (TryPublishRealD3D12ECLCandidate(original, "tracked queue original")) {
                    break;
                }
                TryResolveMethodsFromVTable(vtable, "tracked queue vtable");
            }
        }

        TryPublishRealD3D12SignalCandidate(oTraceCommandQueueSignal, "captured trace Signal original");
        TryResolveMethodsFromVTable(dx12_hook_g_LastExecuteCommandListsVTable.load(std::memory_order_acquire),
                                    "last tracked queue vtable");
        if (!dx12_hook_g_RealD3D12Signal.load(std::memory_order_acquire)) {
            for (const auto& [vtable, original] : dx12_hook_g_ExecuteCommandListsOriginalByVTable) {
                (void)original;
                TryResolveMethodsFromVTable(vtable, "tracked queue vtable");
                if (dx12_hook_g_RealD3D12Signal.load(std::memory_order_acquire)) {
                    break;
                }
            }
        }
    }

    if (dx12_hook_g_RealD3D12ECL.load(std::memory_order_acquire)) {
        s_lastUnresolvedCaptureGeneration.store(UINT64_MAX, std::memory_order_release);
        dx12_hook_g_ProbeRealD3D12ECLDeferred.store(false, std::memory_order_release);
        return;
    }

    // Creating a diagnostic queue mutates the live D3D12/Streamline device and
    // can deadlock an engine transition even after the nominal startup window.
    // Keep the request pending until an existing queue hook captures a native
    // original; PostSL already has a guarded selected-queue fallback meanwhile.
    s_lastUnresolvedCaptureGeneration.store(captureGeneration, std::memory_order_release);
    dx12_hook_g_ProbeRealD3D12ECLDeferred.store(true, std::memory_order_release);
    static std::atomic<int> s_passiveResolutionPendingLogs{0};
    const int logCount = s_passiveResolutionPendingLogs.fetch_add(1, std::memory_order_relaxed);
    if (logCount < 10 || (logCount % 120) == 0) {
        HookLogImportant(
            "DX12: Passive real ECL resolution pending; refusing temporary queue creation during live runtime "
            "(device=%p trackedVtables=%zu attempt=%d)",
            device, trackedVtableCount, logCount + 1);
    }
}
