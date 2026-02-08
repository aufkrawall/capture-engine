/**
 * MINIMAL TEST DLL - Pure C, no C++ runtime, only kernel32 imports
 * Tests if the mere PRESENCE of our DLL causes the UE5 crash
 */

#include <windows.h>

// Declare DisableThreadLibraryCalls manually to avoid headers
typedef int(WINAPI* DisableThreadLibraryCalls_t)(HMODULE hLibModule);

// Simple entry point - no C++ runtime
BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD ul_reason_for_call, LPVOID lpReserved)
{
    if (ul_reason_for_call == 1) {  // DLL_PROCESS_ATTACH
        // Don't even call DisableThreadLibraryCalls - do absolutely nothing

        // Check for d3d12.dll - use PEB traversal to avoid imports
        // For now, just return TRUE immediately
        return TRUE;
    }

    return TRUE;
}
