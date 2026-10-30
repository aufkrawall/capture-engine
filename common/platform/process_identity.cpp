#include "process_identity.h"

#include "common/platform/utf8.h"

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

    std::vector<wchar_t> path(32768, L'\0');
    DWORD pathLength = static_cast<DWORD>(path.size());
    if (!QueryFullProcessImageNameW(process, 0, path.data(), &pathLength) || pathLength == 0 ||
        pathLength >= path.size()) {
        result.error = GetLastError();
        CloseHandle(process);
        return result;
    }
    CloseHandle(process);

    const std::wstring widePath(path.data(), pathLength);
    std::string imagePath = ce::utf8::FromWide(widePath);
    if (imagePath.empty()) {
        result.error = ERROR_NO_UNICODE_TRANSLATION;
        return result;
    }
    std::string imageName = imagePath;
    const size_t separator = imageName.find_last_of("\\/");
    if (separator != std::string::npos)
        imageName.erase(0, separator + 1);
    if (imageName.empty()) {
        result.error = ERROR_INVALID_DATA;
        return result;
    }

    result.imageName = std::move(imageName);
    result.imagePath = std::move(imagePath);
    result.error = ERROR_SUCCESS;
    return result;
}

}  // namespace ce::process
