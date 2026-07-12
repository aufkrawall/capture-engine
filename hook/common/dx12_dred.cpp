#include "dx12_dred.h"

// clang-format off
#include <windows.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
// clang-format on
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "dx12_overlay_policy.h"
#include "hook_common.h"

// Retention belt-and-suspenders. The DRED entry points are `__declspec(dllexport)`
// (see CE_DRED_API in the header) so the linker keeps them as GC roots on both x86
// and x64 (plain `used` was honored on x64 LLD but NOT x86, so arming was silently
// stripped on x86). `noinline` additionally keeps the call site in
// Wrapped_D3D12CreateDevice a real, side-effecting call so the arming actually runs.
#define CE_DRED_KEEP __attribute__((noinline))

namespace ce::dx12_dred {

namespace {

std::atomic<bool> s_dumpedThisEpoch{false};

// Read a flag file located next to the hook DLL into `out` (NUL-terminated, first whitespace-
// delimited token only). Returns true if the file existed. The inject model makes env vars
// awkward (the target is launched by something else), so a flag file the user just creates is
// the robust toggle — same pattern as DebugLayerLevel()'s "ce_dx12_debug_layer".
bool ReadHookDirFlagFile(const char* fileName, char* out, size_t outSize) {
    if (out && outSize) {
        out[0] = '\0';
    }
    HMODULE self = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCSTR>(&ReadHookDirFlagFile), &self) ||
        !self) {
        return false;
    }
    char path[MAX_PATH] = {};
    DWORD len = GetModuleFileNameA(self, path, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        return false;
    }
    for (DWORD i = len; i > 0; --i) {
        if (path[i - 1] == '\\' || path[i - 1] == '/') {
            path[i] = '\0';
            break;
        }
    }
    char flagPath[MAX_PATH] = {};
    _snprintf_s(flagPath, sizeof(flagPath), _TRUNCATE, "%s%s", path, fileName);
    HANDLE h = CreateFileA(flagPath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return false;
    }
    char tmp[16] = {};
    DWORD rd = 0;
    ReadFile(h, tmp, sizeof(tmp) - 1, &rd, nullptr);
    CloseHandle(h);
    for (DWORD i = 0; i < rd && i < sizeof(tmp) - 1; ++i) {
        if (tmp[i] == '\r' || tmp[i] == '\n' || tmp[i] == ' ' || tmp[i] == '\t') {
            tmp[i] = '\0';
            break;
        }
    }
    if (out && outSize) {
        strncpy_s(out, outSize, tmp, _TRUNCATE);
    }
    return true;
}

// Read CE_DX12_DRED once. Default OFF.
//
// DRED auto-breadcrumbs (SetAutoBreadcrumbsEnablement FORCED_ON) make the application's every
// ID3D12GraphicsCommandList::Reset() allocate/open a breadcrumb buffer via a KERNEL GPU
// allocation (D3D12Core Dred::AllocateBreadcrumbBuffer -> OpenExistingHeapFromAddress ->
// NtGdiDdDDICreateAllocation/DestroyAllocation). During the Alt+Tab iflip<->composited mode
// switch that kernel allocation contends with the GPU/DWM reconfiguration and stalls the
// present thread for seconds, tripping the 2 s GPU TDR (DEVICE_HUNG). The freeze dump in
// logs/20260606_145929 caught exactly that: dx12_test!Render -> CGraphicsCommandList::Reset ->
// Dred::AllocateBreadcrumbBuffer -> NtGdiDdDDIDestroyAllocation2, gap=2646ms. So the full
// (auto-breadcrumb) diagnostic itself was causing the freeze it was meant to capture, and it
// likewise shifts timing enough to mask a timing-sensitive steady-state GPU hang.
//
// DRED is therefore opt-in with two levels (env CE_DX12_DRED, or a flag file "ce_dx12_dred"
// next to the hook DLL for the inject model):
//   page-fault-only ("pf"/"2", or an EMPTY flag file): arms ONLY page-fault output — no
//     auto-breadcrumbs, no per-Reset kernel allocation. Low perturbation; still captures the
//     faulting GPU VA + recently-freed/existing allocations on a DEVICE_HUNG. Use this for the
//     uncapped steady-state repro.
//   full ("1"/"on"/"true"/"yes"/"full"): auto-breadcrumbs + page-fault + context. Highest
//     detail (names the exact hung op) but high perturbation — only for hangs the low-overhead
//     mode reports as a "pure hang".
ce::dx12_overlay_policy::DredArmMode ReadArmModeFromEnvAndFile() {
    using Mode = ce::dx12_overlay_policy::DredArmMode;
    char buf[16] = {};
    DWORD n = GetEnvironmentVariableA("CE_DX12_DRED", buf, sizeof(buf));
    const bool isSet = (n != 0 && n < sizeof(buf));
    Mode mode = ce::dx12_overlay_policy::DecideDredArmMode(isSet ? buf : nullptr, isSet);
    if (mode != Mode::kOff) {
        return mode;
    }
    // Flag-file fallback. An empty file selects page-fault-only (the low-perturbation default
    // for diagnosis); otherwise the file contents pick the mode exactly like the env var.
    char fileBuf[16] = {};
    if (ReadHookDirFlagFile("ce_dx12_dred", fileBuf, sizeof(fileBuf))) {
        if (fileBuf[0] == '\0') {
            return Mode::kPageFaultOnly;
        }
        return ce::dx12_overlay_policy::DecideDredArmMode(fileBuf, true);
    }
    return Mode::kOff;
}

