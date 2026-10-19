#pragma once

// Where one configuration load reads from.
//
// LoadConfig(path) and LoadConfigFromDocument(document) share one loader. The
// loader asks a ConfigSource for values, section names, section lines and the raw
// text; the source answers from a file or from an already parsed document:
//   - ForPath keeps the behavior LoadConfig always had: a UTF-8 file is answered
//     from the cached parse of its own bytes, an ANSI file goes through the Win32
//     profile API (config_text_encoding.h).
//   - ForDocument answers from the given IniDocument with the UTF-8 semantics of
//     config_ini_reader.h, converted to the active code page like every other
//     value the loader hands out.

#include <string>
#include <vector>

#include "common/config/config_ini_reader.h"

class ConfigSource {
public:
    static ConfigSource ForPath(std::string path);
    // `document` must outlive the source (and come from ParseUtf8Ini or an edit of one).
    static ConfigSource ForDocument(const ce::config_text::IniDocument& document);

    // GetPrivateProfileString semantics (first matching section, first matching key,
    // matching quote pair stripped, trailing blanks of the default dropped), answered in
    // the active code page.
    std::string ReadValue(const char* section, const char* key, const char* defaultValue) const;
    // Every section header in file order, in the active code page.
    std::vector<std::string> SectionNames() const;
    // GetPrivateProfileSection: the first matching section's lines ("key=value", quotes kept).
    bool SectionLines(const std::string& section, std::vector<std::string>* lines) const;
    // The whole text for the loader's line-oriented passes (the whitelist lists, which
    // continue over several lines). False when a file cannot be read.
    bool RawText(std::string* text) const;

private:
    ConfigSource() = default;

    std::string path_;
    const ce::config_text::IniDocument* document_ = nullptr;
};
