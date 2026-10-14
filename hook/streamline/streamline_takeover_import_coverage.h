#pragma once

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <string>
#include <vector>

namespace ce::streamline_interposer_takeover {

// What the game's modules import from sl.interposer.dll, measured against the
// takeover's thunk table and the configured interposer's export table.
//
// sl.interposer.dll 2.11 exports 262 names (the sl* API plus D3D11, D3D12,
// DXGI and some 200 Vulkan proxies); the takeover gates 50 of them with
// thunks. The other imports are not thunked - a generic thunk cannot forward
// floating-point or arbitrarily many arguments - but they are rebound straight
// to the configured interposer when the takeover activates, so every import
// moves at once. That requires every imported name to exist in the configured
// copy (a same-major but older build may lack some) and to be a name at all:
// ordinals differ between builds, and a descriptor without a lookup table only
// holds resolved addresses. Anything else would split one Streamline runtime
// across two interposers, so the takeover is declined instead.
struct ImportCoverage {
    size_t named = 0;
    size_t thunked = 0;
    size_t ordinal = 0;
    size_t unnamed = 0;
    size_t missingFromOverride = 0;
    bool malformed = false;
    bool overrideExportsKnown = false;
    std::string firstMissing;
};

enum class ImportCoverageVerdict {
    Complete,
    MalformedImports,
    OrdinalImports,
    UnnamedImports,
    OverrideExportsUnreadable,
    MissingOverrideExport,
};

inline const char* ImportCoverageVerdictName(ImportCoverageVerdict verdict) {
    switch (verdict) {
        case ImportCoverageVerdict::Complete:
            return "complete";
        case ImportCoverageVerdict::MalformedImports:
            return "malformed import table";
        case ImportCoverageVerdict::OrdinalImports:
            return "ordinal imports";
        case ImportCoverageVerdict::UnnamedImports:
            return "imports without a name table";
        case ImportCoverageVerdict::OverrideExportsUnreadable:
            return "override export table unreadable";
        case ImportCoverageVerdict::MissingOverrideExport:
            return "override lacks an imported export";
    }
    return "unknown";
}

// `sortedOverrideExports` must be sorted (std::sort) for the binary search.
inline void RecordNamedImport(ImportCoverage* coverage, const char* name, bool thunked,
                              const std::vector<std::string>& sortedOverrideExports) {
    ++coverage->named;
    if (thunked) {
        ++coverage->thunked;
    }
    if (coverage->overrideExportsKnown &&
        !std::binary_search(sortedOverrideExports.begin(), sortedOverrideExports.end(), std::string(name))) {
        if (coverage->missingFromOverride++ == 0) {
            coverage->firstMissing = name;
        }
    }
}

inline ImportCoverageVerdict DecideImportCoverage(const ImportCoverage& coverage) {
    if (coverage.malformed) {
        return ImportCoverageVerdict::MalformedImports;
    }
    if (coverage.ordinal != 0) {
        return ImportCoverageVerdict::OrdinalImports;
    }
    if (coverage.unnamed != 0) {
        return ImportCoverageVerdict::UnnamedImports;
    }
    if (!coverage.overrideExportsKnown) {
        return ImportCoverageVerdict::OverrideExportsUnreadable;
    }
    if (coverage.missingFromOverride != 0) {
        return ImportCoverageVerdict::MissingOverrideExport;
    }
    return ImportCoverageVerdict::Complete;
}

}  // namespace ce::streamline_interposer_takeover