ce::dx12_overlay_policy::DredArmMode ArmMode() {
    static const ce::dx12_overlay_policy::DredArmMode s_mode = ReadArmModeFromEnvAndFile();
    return s_mode;
}

const char* BreadcrumbOpName(D3D12_AUTO_BREADCRUMB_OP op) {
    switch (op) {
        case D3D12_AUTO_BREADCRUMB_OP_SETMARKER:
            return "SETMARKER";
        case D3D12_AUTO_BREADCRUMB_OP_BEGINEVENT:
            return "BEGINEVENT";
        case D3D12_AUTO_BREADCRUMB_OP_ENDEVENT:
            return "ENDEVENT";
        case D3D12_AUTO_BREADCRUMB_OP_DRAWINSTANCED:
            return "DRAWINSTANCED";
        case D3D12_AUTO_BREADCRUMB_OP_DRAWINDEXEDINSTANCED:
            return "DRAWINDEXEDINSTANCED";
        case D3D12_AUTO_BREADCRUMB_OP_EXECUTEINDIRECT:
            return "EXECUTEINDIRECT";
        case D3D12_AUTO_BREADCRUMB_OP_DISPATCH:
            return "DISPATCH";
        case D3D12_AUTO_BREADCRUMB_OP_COPYBUFFERREGION:
            return "COPYBUFFERREGION";
        case D3D12_AUTO_BREADCRUMB_OP_COPYTEXTUREREGION:
            return "COPYTEXTUREREGION";
        case D3D12_AUTO_BREADCRUMB_OP_COPYRESOURCE:
            return "COPYRESOURCE";
        case D3D12_AUTO_BREADCRUMB_OP_COPYTILES:
            return "COPYTILES";
        case D3D12_AUTO_BREADCRUMB_OP_RESOLVESUBRESOURCE:
            return "RESOLVESUBRESOURCE";
        case D3D12_AUTO_BREADCRUMB_OP_CLEARRENDERTARGETVIEW:
            return "CLEARRENDERTARGETVIEW";
        case D3D12_AUTO_BREADCRUMB_OP_CLEARUNORDEREDACCESSVIEW:
            return "CLEARUNORDEREDACCESSVIEW";
        case D3D12_AUTO_BREADCRUMB_OP_CLEARDEPTHSTENCILVIEW:
            return "CLEARDEPTHSTENCILVIEW";
        case D3D12_AUTO_BREADCRUMB_OP_RESOURCEBARRIER:
            return "RESOURCEBARRIER";
        case D3D12_AUTO_BREADCRUMB_OP_EXECUTEBUNDLE:
            return "EXECUTEBUNDLE";
        case D3D12_AUTO_BREADCRUMB_OP_PRESENT:
            return "PRESENT";
        case D3D12_AUTO_BREADCRUMB_OP_RESOLVEQUERYDATA:
            return "RESOLVEQUERYDATA";
        case D3D12_AUTO_BREADCRUMB_OP_BEGINSUBMISSION:
            return "BEGINSUBMISSION";
        case D3D12_AUTO_BREADCRUMB_OP_ENDSUBMISSION:
            return "ENDSUBMISSION";
        case D3D12_AUTO_BREADCRUMB_OP_DECODEFRAME:
            return "DECODEFRAME";
        case D3D12_AUTO_BREADCRUMB_OP_PROCESSFRAMES:
            return "PROCESSFRAMES";
        case D3D12_AUTO_BREADCRUMB_OP_DISPATCHRAYS:
            return "DISPATCHRAYS";
        case D3D12_AUTO_BREADCRUMB_OP_BUILDRAYTRACINGACCELERATIONSTRUCTURE:
            return "BUILDRTAS";
        default:
            return "OP";
    }
}

