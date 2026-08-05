#include "media_main_internal.h"

BOOL WINAPI MediaConsoleHandler(DWORD ctrlType) {
    // Handle all console events including Windows shutdown/logoff
    LogInfo("[Media] Console event %lu received, shutting down...", ctrlType);
    media_main_g_Running = false;
    return TRUE;
}

void MediaLogCallback(const char* msg) {
    LogInfo("[Media] %s", msg);
}
