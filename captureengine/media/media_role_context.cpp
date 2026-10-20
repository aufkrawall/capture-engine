#include "media_role_context.h"

#include "common/logging/logging.h"
#include "common/platform/ansi_path.h"

namespace ce::media_role {

RolePaths DeriveRolePaths(const std::wstring& packageDir, const std::string& configPathArgument) {
    RolePaths paths;
    paths.configPath = configPathArgument.empty() ? ce::ansi_path::CompatiblePath(packageDir) + "\\config.ini"
                                                  : configPathArgument;
    const size_t slash = paths.configPath.find_last_of("\\/");
    paths.mediaCacheDir = slash == std::string::npos ? std::string() : paths.configPath.substr(0, slash);
    return paths;
}

bool WaitForPollOrStop(HANDLE stop, DWORD ms) {
    if (!stop) {
        Sleep(ms);
        return false;
    }
    const DWORD result = WaitForSingleObject(stop, ms);
    if (result == WAIT_OBJECT_0)
        return true;
    if (result != WAIT_TIMEOUT)
        Sleep(ms);  // not a waitable handle: keep the poll cadence rather than spin
    return false;
}

bool AllowsProcessTuning(bool processTuning, std::atomic<bool>& loggedRefusal) {
    if (processTuning)
        return true;
    if (!loggedRefusal.exchange(true, std::memory_order_relaxed))
        LogInfo(
            "[MediaPriority] process priority and GPU scheduling left to the client (no CE_FEATURE_PROCESS_TUNING)");
    return false;
}

}  // namespace ce::media_role
