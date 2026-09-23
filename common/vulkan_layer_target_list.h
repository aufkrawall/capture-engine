#pragma once

// Which processes the implicit Vulkan layer may enter at all.
//
// CE's Vulkan layer is registered as an IMPLICIT layer, so the Vulkan loader
// loads it into every Vulkan process on the machine. "Dormant" used to mean
// passthrough: the layer still sat in the call chain of every Vulkan
// application, pinned itself resident and started a host watcher thread there -
// including games whose profile says dll_injection=never, and games protected
// by anti-cheat that scans for foreign modules and call-chain hooks.
//
// The loader offers the right tool for this: a layer whose
// vkNegotiateLoaderLayerInterfaceVersion returns VK_ERROR_INITIALIZATION_FAILED
// is treated as unusable and not loaded into the instance (Khronos
// LoaderLayerInterface.md), while the application's vkCreateInstance continues
// normally. The layer now declines there unless the process is one CE may
// inject into:
//  - with a compatible CaptureEngine host running, exactly when the host's
//    published whitelist (or a whitelisted parent) makes it eligible;
//  - with no host, when its executable is in the injection whitelist the host
//    persisted last time (below). That keeps late injection working for a
//    whitelisted Vulkan title started before CaptureEngine.
// A declined layer never pins itself, so the loader unloads it again.
//
// The persisted list is a REG_MULTI_SZ value in CE's per-user key, not a file.
// The layer runs from a versioned staging copy
// (common/vulkan_layer_registration.cpp: %LOCALAPPDATA% or %ProgramData%
// \CaptureEngine\vulkan_layers\b<build>), which the injector has no reason to
// know and which a resident layer of an older build does not share with the
// current host; the registry key is the same for every build, both bitnesses
// (HKCU\Software is not WOW64-redirected) and every installation directory.
// The first version wrote vulkan_layer_targets.txt beside captureengine.exe
// while the layer looked beside its staged DLL, so no Vulkan title started
// before CaptureEngine was ever admitted.

#include <windows.h>

#include <string>
#include <string_view>
#include <vector>

namespace ce::vulkan_layer_targets {

inline constexpr const wchar_t* kRegistryKey = L"Software\\CaptureEngine";
inline constexpr const wchar_t* kRegistryValue = L"VulkanLayerTargets";

inline std::wstring ToLowerAscii(std::wstring_view text) {
    std::wstring lower(text);
    for (wchar_t& ch : lower) {
        if (ch >= L'A' && ch <= L'Z')
            ch = static_cast<wchar_t>(ch - L'A' + L'a');
    }
    return lower;
}

// The REG_MULTI_SZ image: one lower-cased executable name per string, stable
// order, no duplicates, terminated by an empty string.
inline std::wstring SerializeTargetList(const std::vector<std::wstring>& names) {
    std::vector<std::wstring> unique;
    for (const std::wstring& name : names) {
        if (name.empty() || name.find(L'\0') != std::wstring::npos)
            continue;
        const std::wstring lower = ToLowerAscii(name);
        bool seen = false;
        for (const std::wstring& existing : unique)
            seen = seen || existing == lower;
        if (!seen)
            unique.push_back(lower);
    }
    std::wstring contents;
    for (const std::wstring& name : unique) {
        contents += name;
        contents += L'\0';
    }
    contents += L'\0';
    return contents;
}

// Case-insensitive exact match of `processName` against a REG_MULTI_SZ list,
// the same rule the host's published whitelist uses. The list ends at the first
// empty string or at the end of the data, whichever comes first.
inline bool IsProcessNameListed(std::wstring_view list, std::wstring_view processName) {
    if (processName.empty())
        return false;
    const std::wstring wanted = ToLowerAscii(processName);
    size_t start = 0;
    while (start < list.size()) {
        size_t end = list.find(L'\0', start);
        if (end == std::wstring_view::npos)
            end = list.size();
        if (end == start)
            return false;
        if (ToLowerAscii(list.substr(start, end - start)) == wanted)
            return true;
        start = end + 1;
    }
    return false;
}

// The participation decision at negotiation time. A running compatible host is
// authoritative (its whitelist is current and knows about inherited renderer
// processes); the persisted list only stands in while no host is published.
inline bool ShouldLayerParticipate(bool compatibleHostPublished, bool eligibleByHost, bool listedAsTarget) {
    return compatibleHostPublished ? eligibleByHost : listedAsTarget;
}

// Reads the persisted list. Returns false when the value is absent or
// unreadable; `outList` is then empty.
inline bool ReadPersistedTargetList(std::wstring* outList, const wchar_t* valueName = kRegistryValue) {
    outList->clear();
    // The value can be replaced between the size query and the read; the
    // second read then reports ERROR_MORE_DATA and is simply repeated.
    for (int attempt = 0; attempt < 4; ++attempt) {
        DWORD bytes = 0;
        LONG status =
            RegGetValueW(HKEY_CURRENT_USER, kRegistryKey, valueName, RRF_RT_REG_MULTI_SZ, nullptr, nullptr, &bytes);
        if (status != ERROR_SUCCESS)
            return false;
        std::wstring data(bytes / sizeof(wchar_t) + 1, L'\0');
        DWORD size = static_cast<DWORD>(data.size() * sizeof(wchar_t));
        status = RegGetValueW(HKEY_CURRENT_USER, kRegistryKey, valueName, RRF_RT_REG_MULTI_SZ, nullptr, data.data(),
                              &size);
        if (status == ERROR_MORE_DATA)
            continue;
        if (status != ERROR_SUCCESS)
            return false;
        data.resize(size / sizeof(wchar_t));
        *outList = std::move(data);
        return true;
    }
    return false;
}

// Replaces the persisted list. A registry value is replaced as a whole, so a
// reader never observes a partial list. Returns the Win32 status.
inline LONG WritePersistedTargetList(const std::wstring& list, const wchar_t* valueName = kRegistryValue) {
    return RegSetKeyValueW(HKEY_CURRENT_USER, kRegistryKey, valueName, REG_MULTI_SZ, list.data(),
                           static_cast<DWORD>(list.size() * sizeof(wchar_t)));
}

// Removes the persisted list (explicit unregistration). An already absent
// value counts as removed. Returns the Win32 status.
inline LONG DeletePersistedTargetList(const wchar_t* valueName = kRegistryValue) {
    const LONG status = RegDeleteKeyValueW(HKEY_CURRENT_USER, kRegistryKey, valueName);
    return status == ERROR_FILE_NOT_FOUND ? ERROR_SUCCESS : status;
}

}  // namespace ce::vulkan_layer_targets
