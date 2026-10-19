#pragma once

// Structured warnings of one configuration load.
//
// The loader has always logged its warnings (invalid values that fall back to a
// documented default, profile conflicts, deprecated keys). A caller that wants to
// show them - the settings API validating a candidate document, a test - passes a
// ConfigDiagnostics to LoadConfig / LoadConfigFromDocument and receives each of
// those warnings as an entry as well. The log output is unchanged either way: the
// sink is an additional destination, never a replacement.
//
// Section, key and message are UTF-8. The loader works in the active code page
// internally, so a value the code page cannot represent shows up as '?' here too
// (the same limit as the log lines; see config_ini_reader.h).

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

enum class ConfigDiagnosticSeverity : uint8_t {
    kWarning = 1,  // values mirror CE_DIAG_WARNING / CE_DIAG_ERROR of the planned settings API
    kError = 2,    // never produced by the loader itself; a caller may promote a kind (for example kInvalidValue)
};

enum class ConfigDiagnosticKind : uint8_t {
    kInvalidValue,    // a value was rejected or out of range; the documented default is used
    kProfile,         // application profile resolution (overlaps, impossible routes)
    kNotice,          // other loader warnings (deprecated or conflicting keys, lossy text)
    kUnknownKey,      // a key is present but no setting reads it (misspelled, retired or in an inactive group)
    kUnknownSection,  // a section is present but no setting reads anything from it (key is empty)
};

struct ConfigDiagnostic {
    ConfigDiagnosticSeverity severity = ConfigDiagnosticSeverity::kWarning;
    ConfigDiagnosticKind kind = ConfigDiagnosticKind::kNotice;
    std::string section;  // empty when the warning is not tied to a section
    std::string key;      // empty when the warning is not tied to a key
    std::string message;
};

struct ConfigDiagnostics {
    // A hand-edited file can raise a warning per line; the sink keeps the first entries
    // and counts the rest, so a hostile document cannot grow it without bound.
    static constexpr size_t kMaxEntries = 256;

    std::vector<ConfigDiagnostic> entries;
    size_t dropped = 0;  // warnings beyond kMaxEntries

    // Also report the keys and sections no loader path reads (config_unknown_keys.h). On by default
    // when a sink is given; a caller that only wants the value warnings turns it off to skip the pass.
    // Without a sink nothing is tracked.
    bool trackUnknownKeys = true;

    void Add(ConfigDiagnostic diagnostic) {
        if (entries.size() >= kMaxEntries) {
            ++dropped;
            return;
        }
        entries.push_back(std::move(diagnostic));
    }

    size_t Count(ConfigDiagnosticKind kind) const {
        size_t count = 0;
        for (const ConfigDiagnostic& entry : entries) {
            if (entry.kind == kind)
                ++count;
        }
        return count;
    }
};
