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
// UTF-8) is left exactly as before. Section names are not converted: they are
// passed back to the same profile API, which must see the file's own bytes.

#include <string>
#include <string_view>

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

// The value as the ANSI-based consumers expect it. Reads (and caches, by file
// time and size) whether `path` is a UTF-8 file.
std::string ConfigValueToNative(const std::string& path, std::string value);

// GetPrivateProfileStringA plus ConfigValueToNative.
std::string ReadIniValue(const std::string& path, const char* section, const char* key, const char* defaultValue);

}  // namespace ce::config_text
