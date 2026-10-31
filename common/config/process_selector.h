#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>
#include <string_view>

#include "common/platform/utf8.h"

namespace ce::config {

// A process selector containing a path separator is a case-insensitive image
// path suffix. A selector without one keeps the historical executable-name
// behavior. Path suffixes match complete components, so "game\app.exe" does
// not match "notgame\app.exe".
template <typename CharT>
inline bool ProcessSelectorIsPath(std::basic_string_view<CharT> selector) {
    return selector.find(static_cast<CharT>('\\')) != std::basic_string_view<CharT>::npos ||
           selector.find(static_cast<CharT>('/')) != std::basic_string_view<CharT>::npos;
}

template <typename CharT>
inline bool SelectorPathSeparator(CharT value) {
    return value == static_cast<CharT>('\\') || value == static_cast<CharT>('/');
}

template <typename CharT>
inline CharT SelectorAsciiLower(CharT value) {
    if constexpr (sizeof(CharT) == 1) {
        return static_cast<CharT>(std::tolower(static_cast<unsigned char>(value)));
    } else {
        return value >= static_cast<CharT>('A') && value <= static_cast<CharT>('Z')
                   ? static_cast<CharT>(value - static_cast<CharT>('A') + static_cast<CharT>('a'))
                   : value;
    }
}

template <typename CharT>
inline bool SelectorEqualsInsensitive(std::basic_string_view<CharT> lhs, std::basic_string_view<CharT> rhs) {
    return lhs.size() == rhs.size() && std::equal(lhs.begin(), lhs.end(), rhs.begin(), [](CharT left, CharT right) {
               return SelectorAsciiLower(left) == SelectorAsciiLower(right);
           });
}

template <>
inline bool SelectorEqualsInsensitive<wchar_t>(std::basic_string_view<wchar_t> lhs,
                                               std::basic_string_view<wchar_t> rhs) {
    return ce::utf8::EqualsOrdinalIgnoreCase(lhs, rhs);
}

template <typename CharT>
inline std::basic_string<CharT> NormalizeSelectorPath(std::basic_string_view<CharT> path) {
    std::basic_string<CharT> normalized(path);
    for (CharT& value : normalized) {
        if (value == static_cast<CharT>('/'))
            value = static_cast<CharT>('\\');
        // Fold only ASCII here. UTF-8 bytes above 0x7f are compared after
        // conversion to UTF-16, and lowercasing individual bytes would corrupt
        // the sequence.
        using UnsignedCharT = std::make_unsigned_t<CharT>;
        const uint32_t code = static_cast<uint32_t>(static_cast<UnsignedCharT>(value));
        if (code <= 0x7f)
            value = SelectorAsciiLower(value);
    }
    if (normalized.size() >= 4 && normalized[0] == static_cast<CharT>('\\') &&
        normalized[1] == static_cast<CharT>('?') && normalized[2] == static_cast<CharT>('?') &&
        normalized[3] == static_cast<CharT>('\\')) {
        normalized.erase(0, 4);
    }
    if (normalized.size() >= 4 && normalized[0] == static_cast<CharT>('\\') &&
        normalized[1] == static_cast<CharT>('\\') && normalized[2] == static_cast<CharT>('?') &&
        normalized[3] == static_cast<CharT>('\\')) {
        if (normalized.size() >= 8 && normalized[4] == static_cast<CharT>('u') &&
            normalized[5] == static_cast<CharT>('n') && normalized[6] == static_cast<CharT>('c') &&
            normalized[7] == static_cast<CharT>('\\')) {
            normalized = std::basic_string<CharT>(2, static_cast<CharT>('\\')) + normalized.substr(8);
        } else if (normalized.size() >= 7 && std::isalpha(static_cast<unsigned char>(normalized[4])) &&
                   normalized[5] == static_cast<CharT>(':') && normalized[6] == static_cast<CharT>('\\')) {
            normalized.erase(0, 4);
        }
    }
    return normalized;
}

template <typename CharT>
inline std::basic_string<CharT> SelectorAsciiLowerCopy(std::basic_string_view<CharT> value) {
    std::basic_string<CharT> lowered(value);
    for (CharT& ch : lowered)
        ch = SelectorAsciiLower(ch);
    return lowered;
}

template <typename CharT>
inline bool SelectorContainsInsensitive(std::basic_string_view<CharT> text, std::basic_string_view<CharT> needle) {
    return SelectorAsciiLowerCopy(text).find(SelectorAsciiLowerCopy(needle)) != std::basic_string<CharT>::npos;
}

template <>
inline bool SelectorContainsInsensitive<wchar_t>(std::basic_string_view<wchar_t> text,
                                                 std::basic_string_view<wchar_t> needle) {
    return ce::utf8::ContainsOrdinalIgnoreCase(text, needle);
}

template <typename CharT>
inline bool ProcessPathSelectorMatches(std::basic_string_view<CharT> selector,
                                       std::basic_string_view<CharT> processPath) {
    if (!ProcessSelectorIsPath(selector) || processPath.empty())
        return false;

    const std::basic_string<CharT> normalizedSelector = NormalizeSelectorPath(selector);
    const std::basic_string<CharT> normalizedPath = NormalizeSelectorPath(processPath);
    if (normalizedSelector.size() > normalizedPath.size())
        return false;

    const size_t offset = normalizedPath.size() - normalizedSelector.size();
    if (!SelectorEqualsInsensitive(std::basic_string_view<CharT>(normalizedSelector),
                                   std::basic_string_view<CharT>(normalizedPath).substr(offset)))
        return false;
    return offset == 0 || SelectorPathSeparator(normalizedPath[offset - 1]) ||
           SelectorPathSeparator(normalizedSelector.front());
}

template <typename CharT>
inline bool ProcessSelectorMatches(std::basic_string_view<CharT> selector, std::basic_string_view<CharT> processName,
                                   std::basic_string_view<CharT> processPath, bool requireExactName = true) {
    if (ProcessSelectorIsPath(selector))
        return ProcessPathSelectorMatches(selector, processPath);
    if (SelectorEqualsInsensitive(selector, processName))
        return true;
    return !requireExactName && SelectorContainsInsensitive(processName, selector);
}

template <typename CharT>
inline int ProcessSelectorSpecificity(std::basic_string_view<CharT> selector) {
    if (!ProcessSelectorIsPath(selector))
        return 0;
    return 1'000'000 + static_cast<int>(NormalizeSelectorPath(selector).size());
}

// The discovery-memory and persisted Vulkan lists need to distinguish a
// positive injection route from a more-specific profile that must shadow it.
// U+001F is invalid in Windows path components, so it cannot collide with a
// real executable selector. Existing unprefixed entries remain positive.
inline constexpr char kProcessSelectorDenyPrefix = '\x1f';

inline std::string EncodeProcessSelector(std::string_view selector, bool allow) {
    std::string encoded(selector);
    if (!allow)
        encoded.insert(encoded.begin(), kProcessSelectorDenyPrefix);
    return encoded;
}

template <typename CharT>
inline bool DecodeProcessSelectorDecision(std::basic_string_view<CharT>& selector) {
    if (!selector.empty() && selector.front() == static_cast<CharT>(kProcessSelectorDenyPrefix)) {
        selector.remove_prefix(1);
        return false;
    }
    return true;
}

enum class ProcessSelectorVerdict : uint8_t { kNoMatch, kDeny, kAllow };

struct ProcessSelectorEvaluation {
    ProcessSelectorVerdict verdict = ProcessSelectorVerdict::kNoMatch;
    int specificity = -1;
};

template <typename CharT>
inline ProcessSelectorEvaluation EvaluateProcessSelector(std::basic_string_view<CharT> encodedSelector,
                                                         std::basic_string_view<CharT> processName,
                                                         std::basic_string_view<CharT> processPath) {
    const bool allow = DecodeProcessSelectorDecision(encodedSelector);
    if (!ProcessSelectorMatches(encodedSelector, processName, processPath, true))
        return {};
    return {allow ? ProcessSelectorVerdict::kAllow : ProcessSelectorVerdict::kDeny,
            ProcessSelectorSpecificity(encodedSelector)};
}

inline ProcessSelectorEvaluation EvaluateProcessSelectorUtf8(std::string_view encodedSelector,
                                                             std::string_view processName,
                                                             std::string_view processPath) {
    const std::wstring wideSelector = ce::utf8::ToWide(encodedSelector);
    if (wideSelector.empty())
        return {};
    return EvaluateProcessSelector(std::wstring_view(wideSelector), std::wstring_view(ce::utf8::ToWide(processName)),
                                   std::wstring_view(ce::utf8::ToWide(processPath)));
}

}  // namespace ce::config
