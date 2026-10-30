#include "process_identity.h"

#include "common/platform/utf8.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winternl.h>

#include <cstdint>
#include <vector>

namespace ce::process {

namespace {

constexpr ULONG kSystemProcessIdInformationClass = 88;

struct SystemProcessIdInformationRecord {
    PVOID processId = nullptr;
    UNICODE_STRING imageName{};
};

using PfnNtQuerySystemInformation = NTSTATUS(NTAPI*)(ULONG systemInformationClass, PVOID systemInformation,
                                                     ULONG systemInformationLength, PULONG returnLength);
using PfnRtlNtStatusToDosError = ULONG(NTAPI*)(NTSTATUS status);

bool PopulateIdentityFromWidePath(std::wstring_view widePath, ProcessIdentityResult& result) {
    std::string imagePath = ce::utf8::FromWide(widePath);
    if (imagePath.empty()) {
        result.error = ERROR_NO_UNICODE_TRANSLATION;
        return false;
    }
    std::string imageName = imagePath;
    const size_t separator = imageName.find_last_of("\\/");
    if (separator != std::string::npos)
        imageName.erase(0, separator + 1);
    if (imageName.empty()) {
        result.error = ERROR_INVALID_DATA;
        return false;
    }

    result.imageName = std::move(imageName);
    result.imagePath = std::move(imagePath);
    result.error = ERROR_SUCCESS;
    return true;
}

std::vector<std::pair<std::wstring, std::wstring>> QueryLocalDriveDeviceMappings() {
    std::vector<std::pair<std::wstring, std::wstring>> mappings;
    const DWORD required = GetLogicalDriveStringsW(0, nullptr);
    if (required == 0)
        return mappings;
    std::vector<wchar_t> drives(static_cast<size_t>(required) + 1, L'\0');
    const DWORD written = GetLogicalDriveStringsW(required, drives.data());
    if (written == 0 || written > required)
        return mappings;

    std::vector<wchar_t> target(1024, L'\0');
    for (const wchar_t* cursor = drives.data(); *cursor != L'\0'; cursor += wcslen(cursor) + 1) {
        if ( cursor[0] == L'\0' || cursor[1] != L':' )
            continue;
        const std::wstring driveColon(cursor, 2);
        const DWORD chars = QueryDosDeviceW(driveColon.c_str(), target.data(), static_cast<DWORD>(target.size()));
        if (chars == 0 || chars >= target.size())
            continue;
        std::wstring devicePrefix(target.data());
        while (!devicePrefix.empty() && (devicePrefix.back() == L'\\' || devicePrefix.back() == L'/'))
            devicePrefix.pop_back();
        if (!devicePrefix.empty())
            mappings.emplace_back(driveColon, std::move(devicePrefix));
    }
    return mappings;
}

}  // namespace

std::wstring TranslateNtDevicePathToWin32(
    std::wstring_view ntPath, const std::vector<std::pair<std::wstring, std::wstring>>& driveMappings) {
    constexpr std::wstring_view kMupPrefix = L"\\Device\\Mup\\";
    if (ntPath.size() > kMupPrefix.size() &&
        ce::utf8::EqualsOrdinalIgnoreCase(ntPath.substr(0, kMupPrefix.size()), kMupPrefix)) {
        return L"\\\\" + std::wstring(ntPath.substr(kMupPrefix.size()));
    }
    for (const auto& [driveColon, devicePrefix] : driveMappings) {
        if (driveColon.empty() || devicePrefix.empty() || ntPath.size() < devicePrefix.size())
            continue;
        if (!ce::utf8::EqualsOrdinalIgnoreCase(ntPath.substr(0, devicePrefix.size()), devicePrefix))
            continue;
        if (ntPath.size() > devicePrefix.size() && ntPath[devicePrefix.size()] != L'\\' &&
            ntPath[devicePrefix.size()] != L'/')
            continue;
        return driveColon + std::wstring(ntPath.substr(devicePrefix.size()));
    }
    return std::wstring(ntPath);
}

ProcessIdentityResult QueryProcessIdentityBySystemProcessId(uint32_t processId) {
    ProcessIdentityResult result;
    if (processId == 0) {
        result.error = ERROR_INVALID_PARAMETER;
        return result;
    }

    const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    const auto query = ntdll ? reinterpret_cast<PfnNtQuerySystemInformation>(
                                   GetProcAddress(ntdll, "NtQuerySystemInformation"))
                             : nullptr;
    const auto statusToDosError = ntdll ? reinterpret_cast<PfnRtlNtStatusToDosError>(
                                              GetProcAddress(ntdll, "RtlNtStatusToDosError"))
                                        : nullptr;
    if (!query) {
        result.error = ERROR_PROC_NOT_FOUND;
        return result;
    }

    std::vector<wchar_t> buffer(32767, L'\0');
    SystemProcessIdInformationRecord info{};
    info.processId = reinterpret_cast<PVOID>(static_cast<uintptr_t>(processId));
    info.imageName.Length = 0;
    info.imageName.MaximumLength = static_cast<USHORT>(buffer.size() * sizeof(wchar_t));
    info.imageName.Buffer = buffer.data();

    const NTSTATUS status = query(kSystemProcessIdInformationClass, &info, sizeof(info), nullptr);
    if (status != 0) {
        const ULONG converted = statusToDosError ? statusToDosError(status) : 0;
        result.error = converted != 0 ? converted : ERROR_GEN_FAILURE;
        return result;
    }
    if (!info.imageName.Buffer || info.imageName.Length < sizeof(wchar_t)) {
        result.error = ERROR_NOT_FOUND;
        return result;
    }

    const std::wstring_view ntPath(info.imageName.Buffer, info.imageName.Length / sizeof(wchar_t));
    const std::wstring widePath = TranslateNtDevicePathToWin32(ntPath, QueryLocalDriveDeviceMappings());
    PopulateIdentityFromWidePath(widePath, result);
    return result;
}

ProcessIdentityResult QueryProcessIdentity(uint32_t processId) {
    ProcessIdentityResult result;
    if (processId == 0) {
        result.error = ERROR_INVALID_PARAMETER;
        return result;
    }

    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (process) {
        std::vector<wchar_t> path(32768, L'\0');
        DWORD pathLength = static_cast<DWORD>(path.size());
        const BOOL queried = QueryFullProcessImageNameW(process, 0, path.data(), &pathLength);
        const DWORD queryError = queried ? ERROR_SUCCESS : GetLastError();
        CloseHandle(process);
        if (queried && pathLength > 0 && pathLength < path.size()) {
            const std::wstring_view widePath(path.data(), pathLength);
            if (PopulateIdentityFromWidePath(widePath, result))
                return result;
        } else {
            result.error = queryError != ERROR_SUCCESS ? queryError : ERROR_GEN_FAILURE;
        }
    } else {
        result.error = GetLastError();
    }

    const unsigned long primaryError = result.error;
    ProcessIdentityResult fallback = QueryProcessIdentityBySystemProcessId(processId);
    if (fallback)
        return fallback;
    if (primaryError != ERROR_SUCCESS)
        result.error = primaryError;
    else
        result.error = fallback.error;
    return result;
}

}  // namespace ce::process