const char* AllocationTypeName(D3D12_DRED_ALLOCATION_TYPE type) {
    switch (type) {
        case D3D12_DRED_ALLOCATION_TYPE_COMMAND_QUEUE:
            return "COMMAND_QUEUE";
        case D3D12_DRED_ALLOCATION_TYPE_COMMAND_ALLOCATOR:
            return "COMMAND_ALLOCATOR";
        case D3D12_DRED_ALLOCATION_TYPE_PIPELINE_STATE:
            return "PIPELINE_STATE";
        case D3D12_DRED_ALLOCATION_TYPE_COMMAND_LIST:
            return "COMMAND_LIST";
        case D3D12_DRED_ALLOCATION_TYPE_FENCE:
            return "FENCE";
        case D3D12_DRED_ALLOCATION_TYPE_DESCRIPTOR_HEAP:
            return "DESCRIPTOR_HEAP";
        case D3D12_DRED_ALLOCATION_TYPE_HEAP:
            return "HEAP";
        case D3D12_DRED_ALLOCATION_TYPE_RESOURCE:
            return "RESOURCE";
        default:
            return "OTHER";
    }
}

// Best-effort UTF-16 -> UTF-8 for logging context/object names.
void LogWideName(const char* prefix, const WCHAR* wide) {
    if (!wide) {
        return;
    }
    char utf8[256] = {};
    int written = WideCharToMultiByte(CP_UTF8, 0, wide, -1, utf8, sizeof(utf8) - 1, nullptr, nullptr);
    if (written > 0) {
        HookLogImportant("DX12 DRED:    %s%s", prefix, utf8);
    }
}

void LogAllocationNodes(const char* label, const D3D12_DRED_ALLOCATION_NODE1* head) {
    int count = 0;
    for (const D3D12_DRED_ALLOCATION_NODE1* n = head; n && count < 32; n = n->pNext, ++count) {
        const char* name = n->ObjectNameA ? n->ObjectNameA : nullptr;
        if (name) {
            HookLogImportant("DX12 DRED:    [%s] type=%s name='%s' obj=%p", label,
                             AllocationTypeName(n->AllocationType), name, (void*)n->pObject);
        } else if (n->ObjectNameW) {
            HookLogImportant("DX12 DRED:    [%s] type=%s obj=%p name(W):", label, AllocationTypeName(n->AllocationType),
                             (void*)n->pObject);
            LogWideName("name=", n->ObjectNameW);
        } else {
            HookLogImportant("DX12 DRED:    [%s] type=%s name=<unnamed> obj=%p", label,
                             AllocationTypeName(n->AllocationType), (void*)n->pObject);
        }
    }
    if (count == 0) {
        HookLogImportant("DX12 DRED:    [%s] (none)", label);
    }
}

}  // namespace

bool IsEnabled() {
    return ArmMode() != ce::dx12_overlay_policy::DredArmMode::kOff;
}

