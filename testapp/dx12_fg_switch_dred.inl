// Included by dx12_fg_switch_test.cpp; shares that file's static DX12/FG state.
// DRED (Device Removed Extended Data) diagnostics for GPU device-hung/TDR root-causing.

// Force-on breadcrumbs + page-fault BEFORE any device is created so a device-removed/hung (TDR)
// dumps the last GPU op per command queue. Opt-in (--dred) because auto-breadcrumbs add per-op
// overhead. Used to pin down which command list hangs the GPU in suspended DLSS FG.
static void EnableDredIfRequested() {
    if (!g_EnableDred) {
        return;
    }
    Microsoft::WRL::ComPtr<ID3D12DeviceRemovedExtendedDataSettings1> dredSettings;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dredSettings))) && dredSettings) {
        dredSettings->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
        dredSettings->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
        dredSettings->SetBreadcrumbContextEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
        testapp::Log("[FG-DIAG] DRED auto-breadcrumbs + page-fault FORCED_ON\n");
    } else {
        testapp::Log("[FG-DIAG] WARN: DRED settings interface unavailable\n");
    }
}

static const char* DredOpName(D3D12_AUTO_BREADCRUMB_OP op) {
    switch (op) {
        case D3D12_AUTO_BREADCRUMB_OP_SETMARKER: return "SETMARKER";
        case D3D12_AUTO_BREADCRUMB_OP_BEGINEVENT: return "BEGINEVENT";
        case D3D12_AUTO_BREADCRUMB_OP_ENDEVENT: return "ENDEVENT";
        case D3D12_AUTO_BREADCRUMB_OP_DRAWINSTANCED: return "DRAWINSTANCED";
        case D3D12_AUTO_BREADCRUMB_OP_DRAWINDEXEDINSTANCED: return "DRAWINDEXEDINSTANCED";
        case D3D12_AUTO_BREADCRUMB_OP_EXECUTEINDIRECT: return "EXECUTEINDIRECT";
        case D3D12_AUTO_BREADCRUMB_OP_DISPATCH: return "DISPATCH";
        case D3D12_AUTO_BREADCRUMB_OP_COPYBUFFERREGION: return "COPYBUFFERREGION";
        case D3D12_AUTO_BREADCRUMB_OP_COPYTEXTUREREGION: return "COPYTEXTUREREGION";
        case D3D12_AUTO_BREADCRUMB_OP_COPYRESOURCE: return "COPYRESOURCE";
        case D3D12_AUTO_BREADCRUMB_OP_RESOLVESUBRESOURCE: return "RESOLVESUBRESOURCE";
        case D3D12_AUTO_BREADCRUMB_OP_CLEARRENDERTARGETVIEW: return "CLEARRENDERTARGETVIEW";
        case D3D12_AUTO_BREADCRUMB_OP_CLEARDEPTHSTENCILVIEW: return "CLEARDEPTHSTENCILVIEW";
        case D3D12_AUTO_BREADCRUMB_OP_RESOURCEBARRIER: return "RESOURCEBARRIER";
        case D3D12_AUTO_BREADCRUMB_OP_EXECUTEBUNDLE: return "EXECUTEBUNDLE";
        case D3D12_AUTO_BREADCRUMB_OP_PRESENT: return "PRESENT";
        case D3D12_AUTO_BREADCRUMB_OP_DISPATCHRAYS: return "DISPATCHRAYS";
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
    Microsoft::WRL::ComPtr<ID3D12DeviceRemovedExtendedData1> dred;
    if (FAILED(g_Device->QueryInterface(IID_PPV_ARGS(&dred))) || !dred) {
        testapp::Log("[FG-DIAG] DRED data interface unavailable (run with --dred)\n");
        testapp::LogFlush();
        return;
    }
    D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT1 breadcrumbs = {};
    if (SUCCEEDED(dred->GetAutoBreadcrumbsOutput1(&breadcrumbs))) {
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
                    testapp::Log("[FG-DIAG]   op[%u]=%s%s\n", i, DredOpName(node->pCommandHistory[i]),
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
    if (SUCCEEDED(dred->GetPageFaultAllocationOutput(&pageFault))) {
        testapp::Log("[FG-DIAG] DRED pageFaultVA=0x%llx\n", static_cast<unsigned long long>(pageFault.PageFaultVA));
    }
    testapp::LogFlush();
}
