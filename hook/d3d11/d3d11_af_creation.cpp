// The game's D3D11 sampler creations (d3d11_af_creation.h): CE's creation-time policy, the
// creation tag, and the per-thread memo that skips both for a descriptor and object seen before.

#include "d3d11_af_creation.h"

#include <cstring>

#include "d3d11_af_engine.h"
#include "d3d11_af_replacements.h"
#include "d3d11_af_thread.h"
#include "dx11_hook_internal.h"

#include "common/logging/log_meter.h"
#include "hook/metrics/hook_cpu_cost.h"
#include "hook/overrides/sampler_override/creation_policy.h"
#include "hook/overrides/sampler_override/translation_scope.h"

namespace ce::d3d11_af {

namespace {

std::atomic<uint64_t> g_unblocked[kCreationStatCount] = {};  // threads without a block
// Every tag write, any thread: a creation that confirmed its tag while another thread wrote one
// cannot tell whose object that was, and does not memoize.
std::atomic<uint64_t> g_tagWrites{0};

void Bump(ThreadBlock* block, CreationStat stat, uint64_t amount = 1) {
    if (block) {
        block->creationStats[stat] += amount;
    } else {
        g_unblocked[stat].fetch_add(amount, std::memory_order_relaxed);
    }
}

// 32-bit multiplies in two independent lanes: the x86 hook has no fast 64-bit multiply, and the
// 64-bit FNV chain this replaced took 52 cycles there against 24 (measured 2026-10-17). A
// collision costs only a probe: entries compare the whole descriptor.
uint32_t HashDesc(const D3D11_SAMPLER_DESC& desc, uint64_t policyHash) {
    constexpr size_t kWords = sizeof(D3D11_SAMPLER_DESC) / sizeof(uint32_t);
    static_assert(sizeof(D3D11_SAMPLER_DESC) % sizeof(uint32_t) == 0 && kWords % 2 == 1);
    uint32_t words[kWords];
    std::memcpy(words, &desc, sizeof(words));
    uint32_t a = static_cast<uint32_t>(policyHash) ^ 0x9E3779B1u;
    uint32_t b = static_cast<uint32_t>(policyHash >> 32) + 0x85EBCA77u;
    for (size_t i = 0; i + 1 < kWords; i += 2) {
        a = (a ^ words[i]) * 0x01000193u;
        b = (b ^ words[i + 1]) * 0x01000193u;
    }
    a = (a ^ words[kWords - 1]) * 0x01000193u;
    const uint32_t hash = a ^ (b * 0x9E3779B1u);
    return hash ^ (hash >> 15);
}

// The owner's entry for this policy and application descriptor, or null.
CreationMemoEntry* FindEntry(ThreadBlock& block, uint64_t policyHash, const D3D11_SAMPLER_DESC& desc,
                             uint32_t descHash) {
    for (size_t probe = 0; probe < ThreadBlock::kCreationProbe; ++probe) {
        CreationMemoEntry& entry = block.creation[(descHash + probe) % ThreadBlock::kCreationEntries];
        // NOLINTNEXTLINE(bugprone-suspicious-memory-comparison) - the descriptor has no padding; bit identity is meant
        if (entry.used && entry.descHash == descHash && entry.policyHash == policyHash &&
            std::memcmp(&entry.application, &desc, sizeof(desc)) == 0) {
            entry.stamp = ++block.creationClock;
            return &entry;
        }
    }
    return nullptr;
}

// A new entry (the probe window's empty or least recently used one), confirming no object yet.
CreationMemoEntry& ClaimEntry(ThreadBlock& block, uint64_t policyHash, const D3D11_SAMPLER_DESC& desc,
                              uint32_t descHash, const D3D11_SAMPLER_DESC& applied, bool modified) {
    CreationMemoEntry* victim = nullptr;
    for (size_t probe = 0; probe < ThreadBlock::kCreationProbe; ++probe) {
        CreationMemoEntry& entry = block.creation[(descHash + probe) % ThreadBlock::kCreationEntries];
        if (!victim || (victim->used && (!entry.used || entry.stamp < victim->stamp))) {
            victim = &entry;
        }
    }
    victim->tagged.store(nullptr, std::memory_order_seq_cst);
    victim->used = true;
    victim->modified = modified;
    victim->descHash = descHash;
    victim->policyHash = policyHash;
    victim->application = desc;
    victim->applied = applied;
    victim->stamp = ++block.creationClock;
    return *victim;
}

// Whether the memo vouches for the tag of the object the runtime returned: confirmed by this
// thread before, and since then neither destroyed (its sentinel would have cleared the entry
// before the address could be reused) nor retagged.
bool TagVouched(const CreationMemoEntry* entry, const ID3D11SamplerState* sampler) {
    return entry && entry->tagged.load(std::memory_order_acquire) == sampler;
}

// Tags the object the runtime returned with the policy hash and the application's descriptor, and
// lets `entry` (may be null) vouch for it from now on.
void RecordTag(ThreadBlock* block, CreationMemoEntry* entry, ID3D11SamplerState* sampler, uint64_t policyHash,
               const D3D11_SAMPLER_DESC& application) {
    // Read before the tag: any tag write after this point leaves this creation's confirmation
    // unmemoized (seq_cst pairs with ForgetCreatedSampler's scan; see the second load below).
    const uint64_t writes = g_tagWrites.load(std::memory_order_seq_cst);
    const HRESULT tagHr = ce::sampler_override::RecordD3D11SamplerCreationPolicy(sampler, policyHash, application);
    uint64_t expected = writes;
    bool raced = false;
    if (tagHr == S_FALSE) {
        Bump(block, kTagsKeptRead);
    } else if (SUCCEEDED(tagHr)) {
        Bump(block, kTagsWritten);
        raced = g_tagWrites.fetch_add(1, std::memory_order_seq_cst) != writes;
        expected = writes + 1;
        // Other threads' memos may name this object with the tag it carried until now.
        ForgetCreatedSampler(sampler);
    } else {
        static ce::log_meter::ChangeGate tagFailures;
        if (const auto verdict = tagFailures.Observe(ce::log_meter::FieldKey(tagHr))) {
            HookLog("DX11 AF: sampler creation policy tag failed hr=0x%08X; using legacy bind policy%s", tagHr,
                    ce::log_meter::SuppressedNote(verdict.suppressed).c_str());
        }
        return;
    }
    if (!entry) {
        return;
    }
    if (raced) {
        Bump(block, kRaced);
        return;
    }
    // The sentinel is what makes vouching for an address safe: without it, no entry.
    if (!AttachSamplerSentinel(sampler)) {
        return;
    }
    entry->tagged.store(sampler, std::memory_order_seq_cst);
    if (g_tagWrites.load(std::memory_order_seq_cst) != expected) {
        // Another creation wrote a tag meanwhile, maybe this object's; its scan may have run
        // before the store above.
        entry->tagged.store(nullptr, std::memory_order_seq_cst);
        Bump(block, kRaced);
    }
}

}  // namespace

void ForgetCreatedSampler(const ID3D11SamplerState* sampler) {
    if (!sampler) {
        return;
    }
    for (size_t index = 0; index < kMaxThreadBlocks; ++index) {
        ThreadBlock* block = ThreadBlockAt(index);
        if (!block) {
            continue;
        }
        for (CreationMemoEntry& entry : block->creation) {
            const ID3D11SamplerState* expected = sampler;
            if (entry.tagged.load(std::memory_order_seq_cst) == sampler) {
                entry.tagged.compare_exchange_strong(expected, nullptr, std::memory_order_seq_cst);
            }
        }
    }
}

SamplerCreationStats SnapshotSamplerCreationStats() {
    uint64_t totals[kCreationStatCount] = {};
    for (size_t stat = 0; stat < kCreationStatCount; ++stat) {
        totals[stat] = g_unblocked[stat].load(std::memory_order_relaxed);
    }
    size_t threads = 0;
    for (size_t index = 0; index < kMaxThreadBlocks; ++index) {
        if (const ThreadBlock* block = ThreadBlockAt(index)) {
            ++threads;
            for (size_t stat = 0; stat < kCreationStatCount; ++stat) {
                totals[stat] += block->creationStats[stat].Load();
            }
        }
    }
    SamplerCreationStats stats;
    stats.creates = totals[kFullHits] + totals[kOtherCreates];
    stats.tagsWritten = totals[kTagsWritten];
    stats.tagsKept = totals[kFullHits] + totals[kTagsKeptRead];
    stats.descHits = totals[kFullHits] + totals[kDescHitsPartial];
    stats.tagSkips = totals[kFullHits];
    stats.raced = totals[kRaced];
    stats.timedCreates = totals[kTimedCreates];
    stats.timedCeTicks = totals[kTimedCeTicks];
    stats.timedRuntimeTicks = totals[kTimedRuntimeTicks];
    stats.threads = threads;
    return stats;
}

HRESULT CreateSamplerWithCreationPolicy(const D3D11_SAMPLER_DESC* desc, ID3D11SamplerState** sampler,
                                        CreateSamplerFn create, void* createContext, const char* path) {
    ce::sampler_override::D3D11SamplerCreationScope translationScope;
    if (!desc || HookIsShuttingDown() || !g_GraphicsOverridesActive.load(std::memory_order_acquire)) {
        return create(createContext, desc, sampler);
    }
    // One creation in 32 per thread is timed: CE's work around the runtime's call, and the call.
    thread_local uint32_t t_creates = 0;
    const int64_t startTicks = (++t_creates & 31u) == 0 ? HookQpcTicks() : 0;
    int64_t runtimeTicks[2] = {};
    // The thread's cached config and its hash: GetActiveGraphicsConfig() took a process-wide lock
    // and copied the whole config, dozens of strings, for every sampler a game creates. Creation
    // needs no replacement epoch: binds and draws acquire it and keep it fresh.
    const SamplerConfigSnapshot policy = GetActiveSamplerConfigCached();
    ThreadBlock* block = CurrentThreadBlock();
    // Forced AF needs the shader and the texture, so it waits for the bind; creation handles AF
    // off, mip mapping and mip bias. The result depends on the policy and the descriptor only.
    const uint32_t descHash = HashDesc(*desc, policy.hash);
    CreationMemoEntry* entry = block ? FindEntry(*block, policy.hash, *desc, descHash) : nullptr;
    const bool descHit = entry != nullptr;
    D3D11_SAMPLER_DESC applied = *desc;
    bool modified = false;
    if (entry) {
        applied = entry->applied;
        modified = entry->modified;
    } else {
        modified = DX11Hook_ApplySamplerOverrides(applied, *policy.gfx, false);
        if (block) {
            entry = &ClaimEntry(*block, policy.hash, *desc, descHash, applied, modified);
        }
    }
    const uint64_t entryStamp = entry ? entry->stamp : 0;
    static std::atomic<int> s_createLogs{0};
    int idx = 48;  // past the cap a creation costs no atomic write here
    if (s_createLogs.load(std::memory_order_relaxed) < 48) {
        idx = s_createLogs.fetch_add(1, std::memory_order_relaxed);
    }
    if (idx < 48 && (modified || ce::sampler_override::IsAnisotropicOverrideEnabled(*policy.gfx))) {
        HookLogImportant(
            "DX11: CreateSamplerState %s path=%s Filter=0x%X->0x%X Aniso=%u->%u Bias=%.2f Addr=%d/%d/%d Comp=%d "
            "MinLOD=%.1f MaxLOD=%.1f (#%d)",
            modified ? "override" : "deferred runtime AF", path ? path : "?", desc->Filter, applied.Filter,
            desc->MaxAnisotropy, applied.MaxAnisotropy, applied.MipLODBias, desc->AddressU, desc->AddressV,
            desc->AddressW, desc->ComparisonFunc, desc->MinLOD, desc->MaxLOD, idx + 1);
    }
    if (startTicks != 0) {
        runtimeTicks[0] = HookQpcTicks();
    }
    const HRESULT hr = create(createContext, modified ? &applied : desc, sampler);
    if (startTicks != 0) {
        runtimeTicks[1] = HookQpcTicks();
    }
    if (FAILED(hr)) {
        // A game that cannot create a sampler usually dies on it, as BioShock Infinite did when
        // CE's replacements had used up the device's 4096 (20261017_135632): log every cause.
        static ce::log_meter::ChangeGate failures;
        if (const auto verdict = failures.Observe(ce::log_meter::FieldKey(hr, modified))) {
            HookLogImportant(
                "DX11: Game CreateSamplerState failed hr=0x%08X path=%s policyModified=%d Filter=0x%X Bias=%.2f "
                "replacementEntries=%lld%s",
                hr, path ? path : "?", modified ? 1 : 0, applied.Filter, applied.MipLODBias,
                static_cast<long long>(SnapshotReplacementStats().entries),
                ce::log_meter::SuppressedNote(verdict.suppressed).c_str());
        }
    }
    if (SUCCEEDED(hr) && sampler && *sampler) {
        // A creation the runtime ran on this thread (a layered device creating its own) may have
        // reused the entry meanwhile.
        if (entry && entry->stamp != entryStamp) {
            entry = nullptr;
        }
        if (descHit && TagVouched(entry, *sampler)) {
            Bump(block, kFullHits);  // the steady state: one count
        } else {
            Bump(block, kOtherCreates);
            if (descHit) {
                Bump(block, kDescHitsPartial);
            }
            RecordTag(block, entry, *sampler, policy.hash, *desc);
        }
    } else {
        Bump(block, kOtherCreates);
    }
    if (startTicks != 0) {
        const int64_t endTicks = HookQpcTicks();
        const int64_t ceTicks = (runtimeTicks[0] - startTicks) + (endTicks - runtimeTicks[1]);
        Bump(block, kTimedCreates);
        Bump(block, kTimedCeTicks, static_cast<uint64_t>(ceTicks));
        Bump(block, kTimedRuntimeTicks, static_cast<uint64_t>(runtimeTicks[1] - runtimeTicks[0]));
    }
    return hr;
}

#ifdef CE_FLOW_TEST

// Whether any thread's memo vouches for `sampler`'s tag (an address comparison only).
extern "C" __declspec(dllexport) bool CEFlow_D3D11CreationMemoVouches(const ID3D11SamplerState* sampler) {
    for (size_t index = 0; index < kMaxThreadBlocks; ++index) {
        if (const ThreadBlock* block = ThreadBlockAt(index)) {
            for (const CreationMemoEntry& entry : block->creation) {
                if (entry.tagged.load(std::memory_order_seq_cst) == sampler) {
                    return true;
                }
            }
        }
    }
    return false;
}

// The creation tag `sampler` carries: the policy hash and the application's descriptor.
extern "C" __declspec(dllexport) bool CEFlow_GetD3D11SamplerCreationTag(ID3D11SamplerState* sampler, uint64_t* hash,
                                                                        D3D11_SAMPLER_DESC* application) {
    ce::sampler_override::D3D11SamplerCreationPolicy tag = {};
    UINT size = sizeof(tag);
    if (!sampler ||
        FAILED(sampler->GetPrivateData(ce::sampler_override::kD3D11SamplerCreationPolicyGuid, &size, &tag)) ||
        size != sizeof(tag)) {
        return false;
    }
    if (hash) {
        *hash = tag.hash;
    }
    if (application) {
        *application = tag.applicationDesc;
    }
    return true;
}

#endif

}  // namespace ce::d3d11_af