CE_DRED_KEEP bool ArmBeforeDeviceCreation() {
    const ce::dx12_overlay_policy::DredArmMode mode = ArmMode();
    if (mode == ce::dx12_overlay_policy::DredArmMode::kOff) {
        return false;
    }
    // Full mode adds auto-breadcrumbs (per-Reset kernel GPU allocation, high perturbation);
    // page-fault-only mode arms ONLY page-fault output (no auto-breadcrumbs, low perturbation).
    const bool fullMode = (mode == ce::dx12_overlay_policy::DredArmMode::kFull);

    HMODULE d3d12 = GetModuleHandleW(L"d3d12.dll");
    if (!d3d12) {
        d3d12 = LoadLibraryW(L"d3d12.dll");
    }
    if (!d3d12) {
        return false;
    }

    using PFN_D3D12GetDebugInterface = HRESULT(WINAPI*)(REFIID, void**);
    auto pGetDebugInterface =
        reinterpret_cast<PFN_D3D12GetDebugInterface>(GetProcAddress(d3d12, "D3D12GetDebugInterface"));
    if (!pGetDebugInterface) {
        return false;
    }

    // Prefer Settings1 (adds breadcrumb-context strings); fall back to base.
    ID3D12DeviceRemovedExtendedDataSettings1* dred1 = nullptr;
    HRESULT hr = pGetDebugInterface(__uuidof(ID3D12DeviceRemovedExtendedDataSettings1), (void**)&dred1);
    if (SUCCEEDED(hr) && dred1) {
        if (fullMode) {
            dred1->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
            dred1->SetBreadcrumbContextEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
        }
        dred1->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
        dred1->Release();
    } else {
        ID3D12DeviceRemovedExtendedDataSettings* dred = nullptr;
        hr = pGetDebugInterface(__uuidof(ID3D12DeviceRemovedExtendedDataSettings), (void**)&dred);
        if (FAILED(hr) || !dred) {
            static std::atomic<bool> s_failLog{false};
            if (!s_failLog.exchange(true)) {
                HookLogImportant("DX12 DRED: could not obtain settings interface hr=0x%08X (DRED unavailable)",
                                 (unsigned)hr);
            }
            return false;
        }
        if (fullMode) {
            dred->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
        }
        dred->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
        dred->Release();
    }

    static std::atomic<bool> s_armedLog{false};
    if (!s_armedLog.exchange(true)) {
        HookLogImportant("DX12 DRED: armed %s (forced on) before device creation (mode=%s context1=%d)",
                         fullMode ? "auto-breadcrumbs + page-fault" : "page-fault only",
                         fullMode ? "full" : "page-fault-only", dred1 ? 1 : 0);
    }
    return true;
}

CE_DRED_KEEP void ResetDumpEpoch() {
    s_dumpedThisEpoch.store(false, std::memory_order_release);
}

