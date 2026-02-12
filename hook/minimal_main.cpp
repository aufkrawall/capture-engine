/**
 * MINIMAL TEST DLL - No C++ runtime, no imports, no exports
 * Tests if the mere PRESENCE of our DLL causes the UE5 crash
 */

#include <windows.h>

// Pure C DllMain - no C++ runtime initialization
BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD ul_reason_for_call,
                    LPVOID lpReserved) {
  if (ul_reason_for_call == DLL_PROCESS_ATTACH) {
    // Disable thread library calls to minimize impact
    DisableThreadLibraryCalls(hinstDLL);

    // Check if D3D12 is present (using only kernel32 - no imports from
    // d3d12.dll) Use GetModuleHandleA which is in kernel32
    HMODULE hD3D12 = GetModuleHandleA("d3d12.dll");
    if (hD3D12 != NULL) {
      // D3D12 detected - return immediately without doing ANYTHING
      // No logging, no memory allocation, nothing
      return TRUE;
    }

    // Not D3D12 - also return TRUE (we're dormant in this test)
    return TRUE;
  }

  return TRUE;
}
