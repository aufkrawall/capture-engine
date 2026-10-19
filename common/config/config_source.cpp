#include "config_source.h"

#include <set>

#include "config_internal.h"
#include "config_text_encoding.h"
#include "config_unknown_keys.h"

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
    Note(section, key);
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

void ConfigSource::Note(const char* section, const char* key) const {
    if (tracker_) {
        tracker_->Note(section, key);
    }
}

void ConfigSource::DeclareAnySection(const char* key) const {
    if (tracker_) {
        tracker_->NoteAnySection(key);
    }
}

namespace {

void AppendKeys(const ce::config_text::IniDocument& document, std::vector<ConfigPresentKey>* keys) {
    for (const ce::config_text::IniSection& section : document.sections) {
        for (const ce::config_text::IniLine& line : section.lines) {
            if (line.hasEquals && !line.key.empty()) {
                keys->push_back({section.name, line.key});
            }
        }
    }
}

}  // namespace

std::vector<ConfigPresentKey> ConfigSource::PresentKeys() const {
    std::vector<ConfigPresentKey> keys;
    if (document_) {
        AppendKeys(*document_, &keys);
        return keys;
    }
    if (const std::shared_ptr<const ce::config_text::IniDocument> utf8 = ce::config_text::ReadUtf8ConfigDocument(path_)) {
        AppendKeys(*utf8, &keys);
        return keys;
    }
    // An ANSI file has no document: enumerate it through the same profile API the loader reads it with.
    std::set<std::string> visited;
    for (const std::string& section : SectionNames()) {
        if (!visited.insert(section).second) {
            continue;
        }
        std::vector<std::string> lines;
        if (!SectionLines(section, &lines)) {
            continue;
        }
        const std::wstring wideSection = ce::config_text::CodePageToWide(section, GetACP());
        for (const std::string& line : lines) {
            const size_t equals = line.find('=');
            if (equals == std::string::npos) {
                continue;
            }
            const std::string key = Trim(line.substr(0, equals), " \t");
            if (!key.empty()) {
                keys.push_back({wideSection, ce::config_text::CodePageToWide(key, GetACP())});
            }
        }
    }
    return keys;
}
