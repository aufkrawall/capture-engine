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

// One physical line of the source text, byte for byte: its content without the terminator and
// the terminator itself ("\r\n", "\n", "\r", or empty for a last line the text ends on).
struct IniSourceLine {
    std::string text;
    std::string eol;
};

// The parsed view (`sections`, what lookups search) and the text it came from. The source is
// kept so a document serializes back to its exact bytes and can be edited in place
// (config_ini_edit.h); `sections` is always derived from it (RebuildIniSections).
struct IniDocument {
    std::vector<IniSection> sections;
    bool hasBom = false;  // the text started with a UTF-8 BOM, which SerializeIni writes back
    std::vector<IniSourceLine> sourceLines;
};

// UTF-8 (BOM optional) text to a document. Invalid UTF-8 decodes to U+FFFD in `sections`;
// the source lines keep the bytes as they were.
IniDocument ParseUtf8Ini(std::string_view bytes);

// The document's text: the BOM (if it had one) and every source line with its terminator.
// ParseUtf8Ini followed by SerializeIni returns the input unchanged.
std::string SerializeIni(const IniDocument& document);

// Re-derives `sections` from `sourceLines` after the source lines were edited.
void RebuildIniSections(IniDocument* document);

// GetPrivateProfileString over `document`: true and `*value` when the key
// exists. `*lossy` reports characters `codePage` cannot represent ('?').
bool LookupIniValue(const IniDocument& document, std::string_view section, std::string_view key,
                    unsigned codePage, std::string* value, bool* lossy = nullptr);
// Wide-name form for callers that need a different encoding for the returned
// value (for example active-code-page section names with a UTF-8 process path).
bool LookupIniValueWide(const IniDocument& document, std::wstring_view section, std::wstring_view key,
                        unsigned valueCodePage, std::string* value, bool* lossy = nullptr);

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
