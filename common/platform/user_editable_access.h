#pragma once

// What makes a file or folder editable by ordinary users while the folder around it stays administrator-only (an
// installation under Program Files): one explicit BUILTIN\Users Modify entry on the object itself. The installer
// (config.ini, the folders the program writes) and an elevated first-run config.ini share this one definition.
//
// It is applied through a handle, so it lands on exactly the object the handle names: a caller that opens with
// FILE_FLAG_OPEN_REPARSE_POINT can never open up the target of a link by accident.

#include <windows.h>
#include <aclapi.h>

#include <array>

namespace ce::user_access {

// Adds the entry to the DACL of the object `object` was opened on (READ_CONTROL | WRITE_DAC, never MAXIMUM_ALLOWED,
// which stops propagation). With `inheritToChildren` (a folder) it also reaches the folder's existing and future
// contents. ERROR_SUCCESS or the Windows error.
inline DWORD GrantUsersModify(HANDLE object, bool inheritToChildren) {
    std::array<BYTE, SECURITY_MAX_SID_SIZE> sid{};
    DWORD sidSize = static_cast<DWORD>(sid.size());
    if (!CreateWellKnownSid(WinBuiltinUsersSid, nullptr, sid.data(), &sidSize))
        return GetLastError();
    EXPLICIT_ACCESS_W access{};
    access.grfAccessPermissions = FILE_GENERIC_READ | FILE_GENERIC_WRITE | FILE_GENERIC_EXECUTE | DELETE;
    access.grfAccessMode = GRANT_ACCESS;
    access.grfInheritance = inheritToChildren ? SUB_CONTAINERS_AND_OBJECTS_INHERIT : NO_INHERITANCE;
    access.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    access.Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
    access.Trustee.ptstrName = reinterpret_cast<LPWSTR>(sid.data());

    PACL current = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    DWORD status = GetSecurityInfo(object, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, &current,
                                   nullptr, &descriptor);
    PACL updated = nullptr;
    if (status == ERROR_SUCCESS)
        status = SetEntriesInAclW(1, &access, current, &updated);
    if (status == ERROR_SUCCESS)
        status = SetSecurityInfo(object, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, updated, nullptr);
    if (updated)
        LocalFree(updated);
    if (descriptor)
        LocalFree(descriptor);
    return status;
}

}  // namespace ce::user_access
