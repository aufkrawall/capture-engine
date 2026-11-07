// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall
#pragma once

#include <windows.h>
#include <dxgi1_4.h>

#include "common/platform/raii_helpers.h"

// The LUID of the WARP adapter, which every Windows test host has, so GPU-object tests do
// not depend on the machine's hardware adapter.
inline bool WarpAdapterLuid(LUID& luid) {
    ce::ComGuard<IDXGIFactory4> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(factory.put()))))
        return false;
    ce::ComGuard<IDXGIAdapter> adapter;
    if (FAILED(factory->EnumWarpAdapter(IID_PPV_ARGS(adapter.put()))))
        return false;
    DXGI_ADAPTER_DESC desc{};
    if (FAILED(adapter->GetDesc(&desc)))
        return false;
    luid = desc.AdapterLuid;
    return true;
}
