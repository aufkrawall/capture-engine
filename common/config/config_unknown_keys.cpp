#include "config_unknown_keys.h"

#include <windows.h>

#include <cstdint>
#include <vector>

#include "common/logging/log_meter.h"
#include "config_internal.h"
#include "config_source.h"

namespace {

bool IsAscii(std::wstring_view text) {
    for (const wchar_t ch : text) {
        if (ch >= 0x80)
            return false;
    }
    return true;
}

std::string ToUtf8(std::wstring_view text) {
    return ce::config_text::WideToCodePage(text, CP_UTF8);
}

}  // namespace

std::wstring CanonicalConfigName(std::wstring_view name) {
    std::wstring folded(name);
    if (!IsAscii(folded)) {
        // The loader looks names up through the active code page; a name that page cannot represent
        // reaches it as '?' characters. Fold the present names the same way so they can match.
        folded = ce::config_text::CodePageToWide(ce::config_text::WideToCodePage(folded, GetACP()), GetACP());
    }
    if (!folded.empty()) {
        CharUpperBuffW(folded.data(), static_cast<DWORD>(folded.size()));
    }
    return folded;
}

bool IsProfileSectionName(const std::wstring& canonicalSection) {
    auto hasPrefix = [&](std::wstring_view prefix) {
        return canonicalSection.size() > prefix.size() && canonicalSection.compare(0, prefix.size(), prefix) == 0;
    };
    return hasPrefix(L"PROFILE.") || hasPrefix(L"APP.");
}

void ConfigLookupTracker::Note(const char* section, const char* key) {
    const std::wstring canonicalSection =
        CanonicalConfigName(ce::config_text::CodePageToWide(section ? section : "", GetACP()));
    const std::wstring canonicalKey = CanonicalConfigName(ce::config_text::CodePageToWide(key ? key : "", GetACP()));
    sections_.insert(canonicalSection);
    if (pairs_.emplace(canonicalSection, canonicalKey).second) {
        overrideKeysBuilt_ = false;
    }
}

void ConfigLookupTracker::NoteAnySection(const char* key) {
    anySectionKeys_.insert(CanonicalConfigName(ce::config_text::CodePageToWide(key ? key : "", GetACP())));
}

bool ConfigLookupTracker::Knows(const std::wstring& section, const std::wstring& key) const {
    return pairs_.find({section, key}) != pairs_.end();
}

bool ConfigLookupTracker::KnowsSection(const std::wstring& section) const {
    return sections_.find(section) != sections_.end();
}

bool ConfigLookupTracker::KnowsAnySection(const std::wstring& key) const {
    return anySectionKeys_.find(key) != anySectionKeys_.end();
}

bool ConfigLookupTracker::KnowsAsProfileOverride(const std::wstring& key) const {
    if (!overrideKeysBuilt_) {
        // Built once per query phase: a document with thousands of profile keys must not rescan
        // every known pair for each of them.
        overrideKeys_.clear();
        for (const auto& [section, name] : pairs_) {
            if (IsProfileSectionName(section))
                continue;
            overrideKeys_.insert(name);
            std::wstring qualified(section);
            qualified.append(L".").append(name);
            overrideKeys_.insert(std::move(qualified));
        }
        overrideKeysBuilt_ = true;
    }
    return overrideKeys_.find(key) != overrideKeys_.end();
}

namespace {

struct UnknownEntry {
    std::wstring section;
    std::wstring key;  // empty: the whole section is unknown
};

constexpr size_t kLoggedUnknownEntries = 8;

uint64_t HashText(const std::string& text) {
    uint64_t hash = 14695981039346656037ull;
    for (const unsigned char ch : text) {
        hash ^= ch;
        hash *= 1099511628211ull;
    }
    return hash;
}

void LogUnknownSummary(const std::vector<UnknownEntry>& unknown) {
    if (unknown.empty() || !Log_IsEnabled(LogLevel::Warn)) {
        return;
    }
    std::string list;
    for (size_t i = 0; i < unknown.size() && i < kLoggedUnknownEntries; ++i) {
        if (i != 0)
            list += ", ";
        list += "[" + ToUtf8(unknown[i].section) + "]";
        list += unknown[i].key.empty() ? " (section)" : " " + ToUtf8(unknown[i].key);
    }
    if (unknown.size() > kLoggedUnknownEntries)
        list += ", ... (+" + std::to_string(unknown.size() - kLoggedUnknownEntries) + " more)";
    // A reload that finds the same typo again is one line, not one per load.
    static ce::log_meter::ChangeGate gate;
    const ce::log_meter::ChangeGate::Verdict verdict = gate.Observe(HashText(list));
    if (verdict.log) {
        LogWarn("Config: %zu entr%s no setting reads (misspelled, retired or unused): %s%s", unknown.size(),
                unknown.size() == 1 ? "y" : "ies", list.c_str(),
                ce::log_meter::SuppressedNote(verdict.suppressed).c_str());
    }
}

}  // namespace

void ReportUnknownConfigKeys(const ConfigSource& source, const ConfigLookupTracker& tracker, const AppConfig& config) {
    std::set<std::wstring> resolvedProfiles;
    for (const ApplicationProfile& profile : config.applicationProfiles) {
        resolvedProfiles.insert(
            CanonicalConfigName(ce::config_text::CodePageToWide(profile.section, GetACP())));
    }

    std::vector<UnknownEntry> unknown;
    std::set<std::pair<std::wstring, std::wstring>> reported;
    for (const ConfigPresentKey& present : source.PresentKeys()) {
        const std::wstring section = CanonicalConfigName(present.section);
        const std::wstring key = CanonicalConfigName(present.key);
        if (tracker.Knows(section, key) || tracker.KnowsAnySection(key)) {
            continue;
        }
        if (IsProfileSectionName(section)) {
            // A profile the loader rejected already has its own warning; a resolved profile accepts
            // the "Section.key" / bare-key override of any global setting.
            if (resolvedProfiles.find(section) == resolvedProfiles.end() || tracker.KnowsAsProfileOverride(key)) {
                continue;
            }
        } else if (!tracker.KnowsSection(section)) {
            if (reported.emplace(section, std::wstring()).second)
                unknown.push_back({present.section, std::wstring()});
            continue;
        }
        if (reported.emplace(section, key).second)
            unknown.push_back({present.section, present.key});
    }

    for (const UnknownEntry& entry : unknown) {
        if (entry.key.empty()) {
            AddConfigDiagnosticUtf8(ConfigDiagnosticKind::kUnknownSection, ToUtf8(entry.section), std::string(),
                                    "unknown section: no setting reads anything from it");
        } else {
            AddConfigDiagnosticUtf8(ConfigDiagnosticKind::kUnknownKey, ToUtf8(entry.section), ToUtf8(entry.key),
                                    "unknown key: no setting reads it (misspelled, retired or unused)");
        }
    }
    LogUnknownSummary(unknown);
}
