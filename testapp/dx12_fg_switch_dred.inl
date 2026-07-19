// Included by dx12_fg_switch_test.cpp; shares that file's static DX12/FG state.
// DRED (Device Removed Extended Data) diagnostics for GPU device-hung/TDR root-causing.

// Ubuntu's MinGW 11 headers expose only the base DRED settings interface. Capability-gate every
// newer declaration so the switch app retains full Settings1/Data1 diagnostics with current
// Windows headers, base arming/data with intermediate headers, and an explicit diagnostic when
// the compile-time SDK cannot describe the data interface at all.
#ifndef CE_TESTAPP_HAS_D3D12_DRED_SETTINGS
#if defined(__ID3D12DeviceRemovedExtendedDataSettings_INTERFACE_DEFINED__)
#define CE_TESTAPP_HAS_D3D12_DRED_SETTINGS 1
#else
#define CE_TESTAPP_HAS_D3D12_DRED_SETTINGS 0
#endif
#endif
#ifndef CE_TESTAPP_HAS_D3D12_DRED_SETTINGS1
#if defined(__ID3D12DeviceRemovedExtendedDataSettings1_INTERFACE_DEFINED__)
#define CE_TESTAPP_HAS_D3D12_DRED_SETTINGS1 1
#else
#define CE_TESTAPP_HAS_D3D12_DRED_SETTINGS1 0
#endif
#endif
#ifndef CE_TESTAPP_HAS_D3D12_DRED_DATA
#if defined(__ID3D12DeviceRemovedExtendedData_INTERFACE_DEFINED__)
#define CE_TESTAPP_HAS_D3D12_DRED_DATA 1
#else
#define CE_TESTAPP_HAS_D3D12_DRED_DATA 0
#endif
#endif
#ifndef CE_TESTAPP_HAS_D3D12_DRED_DATA1
#if defined(__ID3D12DeviceRemovedExtendedData1_INTERFACE_DEFINED__)
#define CE_TESTAPP_HAS_D3D12_DRED_DATA1 1
#else
#define CE_TESTAPP_HAS_D3D12_DRED_DATA1 0
#endif
#endif
#if !CE_TESTAPP_HAS_D3D12_DRED_SETTINGS
#undef CE_TESTAPP_HAS_D3D12_DRED_SETTINGS1
#define CE_TESTAPP_HAS_D3D12_DRED_SETTINGS1 0
#endif
#if !CE_TESTAPP_HAS_D3D12_DRED_DATA
#undef CE_TESTAPP_HAS_D3D12_DRED_DATA1
#define CE_TESTAPP_HAS_D3D12_DRED_DATA1 0
#endif

// Force-on breadcrumbs + page-fault BEFORE any device is created so a device-removed/hung (TDR)
// dumps the last GPU op per command queue. Opt-in (--dred) because auto-breadcrumbs add per-op
// overhead. Used to pin down which command list hangs the GPU in suspended DLSS FG.
static void EnableDredIfRequested() {
    if (!g_EnableDred) {
        return;
    }
#if CE_TESTAPP_HAS_D3D12_DRED_SETTINGS1
    Microsoft::WRL::ComPtr<ID3D12DeviceRemovedExtendedDataSettings1> dredSettings;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dredSettings))) && dredSettings) {
        dredSettings->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
        dredSettings->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
        dredSettings->SetBreadcrumbContextEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
        testapp::Log("[FG-DIAG] DRED auto-breadcrumbs + page-fault FORCED_ON\n");
    } else {
        testapp::Log("[FG-DIAG] WARN: DRED settings interface unavailable\n");
    }
#elif CE_TESTAPP_HAS_D3D12_DRED_SETTINGS
    Microsoft::WRL::ComPtr<ID3D12DeviceRemovedExtendedDataSettings> dredSettings;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dredSettings))) && dredSettings) {
        dredSettings->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
        dredSettings->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
        testapp::Log("[FG-DIAG] DRED auto-breadcrumbs + page-fault FORCED_ON (base settings)\n");
    } else {
        testapp::Log("[FG-DIAG] WARN: DRED base settings interface unavailable\n");
    }