CE_DRED_KEEP void DumpOnDeviceRemoved(ID3D12Device* device, const char* reason) {
    if (!device || !IsEnabled()) {
        return;
    }
    if (s_dumpedThisEpoch.exchange(true, std::memory_order_acq_rel)) {
        return;  // already dumped for this device-removed epoch
    }

    // If the debug layer is on, flush any validation messages accumulated up to the
    // removal first — they often name the exact misuse behind the hang.
    DrainDebugLayerMessages(device, "device-removed");

    ID3D12DeviceRemovedExtendedData1* dred = nullptr;
    HRESULT hr = device->QueryInterface(__uuidof(ID3D12DeviceRemovedExtendedData1), (void**)&dred);
    if (FAILED(hr) || !dred) {
        HookLogImportant(
            "DX12 DRED: device-removed (%s) but ID3D12DeviceRemovedExtendedData1 unavailable hr=0x%08X "
            "(DRED not armed or unsupported)",
            reason ? reason : "?", (unsigned)hr);
        return;
    }

    HookLogImportant("DX12 DRED: ===== device-removed extended data (%s) =====", reason ? reason : "?");

    D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT1 breadcrumbs = {};
    if (SUCCEEDED(dred->GetAutoBreadcrumbsOutput1(&breadcrumbs))) {
        int node = 0;
        bool anyIncomplete = false;
        for (const D3D12_AUTO_BREADCRUMB_NODE1* n = breadcrumbs.pHeadAutoBreadcrumbNode; n && node < 64;
             n = n->pNext, ++node) {
            const char* listName = n->pCommandListDebugNameA ? n->pCommandListDebugNameA : "<unnamed>";
            const char* queueName = n->pCommandQueueDebugNameA ? n->pCommandQueueDebugNameA : "<unnamed>";
            const UINT lastCompleted = n->pLastBreadcrumbValue ? *n->pLastBreadcrumbValue : 0;
            const bool incomplete = n->pLastBreadcrumbValue && lastCompleted < n->BreadcrumbCount;
            // Skip fully-completed nodes unless none are incomplete (keep log focused on the hang).
            if (!incomplete && n->BreadcrumbCount > 0 && lastCompleted >= n->BreadcrumbCount) {
                continue;
            }
            anyIncomplete = anyIncomplete || incomplete;
            HookLogImportant("DX12 DRED:  node#%d queue='%s' list='%s' completedOps=%u/%u%s", node, queueName, listName,
                             lastCompleted, n->BreadcrumbCount,
                             incomplete ? "  <-- INCOMPLETE (GPU hung in this list)" : "");
            if (n->pCommandHistory && n->BreadcrumbCount > 0) {
                const UINT start = lastCompleted > 3 ? lastCompleted - 3 : 0;
                const UINT end = (lastCompleted + 4 < n->BreadcrumbCount) ? lastCompleted + 4 : n->BreadcrumbCount;
                for (UINT i = start; i < end; ++i) {
                    HookLogImportant("DX12 DRED:    op[%u]=%s%s", i, BreadcrumbOpName(n->pCommandHistory[i]),
                                     (i == lastCompleted) ? "  <== last completed" : "");
                }
            }
            for (UINT c = 0; c < n->BreadcrumbContextsCount && c < 16; ++c) {
                const D3D12_DRED_BREADCRUMB_CONTEXT& ctx = n->pBreadcrumbContexts[c];
                char prefix[48] = {};
                _snprintf_s(prefix, sizeof(prefix), _TRUNCATE, "ctx@op%u=", ctx.BreadcrumbIndex);
                LogWideName(prefix, ctx.pContextString);
            }
        }
        if (node == 0) {
            HookLogImportant("DX12 DRED:  (no breadcrumb nodes)");
        } else if (!anyIncomplete) {
            HookLogImportant("DX12 DRED:  (all breadcrumb nodes completed — fault may be page-fault only)");
        }
    } else {
        HookLogImportant("DX12 DRED:  GetAutoBreadcrumbsOutput1 failed");
    }

    D3D12_DRED_PAGE_FAULT_OUTPUT1 pageFault = {};
    if (SUCCEEDED(dred->GetPageFaultAllocationOutput1(&pageFault))) {
        HookLogImportant("DX12 DRED:  pageFaultVA=0x%llX", (unsigned long long)pageFault.PageFaultVA);
        if (pageFault.PageFaultVA != 0 || pageFault.pHeadExistingAllocationNode ||
            pageFault.pHeadRecentFreedAllocationNode) {
            LogAllocationNodes("existing", pageFault.pHeadExistingAllocationNode);
            // Recently-freed allocations matching the fault VA are the smoking gun
            // for stale-backbuffer access across the iflip<->composited transition.
            LogAllocationNodes("recently-freed", pageFault.pHeadRecentFreedAllocationNode);
        } else {
            HookLogImportant("DX12 DRED:  (no page-fault VA — likely a pure hang, not an invalid access)");
        }
    } else {
        HookLogImportant("DX12 DRED:  GetPageFaultAllocationOutput1 failed");
    }

    HookLogImportant("DX12 DRED: ===== end device-removed extended data =====");
    dred->Release();
}

