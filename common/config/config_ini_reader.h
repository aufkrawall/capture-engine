#pragma once

// config.ini reader for UTF-8 files.
//
// The ANSI profile API (GetPrivateProfileStringA) reads a file without a UTF-16
// BOM as active-code-page text: kernel32 decodes it to UTF-16 with CP_ACP and
// the A entry point encodes the answer back with CP_ACP. On a single-byte code
// page (1252) that round trip returns a UTF-8 file's bytes unchanged, which is
// what config_text_encoding relied on. On a double-byte code page it does not:
// UTF-8 continuation bytes pair up as DBCS lead/trail bytes, invalid pairs
// decode to a default character, and the bytes that come back are no longer the
// file's. "日本語テスト" does not survive cp932, "ゲーム" does not survive cp936,
// and on cp949/cp950 neither survives - the value is lost before CE sees it.
//
// A UTF-8 file is therefore parsed here from its own bytes, and only the answer
// is converted to the code page the ANSI consumers expect. The line grammar
// mirrors GetPrivateProfileString, measured against the real API:
//   - lines end at CR LF, LF or a lone CR; spaces and tabs around a line, a
//     key, a value and a requested name are trimmed;
//   - a line starting with ';' is a comment ('#' is not);
//   - "[name" starts a section; the name runs to the first ']' (or the end of
//     the line) and anything after it is ignored;
//   - "key=value" splits at the first '='; a line without '=' is no key, but
//     GetPrivateProfileSection still lists it; lines before any section are
//     unreachable;
//   - section and key names compare case-insensitively; the FIRST section of a
//     given name is the only one searched, and its first matching key wins;
//   - a looked-up value enclosed in a matching pair of '"' or '\'' loses the
//     pair (a one-character value is left alone); a section listing keeps it.
// One deliberate difference: a UTF-8 BOM is skipped. The real API treats it as
// part of the first line, so the first section of a BOM-carrying config.ini
// silently vanished.
//
// Section and key arguments, and every returned string, are text in the given
// code page (the active code page in production, injectable for tests).

#include <string>
#include <string_view>
#include <vector>

namespace ce::config_text {

struct IniLine {
    std::wstring key;    // trimmed; for a line without '=' the whole trimmed line
    std::wstring value;  // trimmed, quotes kept
    bool hasEquals = false;
};

struct IniSection {
    std::wstring name;
    std::vector<IniLine> lines;
};

struct IniDocument {
    std::vector<IniSection> sections;
};

// UTF-8 (BOM optional) text to a document. Invalid UTF-8 decodes to U+FFFD.
IniDocument ParseUtf8Ini(std::string_view bytes);

// GetPrivateProfileString over `document`: true and `*value` when the key
// exists. `*lossy` reports characters `codePage` cannot represent ('?').
bool LookupIniValue(const IniDocument& document, std::string_view section, std::string_view key,
                    unsigned codePage, std::string* value, bool* lossy = nullptr);

// GetPrivateProfileSectionNames: every section header in file order.
std::vector<std::string> IniSectionNames(const IniDocument& document, unsigned codePage);

// GetPrivateProfileSection: the first matching section's lines as "key=value"
// (or the bare line), false when the section does not exist.
bool IniSectionLines(const IniDocument& document, std::string_view section, unsigned codePage,
                     std::vector<std::string>* lines);

// Text conversions at the code-page boundary.
std::wstring CodePageToWide(std::string_view text, unsigned codePage);
std::string WideToCodePage(std::wstring_view text, unsigned codePage, bool* lossy = nullptr);

}  // namespace ce::config_text
