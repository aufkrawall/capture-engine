#include "dx8_hook_internal.h"

void DX8Hook_OnModuleLoaded() {
    TryInstallDirect3DCreate8Hook(GetModuleHandleA("d3d8.dll"));
}

static int64_t g_LastSleepUs = 0;