#else
    testapp::Log("[FG-DIAG] WARN: compiler headers expose no DRED settings interface\n");
#endif
}

static const char* DredOpName(UINT op) {
    // DRED operation values are stable ABI constants. Numeric cases avoid requiring newer enum
    // members from the compile-time Windows SDK while retaining useful breadcrumb names.
    switch (op) {
        case 0x0: return "SETMARKER";
        case 0x1: return "BEGINEVENT";
        case 0x2: return "ENDEVENT";
        case 0x3: return "DRAWINSTANCED";
        case 0x4: return "DRAWINDEXEDINSTANCED";
        case 0x5: return "EXECUTEINDIRECT";
        case 0x6: return "DISPATCH";
        case 0x7: return "COPYBUFFERREGION";
        case 0x8: return "COPYTEXTUREREGION";
        case 0x9: return "COPYRESOURCE";
        case 0xB: return "RESOLVESUBRESOURCE";
        case 0xC: return "CLEARRENDERTARGETVIEW";
        case 0xE: return "CLEARDEPTHSTENCILVIEW";
        case 0xF: return "RESOURCEBARRIER";
        case 0x10: return "EXECUTEBUNDLE";
        case 0x11: return "PRESENT";
        case 0x22: return "DISPATCHRAYS";
        default: return "OTHER";
    }
}

static bool IsDeviceRemovedHr(HRESULT hr) {
    return hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_HUNG || hr == DXGI_ERROR_DEVICE_RESET ||
           hr == DXGI_ERROR_DRIVER_INTERNAL_ERROR;
}

