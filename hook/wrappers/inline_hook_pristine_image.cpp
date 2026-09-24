/**
 * Inline Hook — pristine original-bytes read
 *
 * Reads a function's unpatched bytes from its module's image file on disk and
 * applies the image-base relocations, so deep hooks and bypass trampolines can
 * verify live code against what the module shipped. Split out of
 * inline_hook_deep.cpp; see inline_hook_internal.h for the shared surface and
 * inline_hook_pristine_image.h for the PE/relocation policy this drives.
 */

#include "inline_hook_internal.h"
#include "inline_hook_pristine_image.h"

#include <windows.h>
#include <vector>

#include "../common/child_inject_policy.h"
#include "../common/hook_common.h"

namespace InlineHook {

// Read original (unpatched) function bytes from the DLL file and apply the
// module's image-base relocations so absolute operands match the loaded image.
bool ReadOrigBytesFromDisk(void* funcAddr, uint8_t* outBuf, int count, size_t* relocationsApplied) {
    if (relocationsApplied)
        *relocationsApplied = 0;
    HMODULE hMod = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCSTR)funcAddr, &hMod) ||
        !hMod) {
        return false;
    }

    wchar_t modPath[MAX_PATH];
    if (!ce::child_inject_policy::GetHookModulePathW(hMod, modPath, MAX_PATH))
        return false;

    const uintptr_t rva = (uintptr_t)funcAddr - (uintptr_t)hMod;
    if (rva > MAXDWORD || count <= 0)
        return false;

    HANDLE hFile =
        CreateFileW(modPath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (hFile == INVALID_HANDLE_VALUE)
        return false;

    LARGE_INTEGER fileSize = {};
    constexpr LONGLONG kMaxPristineImageBytes = 512LL * 1024LL * 1024LL;
    if (!GetFileSizeEx(hFile, &fileSize) || fileSize.QuadPart <= 0 || fileSize.QuadPart > kMaxPristineImageBytes) {
        CloseHandle(hFile);
        return false;
    }
    std::vector<uint8_t> fileBytes(static_cast<size_t>(fileSize.QuadPart));
    size_t totalRead = 0;
    while (totalRead < fileBytes.size()) {
        DWORD bytesRead = 0;
        const DWORD request = static_cast<DWORD>(fileBytes.size() - totalRead);
        if (!ReadFile(hFile, fileBytes.data() + totalRead, request, &bytesRead, nullptr) || bytesRead == 0) {
            CloseHandle(hFile);
            return false;
        }
        totalRead += bytesRead;
    }
    CloseHandle(hFile);

    const auto result = ce::inline_hook_pristine_image::ReadRelocatedImageBytes(
        fileBytes.data(), fileBytes.size(), reinterpret_cast<uintptr_t>(hMod), static_cast<DWORD>(rva), outBuf,
        static_cast<size_t>(count));
    if (!result.Succeeded()) {
        HookLogImportant("PristineImage: Refusing original bytes for %p (%s)", funcAddr,
                         ce::inline_hook_pristine_image::ImageBytesIssueName(result.issue));
        return false;
    }
    if (relocationsApplied)
        *relocationsApplied = result.relocationsApplied;
    return true;
}

}  // namespace InlineHook
