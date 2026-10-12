#include "config_load_internal.h"

#include <string_view>

namespace {

bool IsHorizontalBlank(char ch) {
    return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n';
}

std::string_view TrimView(std::string_view text) {
    size_t begin = 0;
    size_t end = text.size();
    while (begin < end && IsHorizontalBlank(text[begin])) {
        ++begin;
    }
    while (end > begin && IsHorizontalBlank(text[end - 1])) {
        --end;
    }
    return text.substr(begin, end - begin);
}

bool EqualsAsciiInsensitive(std::string_view lhs, std::string_view rhs) {
    if (lhs.size() != rhs.size()) {
        return false;
    }
    for (size_t i = 0; i < lhs.size(); ++i) {
        const char a = static_cast<char>(std::tolower(static_cast<unsigned char>(lhs[i])));
        const char b = static_cast<char>(std::tolower(static_cast<unsigned char>(rhs[i])));
        if (a != b) {
            return false;
        }
    }
    return true;
}

bool AreConfigDllPathsEquivalent(std::string_view lhs, std::string_view rhs) {
    size_t i = 0;
    size_t j = 0;
    while (i < lhs.size() && j < rhs.size()) {
        const bool lhsSep = lhs[i] == '\\' || lhs[i] == '/';
        const bool rhsSep = rhs[j] == '\\' || rhs[j] == '/';
        if (lhsSep != rhsSep) {
            return false;
        }
        if (lhsSep) {
            while (i < lhs.size() && (lhs[i] == '\\' || lhs[i] == '/')) {
                ++i;
            }
            while (j < rhs.size() && (rhs[j] == '\\' || rhs[j] == '/')) {
                ++j;
            }
            continue;
        }
        const char a = static_cast<char>(std::tolower(static_cast<unsigned char>(lhs[i])));
        const char b = static_cast<char>(std::tolower(static_cast<unsigned char>(rhs[j])));
        if (a != b) {
            return false;
        }
        ++i;
        ++j;
    }
    return i == lhs.size() && j == rhs.size();
}

// GetPrivateProfileStringA and LookupIniValue strip one outer matching quote
// pair when a value starts and ends with the same quote character. For a list
// written as 'a.dll', 'b.dll' (without an outer wrapper quote), that leaves
// a.dll', 'b.dll. Detect that boundary pattern and restore the outer quote pair
// before tokenizing so quoted paths containing spaces or commas parse identically
// whether or not the whole list was wrapped in outer quotes.
std::string NormalizeOuterListQuotes(std::string_view raw) {
    std::string_view trimmed = TrimView(raw);
    if (trimmed.size() >= 4 &&
        ((trimmed.front() == '"' && trimmed.back() == '"') ||
         (trimmed.front() == '\'' && trimmed.back() == '\''))) {
        const char outerQuote = trimmed.front();
        const std::string_view inner = TrimView(trimmed.substr(1, trimmed.size() - 2));
        if (inner.size() >= 2 &&
            ((outerQuote == '"' && inner.front() == '\'' && inner.back() == '\'') ||
             (outerQuote == '\'' && inner.front() == '"' && inner.back() == '"'))) {
            return std::string(inner);
        }
    }

    if (trimmed.empty() || trimmed.front() == '\'' || trimmed.front() == '"' ||
        trimmed.back() == '\'' || trimmed.back() == '"') {
        return std::string(trimmed);
    }

    for (char quoteChar : {'\'', '"'}) {
        bool hasQuoteBeforeComma = false;
        bool hasQuoteAfterComma = false;
        for (size_t pos = 0; pos < trimmed.size(); ++pos) {
            if (trimmed[pos] != ',') {
                continue;
            }
            size_t left = pos;
            while (left > 0 && IsHorizontalBlank(trimmed[left - 1])) {
                --left;
            }
            if (left > 0 && trimmed[left - 1] == quoteChar) {
                hasQuoteBeforeComma = true;
            }
            size_t right = pos + 1;
            while (right < trimmed.size() && IsHorizontalBlank(trimmed[right])) {
                ++right;
            }
            if (right < trimmed.size() && trimmed[right] == quoteChar) {
                hasQuoteAfterComma = true;
            }
        }
        if (hasQuoteBeforeComma && hasQuoteAfterComma) {
            std::string restored;
            restored.reserve(trimmed.size() + 2);
            restored.push_back(quoteChar);
            restored.append(trimmed.data(), trimmed.size());
            restored.push_back(quoteChar);
            return restored;
        }
    }

    return std::string(trimmed);
}

}  // namespace

std::vector<std::string> ParseThirdPartyCustomDllPaths(const std::string& val) {
    const std::string_view trimmed = TrimView(val);
    if (trimmed.empty() || EqualsAsciiInsensitive(trimmed, "off") ||
        EqualsAsciiInsensitive(trimmed, "none")) {
        return {};
    }

    const std::string normalized = NormalizeOuterListQuotes(trimmed);
    std::vector<std::string> paths;
    std::string current;
    char activeQuote = '\0';
    bool tokenStarted = false;

    auto flushCurrent = [&]() {
        std::string_view candidate = TrimView(current);
        while (candidate.size() >= 2 &&
               ((candidate.front() == '\'' && candidate.back() == '\'') ||
                (candidate.front() == '"' && candidate.back() == '"'))) {
            candidate = TrimView(candidate.substr(1, candidate.size() - 2));
        }
        while (!candidate.empty() && (candidate.front() == '\'' || candidate.front() == '"')) {
            candidate = TrimView(candidate.substr(1));
        }
        while (!candidate.empty() && (candidate.back() == '\'' || candidate.back() == '"')) {
            candidate = TrimView(candidate.substr(0, candidate.size() - 1));
        }
        if (!candidate.empty() && !EqualsAsciiInsensitive(candidate, "off") &&
            !EqualsAsciiInsensitive(candidate, "none")) {
            const bool duplicate = std::any_of(paths.begin(), paths.end(), [&](const std::string& existing) {
                return AreConfigDllPathsEquivalent(existing, candidate);
            });
            if (!duplicate) {
                paths.emplace_back(candidate);
            }
        }
        current.clear();
        activeQuote = '\0';
        tokenStarted = false;
    };

    for (size_t i = 0; i < normalized.size(); ++i) {
        const char ch = normalized[i];
        if (activeQuote != '\0') {
            if (ch == activeQuote) {
                activeQuote = '\0';
            } else {
                current.push_back(ch);
            }
            continue;
        }
        if (ch == ',') {
            flushCurrent();
            continue;
        }
        if (!tokenStarted) {
            if (IsHorizontalBlank(ch)) {
                continue;
            }
            tokenStarted = true;
            if (ch == '\'' || ch == '"') {
                activeQuote = ch;
                continue;
            }
        }
        current.push_back(ch);
    }
    flushCurrent();
    return paths;
}

void LoadThirdParty(ConfigReader& reader, AppConfig& config) {
    config.thirdParty.reshadeDllPath = reader.GetStr("ThirdParty", "reshade_dll_path", "");
    config.thirdParty.optiscalerDllPath = reader.GetStr("ThirdParty", "optiscaler_dll_path", "");
    config.thirdParty.specialkDllPath = reader.GetStr("ThirdParty", "specialk_dll_path", "");
    config.thirdParty.customDllPaths = ParseThirdPartyCustomDllPaths(
        reader.GetStrCompat("ThirdParty", "custom_dll_paths", "ThirdParty", "custom_dll_path", "", true));
}
