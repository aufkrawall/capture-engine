#pragma once

// The graphics-runtime environment variables a game process actually sees (VKD3D_*, DXVK_*, VK_*, ...).
// A variable set for one shell, a launcher started before it, or another user context never reaches the
// game, and nothing in the logs said so: the 2026-10-29 vkd3d-proton A/B runs
// (VKD3D_SWAPCHAIN_PRESENT_MODE, VKD3D_DISABLE_EXTENSIONS) could only be confirmed indirectly from swapchain
// flags. The hook logs this list once per game process.

#include <cstddef>
#include <cwctype>
#include <string>
#include <vector>

namespace ce::graphics_env_report {

// Translation layers (vkd3d-proton, DXVK, dxvk-nvapi, Proton), the Vulkan loader, NVIDIA's GL/Vulkan driver
// variables and the D3D runtimes' own switches.
inline constexpr const wchar_t* kPrefixes[] = {L"VKD3D_", L"DXVK_", L"PROTON_", L"VK_", L"__GL_", L"DXGI_", L"D3D12_"};
inline constexpr std::size_t kMaxValueChars = 160;
inline constexpr std::size_t kMaxEntries = 24;

struct Report {
    // "NAME=value", values longer than kMaxValueChars cut with a "...(N chars)" note.
    std::vector<std::wstring> entries;
    // Matching variables, including those past kMaxEntries.
    std::size_t matched = 0;
};

inline bool HasGraphicsPrefix(const wchar_t* name, std::size_t nameLength) {
    for (const wchar_t* prefix : kPrefixes) {
        std::size_t i = 0;
        for (; prefix[i] != L'\0'; ++i) {
            if (i >= nameLength || std::towupper(name[i]) != prefix[i])
                break;
        }
        if (prefix[i] == L'\0')
            return true;
    }
    return false;
}

// `block` is a GetEnvironmentStringsW block: NAME=value\0 ... \0\0. Entries starting with '=' are the
// per-drive current directories, not variables.
inline Report Collect(const wchar_t* block) {
    Report report;
    if (!block)
        return report;
    for (const wchar_t* entry = block; *entry != L'\0';) {
        std::size_t length = 0;
        while (entry[length] != L'\0')
            ++length;
        const wchar_t* equals = entry[0] == L'=' ? nullptr : entry;
        if (equals) {
            while (*equals != L'\0' && *equals != L'=')
                ++equals;
            if (*equals != L'=')
                equals = nullptr;
        }
        if (equals && HasGraphicsPrefix(entry, static_cast<std::size_t>(equals - entry))) {
            ++report.matched;
            if (report.entries.size() < kMaxEntries) {
                const std::size_t nameLength = static_cast<std::size_t>(equals - entry);
                const std::size_t valueLength = length - nameLength - 1;
                std::wstring text(entry, nameLength + 1);
                if (valueLength > kMaxValueChars) {
                    text.append(equals + 1, kMaxValueChars);
                    text += L"...(" + std::to_wstring(valueLength) + L" chars)";
                } else {
                    text.append(equals + 1, valueLength);
                }
                report.entries.push_back(std::move(text));
            }
        }
        entry += length + 1;
    }
    return report;
}

}  // namespace ce::graphics_env_report
