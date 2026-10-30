#pragma once

#include <windows.h>

#include <string_view>

namespace ce::process_loopback {

// The worker receives a single selector, never a command or file to open.
// Accept Windows image paths as well as executable names while rejecting
// characters that cannot occur in either and drive-relative path syntax.
inline bool IsValidWorkerProcessSelector(std::wstring_view selector) {
    if (selector.empty() || selector.size() >= 32767 || selector == L"." || selector == L".." ||
        selector.back() == L'.' || selector.back() == L' ' || selector.back() == L'\\' || selector.back() == L'/')
        return false;
    const auto separator = [](wchar_t value) { return value == L'\\' || value == L'/'; };
    const bool extended = selector.size() >= 4 && separator(selector[0]) && separator(selector[1]) &&
                          selector[2] == L'?' && separator(selector[3]);
    for (size_t index = 0; index < selector.size(); ++index) {
        const wchar_t value = selector[index];
        if (value < 0x20 || value == L'*' || value == L'"' || value == L'<' || value == L'>' || value == L'|')
            return false;
        if (value == L'?' && !(extended && index == 2))
            return false;
        if (value == L':') {
            const size_t drive = extended ? 4 : 0;
            if (index != drive + 1 || index + 1 >= selector.size() || !separator(selector[index + 1]) ||
                !((selector[drive] >= L'A' && selector[drive] <= L'Z') ||
                  (selector[drive] >= L'a' && selector[drive] <= L'z')))
                return false;
        }
    }
    return WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, selector.data(), static_cast<int>(selector.size()),
                               nullptr, 0, nullptr, nullptr) > 0;
}

// Worker exclusion arguments use a one-character match mode followed by the
// selector. Each argument remains independently quoted by the launcher.
inline bool DecodeWorkerExclusion(std::wstring_view argument, std::wstring_view& selector, bool& exact) {
    if (argument.size() < 2 || (argument.front() != L'E' && argument.front() != L'C'))
        return false;
    exact = argument.front() == L'E';
    selector = argument.substr(1);
    return IsValidWorkerProcessSelector(selector);
}

}  // namespace ce::process_loopback