CE_DRED_KEEP int DebugLayerLevel() {
    static const int s_level = []() -> int {
        // 1) Environment variable CE_DX12_DEBUG_LAYER (inherited by the target at launch).
        char buf[16] = {};
        DWORD n = GetEnvironmentVariableA("CE_DX12_DEBUG_LAYER", buf, sizeof(buf));
        if (n > 0 && n < sizeof(buf)) {
            if (buf[0] == '2') {
                return 2;
            }
            if (buf[0] == '1' || _stricmp(buf, "on") == 0 || _stricmp(buf, "true") == 0) {
                return 1;
            }
        }
        // 2) Flag file next to the hook DLL — robust against env-inheritance with the
        // inject model (the user just creates the file; no launch-context juggling).
        // File "ce_dx12_debug_layer" in installed\captureengine\: empty/"1" -> level 1,
        // first char "2" -> level 2.
        HMODULE self = nullptr;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCSTR>(&IsEnabled), &self) &&
            self) {
            char path[MAX_PATH] = {};
            DWORD len = GetModuleFileNameA(self, path, MAX_PATH);
            if (len > 0 && len < MAX_PATH) {
                for (DWORD i = len; i > 0; --i) {
                    if (path[i - 1] == '\\' || path[i - 1] == '/') {
                        path[i] = '\0';
                        break;
                    }
                }
                char flagPath[MAX_PATH] = {};
                _snprintf_s(flagPath, sizeof(flagPath), _TRUNCATE, "%sce_dx12_debug_layer", path);
                HANDLE h = CreateFileA(flagPath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                       OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (h != INVALID_HANDLE_VALUE) {
                    char c = '1';
                    DWORD rd = 0;
                    ReadFile(h, &c, 1, &rd, nullptr);
                    CloseHandle(h);
                    return (c == '2') ? 2 : 1;
                }
            }
        }
        return 0;
    }();
    return s_level;
}

CE_DRED_KEEP void ArmDebugLayerBeforeDeviceCreation() {
    const int level = DebugLayerLevel();
    if (level < 1) {
        return;
    }
    HMODULE d3d12 = GetModuleHandleW(L"d3d12.dll");
    if (!d3d12) {
        d3d12 = LoadLibraryW(L"d3d12.dll");
    }
    if (!d3d12) {
        return;
    }
    using PFN_D3D12GetDebugInterface = HRESULT(WINAPI*)(REFIID, void**);
    auto pGetDebugInterface =
        reinterpret_cast<PFN_D3D12GetDebugInterface>(GetProcAddress(d3d12, "D3D12GetDebugInterface"));
    if (!pGetDebugInterface) {
        return;
    }
    ID3D12Debug* debug = nullptr;
    HRESULT hr = pGetDebugInterface(__uuidof(ID3D12Debug), (void**)&debug);
    if (FAILED(hr) || !debug) {
        static std::atomic<bool> s_failLog{false};
        if (!s_failLog.exchange(true)) {
            HookLogImportant("DX12 DBGLAYER: ID3D12Debug unavailable hr=0x%08X (Graphics Tools not installed?)",
                             (unsigned)hr);
        }
        return;
    }
    debug->EnableDebugLayer();
    bool gpuValidation = false;
    if (level >= 2) {
        ID3D12Debug1* debug1 = nullptr;
        if (SUCCEEDED(debug->QueryInterface(__uuidof(ID3D12Debug1), (void**)&debug1)) && debug1) {
            debug1->SetEnableGPUBasedValidation(TRUE);
            debug1->Release();
            gpuValidation = true;
        }
    }
    debug->Release();
    static std::atomic<bool> s_armedLog{false};
    if (!s_armedLog.exchange(true)) {
        HookLogImportant("DX12 DBGLAYER: enabled D3D12 debug layer (level=%d gpuValidation=%d) before device creation",
                         level, gpuValidation ? 1 : 0);
    }
}

CE_DRED_KEEP void DrainDebugLayerMessages(ID3D12Device* device, const char* context) {
    if (DebugLayerLevel() < 1 || !device) {
        return;
    }
    ID3D12InfoQueue* infoQueue = nullptr;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&infoQueue))) || !infoQueue) {
        return;
    }
    const UINT64 count = infoQueue->GetNumStoredMessages();
    for (UINT64 i = 0; i < count; ++i) {
        SIZE_T len = 0;
        if (FAILED(infoQueue->GetMessage(i, nullptr, &len)) || len == 0) {
            continue;
        }
        std::vector<char> storage(len);
        D3D12_MESSAGE* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
        if (SUCCEEDED(infoQueue->GetMessage(i, message, &len)) && message->pDescription) {
            HookLogImportant("DX12 DBGLAYER [%s] sev=%d cat=%d id=%d: %s", context ? context : "?",
                             (int)message->Severity, (int)message->Category, (int)message->ID, message->pDescription);
        }
    }
    if (count > 0) {
        infoQueue->ClearStoredMessages();
    }
    infoQueue->Release();
}

}  // namespace ce::dx12_dred
