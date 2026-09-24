#pragma once

// config.ini text encoding.
//
// config.ini is created from a UTF-8 template, and current Windows editors
// (Notepad since Windows 10 1903) save UTF-8. The loader reads it through the
// ANSI profile API, and the rest of CaptureEngine treats configuration strings
// as active-code-page text (std::filesystem::path(std::string), the ANSI file
// APIs, process and window names). A non-ASCII value in a UTF-8 file therefore
// reached those consumers as mojibake: output_dir=D:\Aufnahmen\Spiele Ü
// created (or failed to create) a differently named folder, and profile
// process or window names never matched.
//
// Values read from a UTF-8 config are converted to the active code page at the
// read boundary. An ANSI-saved config (whose non-ASCII bytes are not valid
// UTF-8) is left exactly as before. Section and key names travel in the active
// code page too: the ones ReadIniSectionNames returns are what ReadIniValue
// expects back.

#include <string>
#include <string_view>
#include <vector>

namespace ce::config_text {

bool HasUtf8Bom(std::string_view bytes);
bool ContainsNonAscii(std::string_view bytes);
// Strict UTF-8: rejects overlong forms, surrogates, code points above U+10FFFF
// and truncated sequences.
bool IsValidUtf8(std::string_view bytes);
// A file is UTF-8 when it carries the BOM, or when it has non-ASCII bytes and
// all of them form valid UTF-8 (ANSI text practically never does).
bool IsUtf8ConfigText(std::string_view fileBytes);

// Converts UTF-8 to `codePage`. Characters the code page cannot represent
// become '?'; *lossy reports whether that happened.
std::string Utf8ToCodePage(std::string_view utf8, unsigned codePage, bool* lossy = nullptr);

// GetPrivateProfileStringA semantics, answered in the active code page. A UTF-8
// file (cached parse, keyed by file time and size) is read from its own bytes
// by config_ini_reader: the profile API's CP_ACP round trip is lossless only on
// single-byte code pages and mangles UTF-8 on Japanese, Chinese and Korean
// systems. An ANSI file still goes through the profile API unchanged.
std::string ReadIniValue(const std::string& path, const char* section, const char* key, const char* defaultValue);
// ReadIniValue with the code page injected (tests force a DBCS code page).
std::string ReadIniValueForCodePage(const std::string& path, const char* section, const char* key,
                                    const char* defaultValue, unsigned codePage);
// GetPrivateProfileSectionNamesA / GetPrivateProfileSectionA equivalents over
// the same UTF-8-aware path.
std::vector<std::string> ReadIniSectionNames(const std::string& path);
bool ReadIniSectionLines(const std::string& path, const std::string& section, std::vector<std::string>* lines);

}  // namespace ce::config_text
