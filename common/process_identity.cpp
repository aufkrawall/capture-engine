#include "process_identity.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <vector>

namespace ce::process {

ProcessIdentityResult QueryProcessIdentity(uint32_t processId) {
    ProcessIdentityResult result;
    if (processId == 0) {
        result.error = ERROR_INVALID_PARAMETER;
        return result;
    }

    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (!process) {
        result.error = GetLastError();
        return result;
    }

    std::vector<char> path(32768, '\0');
    DWORD pathLength = static_cast<DWORD>(path.size());
    if (!QueryFullProcessImageNameA(process, 0, path.data(), &pathLength) || pathLength == 0 ||
        pathLength >= path.size()) {
        result.error = GetLastError();
        CloseHandle(process);
        return result;
    }
    CloseHandle(process);

    std::string imageName(path.data(), pathLength);
    const size_t separator = imageName.find_last_of("\\/");
    if (separator != std::string::npos)
        imageName.erase(0, separator + 1);
    if (imageName.empty()) {
        result.error = ERROR_INVALID_DATA;
        return result;
    }

    result.imageName = std::move(imageName);
    result.error = ERROR_SUCCESS;
    return result;
}

}  // namespace ce::process
