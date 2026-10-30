#pragma once

#include <windows.h>

#include <string>
#include <string_view>

namespace ce::utf8 {

inline std::wstring ToWide(std::string_view value) {
    if (value.empty())
        return {};
    const int length =
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (length <= 0)
        return {};
    std::wstring result(static_cast<size_t>(length), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(),
                            length) != length)
        return {};
    return result;
}

inline std::string FromWide(std::wstring_view value) {
    if (value.empty())
        return {};
    const int length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
                                           nullptr, 0, nullptr, nullptr);
    if (length <= 0)
        return {};
    std::string result(static_cast<size_t>(length), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(),
                            length, nullptr, nullptr) != length)
        return {};
    return result;
}

inline std::string AsciiLowerCopy(std::string_view value) {
    std::string lowered(value);
    for (char& ch : lowered) {
        const unsigned char byte = static_cast<unsigned char>(ch);
        if (byte >= 'A' && byte <= 'Z')
            ch = static_cast<char>(byte - 'A' + 'a');
    }
    return lowered;
}

inline bool EqualsOrdinalIgnoreCase(std::wstring_view lhs, std::wstring_view rhs) {
    if (lhs.size() != rhs.size())
        return false;
    if (lhs.empty())
        return true;
    return CompareStringOrdinal(lhs.data(), static_cast<int>(lhs.size()), rhs.data(), static_cast<int>(rhs.size()),
                                TRUE) == CSTR_EQUAL;
}

inline bool ContainsOrdinalIgnoreCase(std::wstring_view text, std::wstring_view needle) {
    if (needle.empty())
        return true;
    if (needle.size() > text.size())
        return false;
    return FindStringOrdinal(FIND_FROMSTART, text.data(), static_cast<int>(text.size()), needle.data(),
                             static_cast<int>(needle.size()), TRUE) >= 0;
}

}  // namespace ce::utf8
