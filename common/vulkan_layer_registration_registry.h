#pragma once

// Registry read/write primitives behind the implicit-layer registration
// (common/vulkan_layer_registration.cpp), split out because the registration
// unit hit the source-size ceiling. This is a mechanical extraction: what each
// helper does and why is documented at its call sites in the registration
// unit, which owns the plan semantics.

#include <windows.h>

#include <filesystem>
#include <string>
#include <vector>

#include "vulkan_layer_registration.h"

namespace ce::vulkan_layer {

// Formatting helpers both registration halves log with.
std::string WideToUtf8(const std::wstring& value);
std::string PathToUtf8(const std::filesystem::path& path);
std::string FormatWindowsError(DWORD error);

inline constexpr wchar_t kImplicitLayersKey[] = L"SOFTWARE\\Khronos\\Vulkan\\ImplicitLayers";

struct RegistryLocation {
    RegistryRoot root;
    RegistryView view;
};

class RegistryKeyGuard {
public:
    RegistryKeyGuard() = default;
    ~RegistryKeyGuard() { Reset(); }
    RegistryKeyGuard(const RegistryKeyGuard&) = delete;
    RegistryKeyGuard& operator=(const RegistryKeyGuard&) = delete;
    RegistryKeyGuard(RegistryKeyGuard&& other) noexcept : key_(other.key_) { other.key_ = nullptr; }
    RegistryKeyGuard& operator=(RegistryKeyGuard&& other) noexcept {
        if (this != &other) {
            Reset();
            key_ = other.key_;
            other.key_ = nullptr;
        }
        return *this;
    }

    void Reset(HKEY key = nullptr) {
        if (key_) RegCloseKey(key_);
        key_ = key;
    }

    HKEY Get() const { return key_; }
    HKEY* Put() {
        Reset();
        return &key_;
    }

private:
    HKEY key_ = nullptr;
};

std::string DescribeLocation(const RegistryLocation& location);
LONG OpenRegistryKey(const RegistryLocation& location, REGSAM access, bool create, RegistryKeyGuard* outKey);
bool DeleteRegistryValue(HKEY key, const std::wstring& valueName, const char* reason,
                         const RegistryLocation& location);
bool WriteRegistryTarget(const RegistryTarget& target);

}  // namespace ce::vulkan_layer
