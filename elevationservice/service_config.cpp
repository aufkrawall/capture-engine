#include "service_internal.h"
#include "common/setup/elevation_runtime_policy.h"

namespace ce::elevation {

// The installation's config.ini lives in an administrator-only folder, so an unelevated CaptureEngine cannot put a
// deleted one back. The service can: it writes its own template into the folder it was staged under, never a path a
// client names, and never over an existing file.
DWORD RestoreConfig() {
    const std::filesystem::path folder = ce::startup::ApplicationFolderForServiceExecutable(ExecutablePath());
    if (folder.empty())
        return ERROR_BAD_PATHNAME;
    return CreateDefaultConfigInFolder(folder.wstring());
}

}  // namespace ce::elevation