// Dumps once per process: callable from every device-removed detection site (Present hr, stalled
// fence waits) without duplicating the breadcrumb walk in the log.
static void DumpDredOnDeviceRemoved(const char* reason) {
    if (!g_Device) {
        return;
    }
    static bool s_dumped = false;
    if (s_dumped) {
        return;
    }
    s_dumped = true;
    const HRESULT removedReason = g_Device->GetDeviceRemovedReason();
    testapp::Log("[FG-DIAG] ===== DEVICE REMOVED (%s) reason=0x%08lx =====\n", reason ? reason : "?",
                 static_cast<unsigned long>(removedReason));
#if CE_TESTAPP_HAS_D3D12_DRED_DATA1
    Microsoft::WRL::ComPtr<ID3D12DeviceRemovedExtendedData1> dred1;
    if (FAILED(g_Device->QueryInterface(IID_PPV_ARGS(&dred1))) || !dred1) {
        testapp::Log("[FG-DIAG] DRED data interface unavailable (run with --dred)\n");
        testapp::LogFlush();
        return;
    }
    D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT1 breadcrumbs = {};
    if (SUCCEEDED(dred1->GetAutoBreadcrumbsOutput1(&breadcrumbs))) {
        const D3D12_AUTO_BREADCRUMB_NODE1* node = breadcrumbs.pHeadAutoBreadcrumbNode;
        int nodeIndex = 0;
        while (node && nodeIndex < 24) {
            const UINT lastCompleted = node->pLastBreadcrumbValue ? *node->pLastBreadcrumbValue : 0;
            const UINT total = node->BreadcrumbCount;
            const bool finished = (lastCompleted == total);
            testapp::Log("[FG-DIAG] DRED node[%d] queue='%S' list='%S' completed=%u/%u %s\n", nodeIndex,
                         node->pCommandQueueDebugNameW ? node->pCommandQueueDebugNameW : L"?",
                         node->pCommandListDebugNameW ? node->pCommandListDebugNameW : L"?", lastCompleted, total,
                         finished ? "(finished)" : "<-- INCOMPLETE (hung here)");
            if (!finished && node->pCommandHistory) {
                const UINT from = lastCompleted > 2 ? lastCompleted - 2 : 0;
                const UINT to = (lastCompleted + 3 < total) ? lastCompleted + 3 : total;
                for (UINT i = from; i < to; ++i) {
                    testapp::Log("[FG-DIAG]   op[%u]=%s%s\n", i,
                                 DredOpName(static_cast<UINT>(node->pCommandHistory[i])),
                                 i == lastCompleted ? "  <== first not-completed op" : "");
                }
            }
            node = node->pNext;
            ++nodeIndex;
        }
        if (nodeIndex == 0) {
            testapp::Log("[FG-DIAG] DRED: no breadcrumb nodes\n");
        }
    } else {
        testapp::Log("[FG-DIAG] DRED breadcrumbs unavailable\n");
    }
    D3D12_DRED_PAGE_FAULT_OUTPUT pageFault = {};
    if (SUCCEEDED(dred1->GetPageFaultAllocationOutput(&pageFault))) {
        testapp::Log("[FG-DIAG] DRED pageFaultVA=0x%llx\n", static_cast<unsigned long long>(pageFault.PageFaultVA));
    }
#elif CE_TESTAPP_HAS_D3D12_DRED_DATA
    Microsoft::WRL::ComPtr<ID3D12DeviceRemovedExtendedData> dred;
    if (FAILED(g_Device->QueryInterface(IID_PPV_ARGS(&dred))) || !dred) {
        testapp::Log("[FG-DIAG] DRED base data interface unavailable (run with --dred)\n");
        testapp::LogFlush();
        return;
    }
    D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT breadcrumbs = {};
    if (SUCCEEDED(dred->GetAutoBreadcrumbsOutput(&breadcrumbs))) {
        const D3D12_AUTO_BREADCRUMB_NODE* node = breadcrumbs.pHeadAutoBreadcrumbNode;
        int nodeIndex = 0;
        while (node && nodeIndex < 24) {
            const UINT lastCompleted = node->pLastBreadcrumbValue ? *node->pLastBreadcrumbValue : 0;
            const UINT total = node->BreadcrumbCount;
            const bool finished = (lastCompleted == total);
            testapp::Log("[FG-DIAG] DRED node[%d] queue='%S' list='%S' completed=%u/%u %s\n", nodeIndex,
                         node->pCommandQueueDebugNameW ? node->pCommandQueueDebugNameW : L"?",
                         node->pCommandListDebugNameW ? node->pCommandListDebugNameW : L"?", lastCompleted, total,
                         finished ? "(finished)" : "<-- INCOMPLETE (hung here)");
            if (!finished && node->pCommandHistory) {
                const UINT from = lastCompleted > 2 ? lastCompleted - 2 : 0;
                const UINT to = (lastCompleted + 3 < total) ? lastCompleted + 3 : total;
                for (UINT i = from; i < to; ++i) {
                    testapp::Log("[FG-DIAG]   op[%u]=%s%s\n", i,
                                 DredOpName(static_cast<UINT>(node->pCommandHistory[i])),
                                 i == lastCompleted ? "  <== first not-completed op" : "");
                }
            }
            node = node->pNext;
            ++nodeIndex;
        }
        if (nodeIndex == 0) {
            testapp::Log("[FG-DIAG] DRED: no breadcrumb nodes\n");
        }
    } else {
        testapp::Log("[FG-DIAG] DRED base breadcrumbs unavailable\n");
    }
    D3D12_DRED_PAGE_FAULT_OUTPUT pageFault = {};
    if (SUCCEEDED(dred->GetPageFaultAllocationOutput(&pageFault))) {
        testapp::Log("[FG-DIAG] DRED pageFaultVA=0x%llx\n", static_cast<unsigned long long>(pageFault.PageFaultVA));
    }
#else
    testapp::Log("[FG-DIAG] Compiler headers expose no DRED data interface\n");
#endif
    testapp::LogFlush();
}
