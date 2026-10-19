#pragma once

// Unknown-key detection by lookup tracking.
//
// The loader is the only authority on which keys exist: a key is known when some loader
// path reads it. Instead of a second, hand-written list (which would rot as loaders change),
// a load run with a diagnostics sink records every section/key pair the loader looked up
// (ConfigSource::ReadValue) or declared as readable (ConfigSource::Declare), and afterwards
// every key the document contains that was never looked up is reported.
//
// A pure trace would still give false positives, because loaders read some keys only under
// conditions. These are handled where the condition lives, not by a schema:
//   - alternatives: GetStrCompat / GetStrCompat2 declare every location they can read, even
//     when the first one answered (a canonical key and its legacy spelling may both be present);
//   - gated groups: a group read only when another key allows it (the [Streaming] fields behind
//     enabled=true, the legacy [General] logging keys behind a canonical log_level, the
//     line-oriented whitelist and process_list passes) declares its keys before the gate;
//   - profile sections: any "Section.key" or bare "key" that the loader reads from a global
//     section is a valid override in an [Profile.*] / [App.*] section, whether or not this load's
//     process matched that profile, so profile keys are checked against the set of global pairs.
// A numbered group whose enabling key is missing ([Microphone.2] without enabled=) is not read
// at all; its keys are reported, which is accurate: nothing uses them.

#include <set>
#include <string>
#include <string_view>
#include <utility>

#include "config.h"

class ConfigSource;

class ConfigLookupTracker {
public:
    // A pair some loader path reads. Names are active-code-page text, as the loader passes them.
    void Note(const char* section, const char* key);
    // A key that a line-oriented pass accepts in any section (wgc_window_detection).
    void NoteAnySection(const char* key);

    bool Knows(const std::wstring& section, const std::wstring& key) const;
    bool KnowsSection(const std::wstring& section) const;
    bool KnowsAnySection(const std::wstring& key) const;
    // True when `key` is "<Section>.<key>" or "<key>" of a pair a global (non-profile) section reads.
    bool KnowsAsProfileOverride(const std::wstring& key) const;

private:
    std::set<std::pair<std::wstring, std::wstring>> pairs_;  // canonical section, canonical key
    std::set<std::wstring> sections_;
    std::set<std::wstring> anySectionKeys_;
    // The "Section.key" and bare "key" spellings of the global pairs, derived on first use.
    mutable std::set<std::wstring> overrideKeys_;
    mutable bool overrideKeysBuilt_ = false;
};

// The case-insensitive form names are compared in: what a lookup would match, folded the way the
// loader's code-page round trip folds names it cannot represent.
std::wstring CanonicalConfigName(std::wstring_view name);

// True for the section names the loader resolves into application profiles.
bool IsProfileSectionName(const std::wstring& canonicalSection);

// Reports every key and section of the source that `tracker` never saw as an unknown-key /
// unknown-section diagnostic to the sink of this thread, plus one capped summary line in the log.
// `config` supplies the profiles that resolved: the keys of a profile section the loader rejected
// are not reported on top of the profile warning.
void ReportUnknownConfigKeys(const ConfigSource& source, const ConfigLookupTracker& tracker, const AppConfig& config);
