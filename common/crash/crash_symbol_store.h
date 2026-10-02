#pragma once

// Session symbol archive without a per-session copy.
//
// Every session directory carries the installed binaries and PDBs so that a
// dump written there stays analyzable after CaptureEngine is updated. That used
// to be a full copy per session - about 180 MB, written twice (once into
// `symbols\` and once into `symbols\captureengine\`) on every start, and twenty
// sessions are kept - so an ordinary user accumulated gigabytes of identical
// PDBs and wrote 180 MB to disk per launch.
//
// The store keeps ONE copy of each artifact below the logs root, and every
// session entry is an NTFS hard link to it. A newer build replaces the store
// file by rename, which gives it a new file identity: sessions that linked the
// older build keep exactly the bytes they linked. A store file whose link count
// has fallen back to one is referenced by no retained session and is pruned.
// Where hard links are unavailable (FAT/exFAT, a different volume) the entry is
// copied exactly as before.

#include <filesystem>
#include <string>

namespace ce::crash_symbols {

inline constexpr const wchar_t* kSymbolStoreDirName = L"symbol_store";

struct PlaceResult {
    bool placed = false;       // destination now holds the artifact
    bool linked = false;       // ... as a hard link to the store copy
    bool alreadyPresent = false;
};

// Makes `destination` hold the bytes of `source`, linking it to the store copy
// in `storeDir` (refreshed from `source` when absent or stale). Never
// overwrites an existing destination.
PlaceResult PlaceArtifact(const std::filesystem::path& source, const std::filesystem::path& destination,
                          const std::filesystem::path& storeDir);

// Deletes store files no session links anymore. Returns the number removed.
size_t PruneUnreferencedStoreFiles(const std::filesystem::path& storeDir);

// Number of directory entries naming this file (1 for an unlinked file), or 0
// when it cannot be read.
unsigned long QueryLinkCount(const std::filesystem::path& file);

// The store directory below a logs root.
inline std::filesystem::path StoreDirForLogsRoot(const std::filesystem::path& logsRoot) {
    return logsRoot / kSymbolStoreDirName;
}

}  // namespace ce::crash_symbols
