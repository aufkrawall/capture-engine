#pragma once

// The properties an in-place INI edit must keep, as one checker that two drivers share:
// tests/fuzz/fuzz_settings_edit.cpp feeds it libFuzzer inputs, tests/test_config_edit.cpp replays
// the committed seed corpus (and a deterministic sweep) through it in the ordinary unit gate.
//
// Input layout: [op count - 1] [4 bytes per op: kind, section, key, value] [document bytes].
// Section, key and value bytes pick from the names the document already has (so edits hit existing
// lines) and from a pool of tricky names and values, including ones an edit must reject.
//
// Properties, for every op of the sequence:
//   - a rejected or unchanged edit leaves every source line byte-identical;
//   - an edit touches at most one existing line (Set) or only removes lines (Remove): all other
//     lines, their terminators and the BOM stay byte-identical;
//   - the parsed sections always equal a fresh parse of the serialized text, and the serialized
//     text round-trips through ParseUtf8Ini unchanged;
//   - after Set, a lookup of the key returns the value (blanks trimmed, one quote pair stripped);
//     after Remove, a lookup finds nothing;
//   - no other key of the original document changes its lookup result;
//   - repeating a Set is reported as unchanged.

#include <cstdint>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <windows.h>

#include "common/config/config_ini_edit.h"
#include "common/config/config_ini_reader.h"

