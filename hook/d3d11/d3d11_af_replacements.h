#pragma once

// CE's forced-AF replacement samplers, kept exactly as long as the game samplers they stand in for.
//
// Until 2026-10-17 a process list kept every replacement until the config changed. BioShock
// Infinite creates a sampler per draw for each streaming texture (MinLOD clamp, fade bias) and
// releases it after the bind; CE added one replacement per descriptor, and the device ran out of
// its 4096 unique sampler objects. The game's own CreateSamplerState failed with
// D3D11_ERROR_TOO_MANY_UNIQUE_STATE_OBJECTS, its purge-and-retry could free none of CE's, and it
// crashed on the null sampler (session 20261017_135632). Each lookup also scanned that list, so
// every new sampler cost more than the one before.
//
// Now a registry maps each game sampler to its replacements. A sentinel in the game sampler's
// private data holds no reference; the runtime destroys it with the sampler, and it removes the
// entry, so the address cannot be reused while an entry still names it. A new policy epoch drops
// every entry. Within one epoch the policy is idempotent (a replacement needs no replacement), so
// entries never hold each other in a cycle, even when the runtime hands out a game sampler as a
// replacement for an identical descriptor.

#include <d3d11.h>

#include <cstdint>

namespace ce::d3d11_af {

// The replacement registered for `logical` in this epoch and AF verdict, acquired; null if none.
ID3D11SamplerState* AcquireRegisteredReplacement(ID3D11SamplerState* logical, uint64_t epoch, bool allowAF);
// Registers `replacement` for `logical` (the registry takes its own reference; the caller keeps
// its own) and marks it as CE's replacement for `epoch` unless the game created it.
void RegisterReplacement(ID3D11SamplerState* logical, uint64_t epoch, bool allowAF, ID3D11SamplerState* replacement);
// Whether CE created `sampler` as a replacement in `epoch`. One private-data read.
bool IsReplacementInEpoch(ID3D11SamplerState* sampler, uint64_t epoch);
// A new policy epoch: drops every entry and the references it held.
void DropRegisteredReplacements(uint64_t epoch);

void CountReplacementCreated();
void CountReplacementCreateFailed();

struct ReplacementStats {
    uint64_t created = 0;          // replacements created for a game sampler with none registered
    uint64_t hits = 0;             // replacements found registered for the game's sampler
    uint64_t createFailures = 0;
    uint64_t registerFailures = 0;
    int64_t entries = 0;           // game samplers holding replacements now
    int64_t peakEntries = 0;
};
ReplacementStats SnapshotReplacementStats();

}  // namespace ce::d3d11_af
