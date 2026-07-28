            // Actually, IPC connects in DX12Hook::Init or similar.
            // NVNGX Install happens early.
            // Let's rely on the fact that GetPresetChar now has a fallback!
            // Wait, GetPresetChar is only called by CreateFeature.
            // If CreateFeature is bypassed, GetPresetChar updates nothing.
            // So I DO need to update shared state here if possible.
            // If IPC fails, we can't update shared state anyway (it doesn't exist).
        }
    }

    // Phase 1: Register dynamic hooks for GetProcAddress interception UNCONDITIONALLY.
    // This must happen before the DLL-presence check so that if the game loads
    // nvngx.dll after this point and immediately calls GetProcAddress, we intercept it.
    // RegisterDynamicHook is idempotent — safe to call on every retry.
    auto RegisterDynamic = [](const char* name, LPVOID pHook, LPVOID* ppOrig) {
        IATHook::RegisterDynamicHook(name, pHook, ppOrig);
    };

    RegisterDynamic("NVSDK_NGX_D3D11_GetParameters", (LPVOID)&Hooked_GetParams_D3D11, (LPVOID*)&oGetParameters_D3D11);
    RegisterDynamic("NVSDK_NGX_D3D11_AllocateParameters", (LPVOID)&Hooked_AllocParams_D3D11,
                    (LPVOID*)&oAllocateParameters_D3D11);
    RegisterDynamic("NVSDK_NGX_D3D11_GetCapabilityParameters", (LPVOID)&Hooked_GetCaps_D3D11,
                    (LPVOID*)&oGetCapabilityParameters_D3D11);
    RegisterDynamic("NVSDK_NGX_D3D12_GetParameters", (LPVOID)&Hooked_GetParams_D3D12, (LPVOID*)&oGetParameters_D3D12);
    RegisterDynamic("NVSDK_NGX_D3D12_AllocateParameters", (LPVOID)&Hooked_AllocParams_D3D12,
                    (LPVOID*)&oAllocateParameters_D3D12);
    RegisterDynamic("NVSDK_NGX_D3D12_GetCapabilityParameters", (LPVOID)&Hooked_GetCaps_D3D12,
                    (LPVOID*)&oGetCapabilityParameters_D3D12);
    RegisterDynamic("NVSDK_NGX_VULKAN_GetParameters", (LPVOID)&Hooked_GetParams_VULKAN,
                    (LPVOID*)&oGetParameters_VULKAN);
    RegisterDynamic("NVSDK_NGX_VULKAN_AllocateParameters", (LPVOID)&Hooked_AllocParams_VULKAN,
                    (LPVOID*)&oAllocateParameters_VULKAN);
    RegisterDynamic("NVSDK_NGX_VULKAN_GetCapabilityParameters", (LPVOID)&Hooked_GetCaps_VULKAN,
                    (LPVOID*)&oGetCapabilityParameters_VULKAN);
    RegisterDynamic("NVSDK_NGX_D3D11_GetFeatureRequirements", (LPVOID)&Hooked_GetFeatureRequirements_D3D11,
                    (LPVOID*)&oGetFeatureRequirements_D3D11);
    RegisterDynamic("NVSDK_NGX_D3D12_GetFeatureRequirements", (LPVOID)&Hooked_GetFeatureRequirements_D3D12,
                    (LPVOID*)&oGetFeatureRequirements_D3D12);
    RegisterDynamic("NVSDK_NGX_VULKAN_GetFeatureRequirements", (LPVOID)&Hooked_GetFeatureRequirements_VULKAN,
                    (LPVOID*)&oGetFeatureRequirements_VULKAN);
    RegisterDynamic("NVSDK_NGX_D3D11_CreateFeature", (LPVOID)&Hooked_CreateFeature_D3D11,
                    (LPVOID*)&oCreateFeature_D3D11);
    RegisterDynamic("NVSDK_NGX_D3D12_CreateFeature", (LPVOID)&Hooked_CreateFeature_D3D12,
                    (LPVOID*)&oCreateFeature_D3D12);
    RegisterDynamic("NVSDK_NGX_VULKAN_CreateFeature", (LPVOID)&Hooked_CreateFeature_VULKAN,
                    (LPVOID*)&oCreateFeature_VULKAN);

    if (!s_DynamicHookRegisterLogged.exchange(true)) {
        HookLogImportant("NVNGX: Dynamic hooks registered for GetProcAddress interception");
    }

    // Phase 2: Patch IAT entries in already-loaded modules (requires DLL to be present).
    const char* foundDllName = "nvngx.dll";
    HMODULE hNGX = GetModuleHandleA("nvngx.dll");
    if (!hNGX) {
        hNGX = GetModuleHandleA("_nvngx.dll");
        if (hNGX)
            foundDllName = "_nvngx.dll";
    }
    if (!hNGX) {
        hNGX = GetModuleHandleA("sl.dlss.dll");
        if (hNGX)
            foundDllName = "sl.dlss.dll";
    }

    if (!hNGX) {
        if (!s_DeferredLoadLogged.exchange(true)) {
            HookLogImportant("NVNGX: DLL not yet loaded; IAT patches deferred (dynamic hooks active, will retry)");
        }
        m_Installed = false;
        return;
    }

    s_DeferredLoadLogged.store(false, std::memory_order_relaxed);
    HookLogImportant("NVNGX: Found '%s' at %p, installing IAT patches", foundDllName, (void*)hNGX);

    auto PatchIAT = [](const char* name, LPVOID pHook, LPVOID* ppOrig) {
        for (const char* module : {"_nvngx.dll", "nvngx.dll", "sl.dlss.dll"}) {
            void* captured = nullptr;
            if (IATHook::PatchIATAllModules(module, name, pHook, &captured) && captured && ppOrig && !*ppOrig)
                *ppOrig = captured;
        }
    };

    PatchIAT("NVSDK_NGX_D3D11_GetParameters", (LPVOID)&Hooked_GetParams_D3D11, (LPVOID*)&oGetParameters_D3D11);
    PatchIAT("NVSDK_NGX_D3D11_AllocateParameters", (LPVOID)&Hooked_AllocParams_D3D11,
             (LPVOID*)&oAllocateParameters_D3D11);
    PatchIAT("NVSDK_NGX_D3D11_GetCapabilityParameters", (LPVOID)&Hooked_GetCaps_D3D11,
             (LPVOID*)&oGetCapabilityParameters_D3D11);
    PatchIAT("NVSDK_NGX_D3D12_GetParameters", (LPVOID)&Hooked_GetParams_D3D12, (LPVOID*)&oGetParameters_D3D12);
    PatchIAT("NVSDK_NGX_D3D12_AllocateParameters", (LPVOID)&Hooked_AllocParams_D3D12,
             (LPVOID*)&oAllocateParameters_D3D12);
    PatchIAT("NVSDK_NGX_D3D12_GetCapabilityParameters", (LPVOID)&Hooked_GetCaps_D3D12,
             (LPVOID*)&oGetCapabilityParameters_D3D12);
    PatchIAT("NVSDK_NGX_VULKAN_GetParameters", (LPVOID)&Hooked_GetParams_VULKAN, (LPVOID*)&oGetParameters_VULKAN);
    PatchIAT("NVSDK_NGX_VULKAN_AllocateParameters", (LPVOID)&Hooked_AllocParams_VULKAN,
             (LPVOID*)&oAllocateParameters_VULKAN);
    PatchIAT("NVSDK_NGX_VULKAN_GetCapabilityParameters", (LPVOID)&Hooked_GetCaps_VULKAN,
             (LPVOID*)&oGetCapabilityParameters_VULKAN);
    PatchIAT("NVSDK_NGX_D3D11_GetFeatureRequirements", (LPVOID)&Hooked_GetFeatureRequirements_D3D11,
             (LPVOID*)&oGetFeatureRequirements_D3D11);
    PatchIAT("NVSDK_NGX_D3D12_GetFeatureRequirements", (LPVOID)&Hooked_GetFeatureRequirements_D3D12,
             (LPVOID*)&oGetFeatureRequirements_D3D12);
    PatchIAT("NVSDK_NGX_VULKAN_GetFeatureRequirements", (LPVOID)&Hooked_GetFeatureRequirements_VULKAN,
             (LPVOID*)&oGetFeatureRequirements_VULKAN);
    PatchIAT("NVSDK_NGX_D3D11_CreateFeature", (LPVOID)&Hooked_CreateFeature_D3D11, (LPVOID*)&oCreateFeature_D3D11);
    PatchIAT("NVSDK_NGX_D3D12_CreateFeature", (LPVOID)&Hooked_CreateFeature_D3D12, (LPVOID*)&oCreateFeature_D3D12);
    PatchIAT("NVSDK_NGX_VULKAN_CreateFeature", (LPVOID)&Hooked_CreateFeature_VULKAN, (LPVOID*)&oCreateFeature_VULKAN);

    HookLogImportant("NVNGX: IAT patches installed");
}

void NVNGXHook::Uninstall() {
    if (!m_Installed.exchange(false))
        return;
    // IAT hooks are permanent until process exit usually
}
