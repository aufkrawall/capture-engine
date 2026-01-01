#include <windows.h>
#include <stdio.h>

int main() {
    printf("TestLoader32: Starting...\n");
    printf("TestLoader32: Attempting to LoadLibrary('capture_hook_x86.dll')...\n");

    HMODULE hHook = LoadLibraryA("capture_hook_x86.dll");
    if (!hHook) {
        DWORD err = GetLastError();
        printf("TestLoader32: FAILED to load DLL! Error: %lu\n", err);
        return 1;
    }

    printf("TestLoader32: DLL loaded successfully! Handle: %p\n", hHook);
    printf("TestLoader32: Waiting 5 seconds...\n");
    Sleep(5000);

    printf("TestLoader32: Freeing library...\n");
    FreeLibrary(hHook);
    printf("TestLoader32: Done.\n");
    return 0;
}
