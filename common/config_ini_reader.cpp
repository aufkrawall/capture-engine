#include "config_ini_reader.h"

#include <windows.h>

namespace ce::config_text {
namespace {

bool IsBlank(wchar_t ch) {
    return ch == L' ' || ch == L'\t';
}

std::wstring_view TrimBlanks(std::wstring_view text) {
    size_t begin = 0;
    size_t end = text.size();
    while (begin < end && IsBlank(text[begin])) {
        ++begin;
    }
    while (end > begin && IsBlank(text[end - 1])) {
        --end;
    }
    return text.substr(begin, end - begin);
}

bool NamesEqual(std::wstring_view lhs, std::wstring_view rhs) {
    if (lhs.size() != rhs.size()) {
        return false;
    }
    if (lhs.empty()) {
        return true;
    }
    return CompareStringOrdinal(lhs.data(), static_cast<int>(lhs.size()), rhs.data(), static_cast<int>(rhs.size()),
                                TRUE) == CSTR_EQUAL;
}

const IniSection* FindSection(const IniDocument& document, std::wstring_view name) {
    for (const IniSection& section : document.sections) {
        if (NamesEqual(section.name, name)) {
            return &section;
        }
    }
    return nullptr;
}

void ParseLine(std::wstring_view raw, IniDocument* document) {
    const std::wstring_view line = TrimBlanks(raw);
    if (line.empty() || line.front() == L';') {
        return;
    }
    if (line.front() == L'[') {
        std::wstring_view name = line.substr(1);
        const size_t close = name.find(L']');
        if (close != std::wstring_view::npos) {
            name = name.substr(0, close);
        }
        document->sections.push_back(IniSection{std::wstring(TrimBlanks(name)), {}});
        return;
    }
    if (document->sections.empty()) {
        return;
    }
    IniLine entry;
    const size_t equals = line.find(L'=');
    if (equals == std::wstring_view::npos) {
        entry.key = std::wstring(line);
    } else {
        entry.hasEquals = true;
        entry.key = std::wstring(TrimBlanks(line.substr(0, equals)));
        entry.value = std::wstring(TrimBlanks(line.substr(equals + 1)));
    }
    document->sections.back().lines.push_back(std::move(entry));
}

}  // namespace

std::wstring CodePageToWide(std::string_view text, unsigned codePage) {
    if (text.empty()) {
        return {};
    }
    const int length = MultiByteToWideChar(codePage, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (length <= 0) {
        return {};
    }
    std::wstring wide(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(codePage, 0, text.data(), static_cast<int>(text.size()), wide.data(), length);
    return wide;
}

std::string WideToCodePage(std::wstring_view text, unsigned codePage, bool* lossy) {
    if (lossy) {
        *lossy = false;
    }
    if (text.empty()) {
        return {};
    }
    // CP_UTF8 rejects both the no-best-fit flag and a default character, and
    // represents every character anyway.
    const bool utf8 = codePage == CP_UTF8;
    const DWORD flags = utf8 ? 0 : WC_NO_BEST_FIT_CHARS;
    const char* defaultChar = utf8 ? nullptr : "?";
    BOOL usedDefault = FALSE;
    const int length = WideCharToMultiByte(codePage, flags, text.data(), static_cast<int>(text.size()), nullptr, 0,
                                           defaultChar, utf8 ? nullptr : &usedDefault);
    if (length <= 0) {
        if (lossy) {
            *lossy = true;
        }
        return {};
    }
    std::string narrow(static_cast<size_t>(length), '\0');
    usedDefault = FALSE;
    WideCharToMultiByte(codePage, flags, text.data(), static_cast<int>(text.size()), narrow.data(), length,
                        defaultChar, utf8 ? nullptr : &usedDefault);
    if (lossy) {
        *lossy = usedDefault != FALSE;
    }
    return narrow;
}

IniDocument ParseUtf8Ini(std::string_view bytes) {
    if (bytes.size() >= 3 && static_cast<unsigned char>(bytes[0]) == 0xEF &&
        static_cast<unsigned char>(bytes[1]) == 0xBB && static_cast<unsigned char>(bytes[2]) == 0xBF) {
        bytes.remove_prefix(3);
    }
    const std::wstring text = CodePageToWide(bytes, CP_UTF8);
    IniDocument document;
    size_t lineStart = 0;
    for (size_t i = 0; i <= text.size(); ++i) {
        if (i < text.size() && text[i] != L'\r' && text[i] != L'\n') {
            continue;
        }
        ParseLine(std::wstring_view(text).substr(lineStart, i - lineStart), &document);
        if (i + 1 < text.size() && text[i] == L'\r' && text[i + 1] == L'\n') {
            ++i;
        }
        lineStart = i + 1;
    }
    return document;
}

bool LookupIniValue(const IniDocument& document, std::string_view section, std::string_view key,
                    unsigned codePage, std::string* value, bool* lossy) {
    if (lossy) {
        *lossy = false;
    }
    const IniSection* found = FindSection(document, TrimBlanks(CodePageToWide(section, codePage)));
    if (!found) {
        return false;
    }
    const std::wstring wideKey = CodePageToWide(key, codePage);
    const std::wstring_view wantedKey = TrimBlanks(wideKey);
    for (const IniLine& line : found->lines) {
        if (!line.hasEquals || !NamesEqual(line.key, wantedKey)) {
            continue;
        }
        std::wstring_view result = line.value;
        if (result.size() >= 2 && (result.front() == L'"' || result.front() == L'\'') &&
            result.back() == result.front()) {
            result = result.substr(1, result.size() - 2);
        }
        if (value) {
            *value = WideToCodePage(result, codePage, lossy);
        }
        return true;
    }
    return false;
}

std::vector<std::string> IniSectionNames(const IniDocument& document, unsigned codePage) {
    std::vector<std::string> names;
    names.reserve(document.sections.size());
    for (const IniSection& section : document.sections) {
        names.push_back(WideToCodePage(section.name, codePage));
    }
    return names;
}

bool IniSectionLines(const IniDocument& document, std::string_view section, unsigned codePage,
                     std::vector<std::string>* lines) {
    const IniSection* found = FindSection(document, TrimBlanks(CodePageToWide(section, codePage)));
    if (!found) {
        return false;
    }
    if (lines) {
        lines->clear();
        for (const IniLine& line : found->lines) {
            lines->push_back(WideToCodePage(line.hasEquals ? line.key + L"=" + line.value : line.key, codePage));
        }
    }
    return true;
}

}  // namespace ce::config_text
