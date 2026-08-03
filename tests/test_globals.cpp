
#include "../common/config.h"

// Defined in hook_common.h but not defined in any source compiled for unit
// tests (normally defined in main.cpp of the hook DLL)
    // NOLINTNEXTLINE(bugprone-throwing-static-initialization) - static object default construction is non-allocating (members are trivial or empty)
AppConfig g_LocalConfig;
