#pragma once

// Build identity exchanged between package-private binaries that ship and change together
// (captureengine.exe loading mediaengine.dll). A binary built from another build has other struct
// layouts and call semantics behind the same export names, so the loader compares identities
// before it resolves or calls anything else (llm-wiki/library/08 DR-12, Q9). The identity comes
// from the generated common/build_version.h through build_identity.h.

#include <cstddef>
#include <cstdint>
#include <string>

namespace ce::build_identity {

inline constexpr size_t kTextChars = 32;

struct Record {
    uint32_t size;  // set by the caller to sizeof(Record); the export refuses any other value
    uint32_t buildNumber;
    char version[kTextChars];
    char timestamp[kTextChars];
};
static_assert(sizeof(Record) == 72, "Record is part of the package-private binary contract");

// Signature of the identity export a package-private DLL provides.
using ExportFn = bool (*)(Record*);

// The identity export of mediaengine.dll (mediaengine/engine/mediaengine.h); both of its loaders name it.
inline constexpr const char* kMediaEngineExportName = "MediaEngine_GetBuildIdentity";

enum class Verdict : uint8_t {
    Match,
    ExportMissing,   // the module has no identity export (a build from before the check existed)
    ExportRejected,  // the export refused the request (another record layout)
    SizeMismatch,    // the export answered with a record of another size
    BuildNumberMismatch,
    VersionMismatch,
    TimestampMismatch,
};

// An empty record that asks for this build's layout.
inline Record Request() noexcept {
    Record record{};
    record.size = sizeof(Record);
    return record;
}

// This binary's identity: the generated build number, version and timestamp it was compiled with.
Record Current() noexcept;

// Body of an identity export: fills `out` with Current() when out->size is this build's record size.
bool Publish(Record* out) noexcept;

// Pure comparison. Text that is not NUL-terminated inside its field never matches.
Verdict Compare(const Record& host, const Record& module) noexcept;

// Calls the module's export (null = missing), stores what it answered in `module` when given, and
// compares it with `host`.
Verdict CheckExport(ExportFn exportFn, const Record& host, Record* module = nullptr) noexcept;

const char* VerdictName(Verdict verdict) noexcept;

// "0.1.7277 (build 7277, built 2026-10-19 21:40:42)" for one log line; bounded and safe on any bytes.
std::string Describe(const Record& record);

}  // namespace ce::build_identity
