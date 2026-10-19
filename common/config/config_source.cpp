#include "config_source.h"

#include "config_internal.h"
#include "config_text_encoding.h"

ConfigSource ConfigSource::ForPath(std::string path) {
    ConfigSource source;
    source.path_ = std::move(path);
    return source;
}

ConfigSource ConfigSource::ForDocument(const ce::config_text::IniDocument& document) {
    ConfigSource source;
    source.document_ = &document;
    return source;
}

std::string ConfigSource::ReadValue(const char* section, const char* key, const char* defaultValue) const {
    bool lossy = false;
    std::string value =
        document_ ? ce::config_text::ReadIniValueFromDocument(*document_, section, key, defaultValue, GetACP(), &lossy)
                  : ce::config_text::ReadIniValue(path_, section, key, defaultValue, &lossy);
    if (lossy) {
        // The log line is written where the value is converted (config_text_encoding.cpp);
        // the sink learns which setting it was.
        ReportConfigDiagnostic(ConfigDiagnosticKind::kNotice, section ? section : "", key ? key : "",
                               "the value contains characters the Windows code page cannot represent; they were "
                               "replaced with '?'");
    }
    return value;
}

std::vector<std::string> ConfigSource::SectionNames() const {
    return document_ ? ce::config_text::IniSectionNames(*document_, GetACP())
                     : ce::config_text::ReadIniSectionNames(path_);
}

bool ConfigSource::SectionLines(const std::string& section, std::vector<std::string>* lines) const {
    return document_ ? ce::config_text::IniSectionLines(*document_, section, GetACP(), lines)
                     : ce::config_text::ReadIniSectionLines(path_, section, lines);
}

bool ConfigSource::RawText(std::string* text) const {
    if (document_) {
        *text = ce::config_text::SerializeIni(*document_);
        return true;
    }
    return ReadTextFile(path_, *text);
}
