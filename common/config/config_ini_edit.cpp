#include "config_ini_edit.h"

#include <windows.h>

#include <string>
#include <vector>

#include "config_text_encoding.h"

namespace ce::config_text {
namespace {

constexpr size_t kNone = static_cast<size_t>(-1);

bool IsBlank(char ch) {
    return ch == ' ' || ch == '\t';
}

std::string_view TrimBytes(std::string_view text) {
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

bool NamesEqual(std::string_view lhs, std::string_view rhs) {
    const std::wstring wideLhs = CodePageToWide(lhs, CP_UTF8);
    const std::wstring wideRhs = CodePageToWide(rhs, CP_UTF8);
    if (wideLhs.size() != wideRhs.size()) {
        return false;
    }
    if (wideLhs.empty()) {
        return true;
    }
    return CompareStringOrdinal(wideLhs.data(), static_cast<int>(wideLhs.size()), wideRhs.data(),
                                static_cast<int>(wideRhs.size()), TRUE) == CSTR_EQUAL;
}

// The grammar of config_ini_reader.cpp (ParseLine) on the raw bytes. Every structural
// character of the grammar is ASCII, so its byte positions equal its UTF-16 positions.
enum class LineKind { kBlank, kComment, kHeader, kEntry, kBare };

struct LineInfo {
    LineKind kind = LineKind::kBlank;
    std::string_view name;    // header: the section name; entry: the key (both trimmed)
    size_t equals = kNone;    // entry: byte index of the first '=' within the line text
};

LineInfo Classify(std::string_view text) {
    LineInfo info;
    const std::string_view line = TrimBytes(text);
    if (line.empty()) {
        return info;
    }
    if (line.front() == ';') {
        info.kind = LineKind::kComment;
        return info;
    }
    if (line.front() == '[') {
        std::string_view name = line.substr(1);
        const size_t close = name.find(']');
        if (close != std::string_view::npos) {
            name = name.substr(0, close);
        }
        info.kind = LineKind::kHeader;
        info.name = TrimBytes(name);
        return info;
    }
    const size_t equals = line.find('=');
    if (equals == std::string_view::npos) {
        info.kind = LineKind::kBare;
        return info;
    }
    info.kind = LineKind::kEntry;
    info.name = TrimBytes(line.substr(0, equals));
    info.equals = static_cast<size_t>(line.data() - text.data()) + equals;
    return info;
}

bool HasForbiddenByte(std::string_view text) {
    return text.find_first_of(std::string_view("\r\n\0", 3)) != std::string_view::npos;
}

bool IsValidSectionName(std::string_view name) {
    return !HasForbiddenByte(name) && name.find(']') == std::string_view::npos && IsValidUtf8(name);
}

bool IsValidKeyName(std::string_view key) {
    return !key.empty() && !HasForbiddenByte(key) && key.find('=') == std::string_view::npos &&
           key.front() != ';' && key.front() != '[' && IsValidUtf8(key);
}

bool IsValidValueText(std::string_view value) {
    return !HasForbiddenByte(value) && IsValidUtf8(value);
}

// The lines a section name addresses: its header and the body up to the next header.
struct SectionSpan {
    bool found = false;
    size_t header = kNone;      // kNone for the lines before the first header
    size_t bodyBegin = 0;
    size_t bodyEnd = 0;         // one past the last body line
    size_t lastContent = kNone; // last entry or bare line of the body
};

SectionSpan LocateSection(const std::vector<IniSourceLine>& lines, std::string_view section) {
    SectionSpan span;
    const bool global = section.empty();
    size_t begin = 0;
    if (!global) {
        for (size_t i = 0; i < lines.size(); ++i) {
            const LineInfo info = Classify(lines[i].text);
            if (info.kind == LineKind::kHeader && NamesEqual(info.name, section)) {
                span.header = i;
                begin = i + 1;
                break;
            }
        }
        if (span.header == kNone) {
            return span;
        }
    }
    span.found = true;
    span.bodyBegin = begin;
    span.bodyEnd = begin;
    for (size_t i = begin; i < lines.size(); ++i) {
        const LineInfo info = Classify(lines[i].text);
        if (info.kind == LineKind::kHeader) {
            break;
        }
        if (info.kind == LineKind::kEntry || info.kind == LineKind::kBare) {
            span.lastContent = i;
        }
        span.bodyEnd = i + 1;
    }
    return span;
}

// The terminator new lines use: the first one the text has, CRLF otherwise (config.ini is
// written on Windows).
std::string DominantEol(const IniDocument& document) {
    for (const IniSourceLine& line : document.sourceLines) {
        if (!line.eol.empty()) {
            return line.eol;
        }
    }
    return "\r\n";
}

void InsertLineAt(IniDocument* document, size_t index, std::string text) {
    std::vector<IniSourceLine>& lines = document->sourceLines;
    const std::string dominant = DominantEol(*document);
    std::string newEol = dominant;
    if (index > 0) {
        if (lines[index - 1].eol.empty()) {
            // Inserting after an unterminated last line: it needs a terminator now, and the new
            // last line stays unterminated like the text did.
            lines[index - 1].eol = dominant;
            newEol.clear();
        } else {
            newEol = lines[index - 1].eol;
        }
    }
    lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(index), IniSourceLine{std::move(text), newEol});
}

void AppendSection(IniDocument* document, std::string_view section, std::string keyLine) {
    std::vector<IniSourceLine>& lines = document->sourceLines;
    const std::string eol = DominantEol(*document);
    bool unterminatedTail = false;
    if (!lines.empty()) {
        if (lines.back().eol.empty()) {
            lines.back().eol = eol;
            unterminatedTail = true;
        }
        if (Classify(lines.back().text).kind != LineKind::kBlank) {
            lines.push_back(IniSourceLine{std::string(), eol});
        }
    }
    lines.push_back(IniSourceLine{"[" + std::string(section) + "]", eol});
    lines.push_back(IniSourceLine{std::move(keyLine), unterminatedTail ? std::string() : eol});
}

}  // namespace

IniEditStatus SetIniValue(IniDocument* document, std::string_view section, std::string_view key,
                          std::string_view value) {
    section = TrimBytes(section);
    key = TrimBytes(key);
    value = TrimBytes(value);
    if (!IsValidSectionName(section)) {
        return IniEditStatus::kInvalidSection;
    }
    if (!IsValidKeyName(key)) {
        return IniEditStatus::kInvalidKey;
    }
    if (!IsValidValueText(value)) {
        return IniEditStatus::kInvalidValue;
    }

    std::vector<IniSourceLine>& lines = document->sourceLines;
    const SectionSpan span = LocateSection(lines, section);
    IniEditStatus status = IniEditStatus::kChanged;
    if (span.found) {
        for (size_t i = span.bodyBegin; i < span.bodyEnd; ++i) {
            std::string& text = lines[i].text;
            const LineInfo info = Classify(text);
            if (info.kind != LineKind::kEntry || !NamesEqual(info.name, key)) {
                continue;
            }
            if (TrimBytes(std::string_view(text).substr(info.equals + 1)) == value) {
                return IniEditStatus::kUnchanged;
            }
            size_t keep = info.equals + 1;  // up to and including '=', and the blanks after it
            if (!value.empty()) {
                while (keep < text.size() && IsBlank(text[keep])) {
                    ++keep;
                }
            }
            text = text.substr(0, keep) + std::string(value);
            RebuildIniSections(document);
            return status;
        }
        size_t after = 0;
        if (span.lastContent != kNone) {
            after = span.lastContent + 1;
        } else if (span.header != kNone) {
            after = span.header + 1;
        }
        InsertLineAt(document, after, std::string(key) + "=" + std::string(value));
    } else {
        AppendSection(document, section, std::string(key) + "=" + std::string(value));
    }
    RebuildIniSections(document);
    return status;
}

IniEditStatus RemoveIniValue(IniDocument* document, std::string_view section, std::string_view key) {
    section = TrimBytes(section);
    key = TrimBytes(key);
    if (!IsValidSectionName(section)) {
        return IniEditStatus::kInvalidSection;
    }
    if (!IsValidKeyName(key)) {
        return IniEditStatus::kInvalidKey;
    }

    std::vector<IniSourceLine>& lines = document->sourceLines;
    const SectionSpan span = LocateSection(lines, section);
    if (!span.found) {
        return IniEditStatus::kNotFound;
    }
    bool removed = false;
    for (size_t i = span.bodyEnd; i > span.bodyBegin; --i) {
        const size_t index = i - 1;
        const LineInfo info = Classify(lines[index].text);
        if (info.kind == LineKind::kEntry && NamesEqual(info.name, key)) {
            lines.erase(lines.begin() + static_cast<std::ptrdiff_t>(index));
            removed = true;
        }
    }
    if (!removed) {
        return IniEditStatus::kNotFound;
    }
    RebuildIniSections(document);
    return IniEditStatus::kChanged;
}

}  // namespace ce::config_text