namespace ce::settings_edit_check {

inline constexpr size_t kMaxDocumentBytes = 8192;  // the checks are quadratic in the key count
inline constexpr size_t kMaxOps = 4;

inline const char* const kSectionPool[] = {"Video", "video", "Output",   "NewSection", "  Spaced  ",
                                           "Profile.Game", "\xE6\x97\xA5\xE6\x9C\xAC", "", "Bad]Name", "A\nB"};
inline const char* const kKeyPool[] = {"fps", "FPS", "codec", "output_dir", "new_key", "a b",
                                       "Video.fps", "bad=key", ";comment", "[x", ""};
inline const char* const kValuePool[] = {"",         " ",           "60",           "\"quoted value\"",
                                         "'single'", "a=b",         "x ; not a comment", "[bracket]",
                                         ";semi",    "\xE6\x97\xA5\xE6\x9C\xAC", "  padded  ", "\"",
                                         "trailing ", "C:\\Videos\\clips", "line\nbreak", "\xFF\xFE"};

template <typename T, size_t N>
constexpr size_t Count(const T (&)[N]) {
    return N;
}

inline std::string Utf8(std::wstring_view wide) {
    return config_text::WideToCodePage(wide, CP_UTF8);
}

inline bool FoldEqual(std::wstring_view lhs, std::wstring_view rhs) {
    if (lhs.size() != rhs.size())
        return false;
    if (lhs.empty())
        return true;
    return CompareStringOrdinal(lhs.data(), static_cast<int>(lhs.size()), rhs.data(), static_cast<int>(rhs.size()),
                                TRUE) == CSTR_EQUAL;
}

inline std::string ExpectedLookup(std::string_view value) {
    size_t begin = 0;
    size_t end = value.size();
    while (begin < end && (value[begin] == ' ' || value[begin] == '\t'))
        ++begin;
    while (end > begin && (value[end - 1] == ' ' || value[end - 1] == '\t'))
        --end;
    value = value.substr(begin, end - begin);
    if (value.size() >= 2 && (value.front() == '"' || value.front() == '\'') && value.back() == value.front())
        value = value.substr(1, value.size() - 2);
    return std::string(value);
}

inline bool SameLines(const std::vector<config_text::IniSourceLine>& lhs,
                      const std::vector<config_text::IniSourceLine>& rhs) {
    if (lhs.size() != rhs.size())
        return false;
    for (size_t i = 0; i < lhs.size(); ++i) {
        if (lhs[i].text != rhs[i].text || lhs[i].eol != rhs[i].eol)
            return false;
    }
    return true;
}

inline bool SameSections(const config_text::IniDocument& lhs, const config_text::IniDocument& rhs) {
    if (lhs.sections.size() != rhs.sections.size())
        return false;
    for (size_t s = 0; s < lhs.sections.size(); ++s) {
        const config_text::IniSection& a = lhs.sections[s];
        const config_text::IniSection& b = rhs.sections[s];
        if (a.name != b.name || a.lines.size() != b.lines.size())
            return false;
        for (size_t l = 0; l < a.lines.size(); ++l) {
            if (a.lines[l].key != b.lines[l].key || a.lines[l].value != b.lines[l].value ||
                a.lines[l].hasEquals != b.lines[l].hasEquals)
                return false;
        }
    }
    return true;
}

struct Lookup {
    bool found = false;
    std::string value;
};

inline Lookup LookupUtf8(const config_text::IniDocument& document, const std::string& section,
                         const std::string& key) {
    Lookup lookup;
    lookup.found = config_text::LookupIniValue(document, section, key, CP_UTF8, &lookup.value);
    return lookup;
}

// Returns "" when every property held, otherwise a description of the first violation.
inline std::string CheckEditSequence(const uint8_t* data, size_t size) {
    using config_text::IniEditStatus;
    if (size < 1 + 4)
        return {};
    const size_t opCount = 1 + data[0] % kMaxOps;
    const size_t opsEnd = 1 + 4 * opCount;
    if (size < opsEnd)
        return {};
    size_t documentSize = size - opsEnd;
    if (documentSize > kMaxDocumentBytes)
        documentSize = kMaxDocumentBytes;
    const std::string original(reinterpret_cast<const char*>(data + opsEnd), documentSize);

    config_text::IniDocument document = config_text::ParseUtf8Ini(original);
    if (config_text::SerializeIni(document) != original)
        return "ParseUtf8Ini/SerializeIni did not round-trip the input";

    for (size_t op = 0; op < opCount; ++op) {
        const uint8_t kind = data[1 + 4 * op] & 3;
        const uint8_t sectionSel = data[2 + 4 * op];
        const uint8_t keySel = data[3 + 4 * op];
        const uint8_t valueSel = data[4 + 4 * op];

        std::string section;
        const config_text::IniSection* existing = nullptr;
        const size_t sectionPick = sectionSel % (document.sections.size() + Count(kSectionPool));
        if (sectionPick < document.sections.size()) {
            existing = &document.sections[sectionPick];
            section = Utf8(existing->name);
        } else {
            section = kSectionPool[sectionPick - document.sections.size()];
        }
        std::string key;
        size_t keyCandidates = 0;
        if (existing) {
            for (const config_text::IniLine& line : existing->lines)
                keyCandidates += line.hasEquals ? 1 : 0;
        }
        const size_t keyPick = keySel % (keyCandidates + Count(kKeyPool));
        if (keyPick < keyCandidates) {
            size_t seen = 0;
            for (const config_text::IniLine& line : existing->lines) {
                if (line.hasEquals && seen++ == keyPick)
                    key = Utf8(line.key);
            }
        } else {
            key = kKeyPool[keyPick - keyCandidates];
        }
        const std::string value = kValuePool[valueSel % Count(kValuePool)];
        const bool remove = kind == 2;

        const std::vector<config_text::IniSourceLine> before = document.sourceLines;
        const bool hadBom = document.hasBom;
        const config_text::IniDocument beforeDocument = document;
        const IniEditStatus status = remove ? config_text::RemoveIniValue(&document, section, key)
                                            : config_text::SetIniValue(&document, section, key, value);
        const std::string where = "op " + std::to_string(op) + (remove ? " remove" : " set") + " [" + section +
                                  "] " + key + (remove ? "" : "=" + value) + ": ";

        if (document.hasBom != hadBom)
            return where + "the BOM flag changed";
        const bool changed = status == IniEditStatus::kChanged;
        if (!changed) {
            if (!SameLines(before, document.sourceLines))
                return where + "a rejected or unchanged edit altered the text";
        } else {
            if (SameLines(before, document.sourceLines))
                return where + "kChanged but the text is identical";
            if (remove) {
                // Only deletions: what is left is the original lines, in order.
                size_t next = 0;
                for (const config_text::IniSourceLine& line : document.sourceLines) {
                    while (next < before.size() && (before[next].text != line.text || before[next].eol != line.eol))
                        ++next;
                    if (next == before.size())
                        return where + "Remove changed a line it did not delete";
                    ++next;
                }
            } else {
                size_t prefix = 0;
                while (prefix < before.size() && prefix < document.sourceLines.size() &&
                       before[prefix].text == document.sourceLines[prefix].text &&
                       before[prefix].eol == document.sourceLines[prefix].eol)
                    ++prefix;
                size_t suffix = 0;
                while (suffix < before.size() - prefix && suffix < document.sourceLines.size() - prefix &&
                       before[before.size() - 1 - suffix].text ==
                           document.sourceLines[document.sourceLines.size() - 1 - suffix].text &&
                       before[before.size() - 1 - suffix].eol ==
                           document.sourceLines[document.sourceLines.size() - 1 - suffix].eol)
                    ++suffix;
                const size_t replaced = before.size() - prefix - suffix;
                const size_t written = document.sourceLines.size() - prefix - suffix;
                // One edited line, or the terminator of the last line plus a separator, the header
                // and the key line of a new section.
                if (replaced > 1 || written > 4)
                    return where + "Set rewrote " + std::to_string(replaced) + " lines into " +
                           std::to_string(written);
            }
        }

        const std::string serialized = config_text::SerializeIni(document);
        const config_text::IniDocument reparsed = config_text::ParseUtf8Ini(serialized);
        if (!SameSections(document, reparsed))
            return where + "the sections differ from a fresh parse of the text";
        if (config_text::SerializeIni(reparsed) != serialized)
            return where + "the edited text does not round-trip";

        const bool valid = status != IniEditStatus::kInvalidSection && status != IniEditStatus::kInvalidKey &&
                           status != IniEditStatus::kInvalidValue;
        // Section "" addresses the lines before the first header, which no lookup can reach.
        if (valid && !section.empty()) {
            const Lookup lookup = LookupUtf8(document, section, key);
            if (remove) {
                if (status != IniEditStatus::kNotFound && lookup.found)
                    return where + "the key is still found after Remove";
            } else {
                if (!lookup.found)
                    return where + "the key is not found after Set";
                if (lookup.value != ExpectedLookup(value))
                    return where + "Set stored '" + lookup.value + "'";
            }
        }
        if (!remove && changed) {
            const std::vector<config_text::IniSourceLine> afterFirst = document.sourceLines;
            if (config_text::SetIniValue(&document, section, key, value) != IniEditStatus::kUnchanged ||
                !SameLines(afterFirst, document.sourceLines))
                return where + "repeating the Set was not a no-op";
        }

        if (valid) {
            std::wstring editedSection = config_text::CodePageToWide(section, CP_UTF8);
            std::wstring editedKey = config_text::CodePageToWide(key, CP_UTF8);
            auto trimBlanks = [](std::wstring text) {
                const size_t begin = text.find_first_not_of(L" \t");
                if (begin == std::wstring::npos)
                    return std::wstring();
                return text.substr(begin, text.find_last_not_of(L" \t") - begin + 1);
            };
            editedSection = trimBlanks(editedSection);
            editedKey = trimBlanks(editedKey);
            for (const config_text::IniSection& other : beforeDocument.sections) {
                for (const config_text::IniLine& line : other.lines) {
                    if (!line.hasEquals)
                        continue;
                    if (FoldEqual(other.name, editedSection) && FoldEqual(line.key, editedKey))
                        continue;
                    const Lookup oldLookup = LookupUtf8(beforeDocument, Utf8(other.name), Utf8(line.key));
                    const Lookup newLookup = LookupUtf8(document, Utf8(other.name), Utf8(line.key));
                    if (oldLookup.found != newLookup.found || oldLookup.value != newLookup.value)
                        return where + "an unrelated key [" + Utf8(other.name) + "] " + Utf8(line.key) + " changed";
                }
            }
        }
    }
    return {};
}

}  // namespace ce::settings_edit_check
