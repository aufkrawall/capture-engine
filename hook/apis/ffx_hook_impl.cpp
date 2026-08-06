#include "ffx_hook_internal.h"


bool IsCommittedReadableCodeAddress(void* address) {


    if (!address) {
        return false;
    }

    MEMORY_BASIC_INFORMATION mbi = {};
    if (!VirtualQuery(address, &mbi, sizeof(mbi))) {
        return false;
    }
    if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) {
        return false;
    }
    return true;

}

bool IsFFXDynamicHookOwnerModule(const char* moduleBaseName,  HMODULE module) {


    if (moduleBaseName && ce::overlay_compat::IsFFXFrameGenerationModulePath(moduleBaseName)) {
        return true;
    }

    char modulePath[MAX_PATH] = {};
    if (module && GetModuleFileNameA(module, modulePath, sizeof(modulePath))) {
        return ce::overlay_compat::IsFFXFrameGenerationModulePath(modulePath);
    }

    return false;

}

void ffx_hook_RegisterDynamicHooksOnce() {


    std::call_once(ffx_hook_g_DynamicHookRegistrationOnce, [] {
        IATHook::RegisterDynamicHookFiltered("ffxCreateContext", reinterpret_cast<void*>(Hooked_ffxCreateContext),
                                             reinterpret_cast<void**>(&ffx_hook_g_Original_ffxCreateContext),
                                             IsFFXDynamicHookOwnerModule);
        IATHook::RegisterDynamicHookFiltered("ffxDestroyContext", reinterpret_cast<void*>(Hooked_ffxDestroyContext),
                                             reinterpret_cast<void**>(&ffx_hook_g_Original_ffxDestroyContext),
                                             IsFFXDynamicHookOwnerModule);
        IATHook::RegisterDynamicHookFiltered("ffxConfigure", reinterpret_cast<void*>(Hooked_ffxConfigure),
                                             reinterpret_cast<void**>(&ffx_hook_g_Original_ffxConfigure),
                                             IsFFXDynamicHookOwnerModule);
        HookLogImportant("FFX Hook: Registered module-filtered dynamic hooks for FFX exports");
    });

}
