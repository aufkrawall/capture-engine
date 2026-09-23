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
//    persisted last time (this file). That keeps late injection working for a
//    whitelisted Vulkan title started before CaptureEngine.
// A declined layer never pins itself, so the loader unloads it again.

#include <cctype>
#include <string>
#include <string_view>
#include <vector>

namespace ce::vulkan_layer_targets {

// Lives next to the layer DLL and the manifest (the installation directory).
inline constexpr const wchar_t* kTargetListFileName = L"vulkan_layer_targets.txt";
inline constexpr std::string_view kTargetListHeader =
    "# CaptureEngine: executables the Vulkan layer may enter while CaptureEngine is not running.\n"
    "# Written from the injection-enabled application profiles; do not edit.\n";

inline std::string ToLowerAscii(std::string_view text) {
    std::string lower(text);
    for (char& ch : lower)
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return lower;
}

// One lower-cased executable name per line, stable order, no duplicates.
inline std::string SerializeTargetList(const std::vector<std::string>& names) {
    std::vector<std::string> unique;
    for (const std::string& name : names) {
        if (name.empty() || name.find_first_of("\r\n") != std::string::npos)
            continue;
        const std::string lower = ToLowerAscii(name);
        bool seen = false;
        for (const std::string& existing : unique)
            seen = seen || existing == lower;
        if (!seen)
            unique.push_back(lower);
    }
    std::string contents(kTargetListHeader);
    for (const std::string& name : unique) {
        contents += name;
        contents += '\n';
    }
    return contents;
}

// Case-insensitive exact match of `processName` against the list, the same rule
// the host's published whitelist uses.
inline bool IsProcessNameListed(std::string_view contents, std::string_view processName) {
    if (processName.empty())
        return false;
    const std::string wanted = ToLowerAscii(processName);
    size_t lineStart = 0;
    while (lineStart < contents.size()) {
        size_t lineEnd = contents.find('\n', lineStart);
        if (lineEnd == std::string_view::npos)
            lineEnd = contents.size();
        std::string_view line = contents.substr(lineStart, lineEnd - lineStart);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
            line.remove_suffix(1);
        while (!line.empty() && (line.front() == ' ' || line.front() == '\t'))
            line.remove_prefix(1);
        if (!line.empty() && line.front() != '#' && ToLowerAscii(line) == wanted)
            return true;
        lineStart = lineEnd + 1;
    }
    return false;
}

// The participation decision at negotiation time. A running compatible host is
// authoritative (its whitelist is current and knows about inherited renderer
// processes); the persisted list only stands in while no host is published.
inline bool ShouldLayerParticipate(bool compatibleHostPublished, bool eligibleByHost, bool listedAsTarget) {
    return compatibleHostPublished ? eligibleByHost : listedAsTarget;
}

}  // namespace ce::vulkan_layer_targets
