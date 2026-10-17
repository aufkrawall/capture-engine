// The game's D3D11 sampler creations (d3d11_af_creation.h): CE's creation-time policy, the
// creation tag, and the per-thread memo that skips both for a descriptor and object seen before.

#include "d3d11_af_creation.h"

#include <cstring>
#include <new>

#include "d3d11_af_engine.h"
#include "d3d11_af_replacements.h"
#include "dx11_hook_internal.h"

#include "common/logging/log_meter.h"
#include "hook/metrics/hook_cpu_cost.h"
#include "hook/overrides/sampler_override/creation_policy.h"
#include "hook/overrides/sampler_override/translation_scope.h"

namespace ce::d3d11_af {

namespace {

constexpr size_t kMemoEntries = 128;
constexpr size_t kMemoProbe = 4;
constexpr size_t kMaxBlocks = 32;
// A thread that found every block owned by a live thread tries again after this many creations.
constexpr uint32_t kClaimRetryInterval = 4096;

enum Stat : size_t {
    kCreates,
    kTagsWritten,
    kTagsKept,
    kDescHits,  // the policy's result came from the memo
    kTagSkips,  // the tag was known to be on the object: no private-data read
    kRaced,     // another creation rewrote a tag while this one confirmed its own: not memoized
    kTimedCreates,
    kTimedCeTicks,
    kTimedRuntimeTicks,
    kStatCount,
};

struct MemoEntry {
    // The object last confirmed to carry this entry's tag. Set by the owner thread; cleared by
    // any thread (ForgetCreatedSampler).
    std::atomic<const ID3D11SamplerState*> tagged{nullptr};
    // Owner thread only.
    bool used = false;
    bool modified = false;
    uint32_t descHash = 0;
    uint64_t policyHash = 0;
    uint64_t stamp = 0;
    D3D11_SAMPLER_DESC application = {};
    D3D11_SAMPLER_DESC applied = {};
};

struct CreationBlock {
    HANDLE owner = nullptr;  // g_claimLock: the owning thread, null when unknown (never reclaimed)
    Count stats[kStatCount];
    uint64_t clock = 0;  // owner thread
    MemoEntry memo[kMemoEntries];
};

// Never freed: cached thread pointers stay valid, and a sentinel can be released after static
// destruction at process exit.
std::atomic<CreationBlock*> g_blocks[kMaxBlocks] = {};
SRWLOCK g_claimLock = SRWLOCK_INIT;
std::atomic<uint64_t> g_blocksClaimed{0};
std::atomic<uint64_t> g_unblocked[kStatCount] = {};  // threads without a block
// Every tag write, any thread: a creation that confirmed its tag while another thread wrote one
// cannot tell whose object that was, and does not memoize.
std::atomic<uint64_t> g_tagWrites{0};

thread_local CreationBlock* t_block = nullptr;
thread_local uint32_t t_unblockedCreates = 0;

void Bump(CreationBlock* block, Stat stat, uint64_t amount = 1) {
    if (block) {
        block->stats[stat] += amount;
    } else {
        g_unblocked[stat].fetch_add(amount, std::memory_order_relaxed);
    }
}

void ResetMemo(CreationBlock& block) {
    for (MemoEntry& entry : block.memo) {
        entry.tagged.store(nullptr, std::memory_order_seq_cst);
        entry.used = false;
        entry.stamp = 0;
    }
    block.clock = 0;
}

// A block for the calling thread: a new one, or one whose thread has exited (its counts stay,
// its memo starts empty). Null when every block belongs to a live thread.
CreationBlock* ClaimBlock() {
    HANDLE self = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &self, SYNCHRONIZE, FALSE, 0)) {
        self = nullptr;
    }
    CreationBlock* claimed = nullptr;
    bool reclaimed = false;
    size_t index = 0;
    AcquireSRWLockExclusive(&g_claimLock);
    for (; index < kMaxBlocks; ++index) {
        CreationBlock* block = g_blocks[index].load(std::memory_order_relaxed);
        if (!block) {
            block = new (std::nothrow) CreationBlock();
            if (block) {
                g_blocks[index].store(block, std::memory_order_release);
            }
            claimed = block;
            break;
        }
        if (block->owner && WaitForSingleObject(block->owner, 0) == WAIT_OBJECT_0) {
            CloseHandle(block->owner);
            ResetMemo(*block);
            claimed = block;
            reclaimed = true;
            break;
        }
    }
    if (claimed) {
        claimed->owner = self;
    }
    ReleaseSRWLockExclusive(&g_claimLock);
    if (!claimed) {
        if (self) {
            CloseHandle(self);
        }
        static std::atomic<int> s_unblockedLogs{0};
        const int logIndex = s_unblockedLogs.fetch_add(1, std::memory_order_relaxed);
        if (logIndex < 8) {
            HookLog(
                "DX11 AF: no sampler creation block for thread %lu (all %zu owned by live threads); its "
                "creations take the slow path, retried every %u (#%d)",
                GetCurrentThreadId(), kMaxBlocks, kClaimRetryInterval, logIndex + 1);
        }
        return nullptr;
    }
    const uint64_t claims = g_blocksClaimed.fetch_add(1, std::memory_order_relaxed) + 1;
    if (claims <= 32 || reclaimed) {
        HookLog("DX11 AF: sampler creation block %zu %s by thread %lu (claims=%llu)", index,
                reclaimed ? "reclaimed from an exited thread" : "created", GetCurrentThreadId(),
                static_cast<unsigned long long>(claims));
    }
    return claimed;
}

CreationBlock* ThreadBlock() {
    if (CreationBlock* block = t_block) {
        return block;
    }
    if (t_unblockedCreates++ % kClaimRetryInterval != 0) {
        return nullptr;
    }
    t_block = ClaimBlock();
    return t_block;
}

uint32_t HashDesc(const D3D11_SAMPLER_DESC& desc, uint64_t policyHash) {
    static_assert(sizeof(D3D11_SAMPLER_DESC) % sizeof(uint32_t) == 0);
    uint32_t words[sizeof(D3D11_SAMPLER_DESC) / sizeof(uint32_t)];
    std::memcpy(words, &desc, sizeof(words));
    uint64_t hash = policyHash ^ 0x9E3779B97F4A7C15ull;
    for (const uint32_t word : words) {
        hash = (hash ^ word) * 0x100000001B3ull;
    }
    return static_cast<uint32_t>(hash ^ (hash >> 32));
}

// The owner's entry for this policy and application descriptor, or null.
MemoEntry* FindEntry(CreationBlock& block, uint64_t policyHash, const D3D11_SAMPLER_DESC& desc, uint32_t descHash) {
    for (size_t probe = 0; probe < kMemoProbe; ++probe) {
        MemoEntry& entry = block.memo[(descHash + probe) % kMemoEntries];
        // NOLINTNEXTLINE(bugprone-suspicious-memory-comparison) - the descriptor has no padding; bit identity is meant
        if (entry.used && entry.descHash == descHash && entry.policyHash == policyHash &&
            std::memcmp(&entry.application, &desc, sizeof(desc)) == 0) {
            entry.stamp = ++block.clock;
            return &entry;
        }
    }
    return nullptr;
}

// A new entry (the probe window's empty or least recently used one), confirming no object yet.
MemoEntry& ClaimEntry(CreationBlock& block, uint64_t policyHash, const D3D11_SAMPLER_DESC& desc, uint32_t descHash,
                      const D3D11_SAMPLER_DESC& applied, bool modified) {
    MemoEntry* victim = nullptr;
    for (size_t probe = 0; probe < kMemoProbe; ++probe) {
        MemoEntry& entry = block.memo[(descHash + probe) % kMemoEntries];
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
    victim->stamp = ++block.clock;
    return *victim;
}

// Tags the object the runtime returned with the policy hash and the application's descriptor,
// unless this thread's memo knows it carries exactly that tag. `entry` may be null.
void RecordTag(CreationBlock* block, MemoEntry* entry, ID3D11SamplerState* sampler, uint64_t policyHash,
               const D3D11_SAMPLER_DESC& application) {
    if (entry && entry->tagged.load(std::memory_order_acquire) == sampler) {
        // Confirmed by this thread before, and since then neither destroyed (its sentinel would
        // have cleared the entry before the address could be reused) nor retagged.
        Bump(block, kTagsKept);
        Bump(block, kTagSkips);
        return;
    }
    // Read before the tag: any tag write after this point leaves this creation's confirmation
    // unmemoized (seq_cst pairs with ForgetCreatedSampler's scan; see the second load below).
    const uint64_t writes = g_tagWrites.load(std::memory_order_seq_cst);
    const HRESULT tagHr = ce::sampler_override::RecordD3D11SamplerCreationPolicy(sampler, policyHash, application);
    uint64_t expected = writes;
    bool raced = false;
    if (tagHr == S_FALSE) {
        Bump(block, kTagsKept);
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
    for (std::atomic<CreationBlock*>& slot : g_blocks) {
        CreationBlock* block = slot.load(std::memory_order_acquire);
        if (!block) {
            continue;
        }
        for (MemoEntry& entry : block->memo) {
            const ID3D11SamplerState* expected = sampler;
            if (entry.tagged.load(std::memory_order_seq_cst) == sampler) {
                entry.tagged.compare_exchange_strong(expected, nullptr, std::memory_order_seq_cst);
            }
        }
    }
}

SamplerCreationStats SnapshotSamplerCreationStats() {
    uint64_t totals[kStatCount] = {};
    for (size_t stat = 0; stat < kStatCount; ++stat) {
        totals[stat] = g_unblocked[stat].load(std::memory_order_relaxed);
    }
    size_t threads = 0;
    for (std::atomic<CreationBlock*>& slot : g_blocks) {
        if (const CreationBlock* block = slot.load(std::memory_order_acquire)) {
            ++threads;
            for (size_t stat = 0; stat < kStatCount; ++stat) {
                totals[stat] += block->stats[stat].Load();
            }
        }
    }
    SamplerCreationStats stats;
    stats.creates = totals[kCreates];
    stats.tagsWritten = totals[kTagsWritten];
    stats.tagsKept = totals[kTagsKept];
    stats.descHits = totals[kDescHits];
    stats.tagSkips = totals[kTagSkips];
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
    // A thread's cached config and hash: GetActiveGraphicsConfig() took a process-wide lock and
    // copied the whole config, dozens of strings, for every sampler a game creates.
    const SamplerPolicySnapshot11 policy = AcquireSamplerPolicy11();
    CreationBlock* block = ThreadBlock();
    Bump(block, kCreates);
    // Forced AF needs the shader and the texture, so it waits for the bind; creation handles AF
    // off, mip mapping and mip bias. The result depends on the policy and the descriptor only.
    const uint32_t descHash = HashDesc(*desc, policy.hash);
    MemoEntry* entry = block ? FindEntry(*block, policy.hash, *desc, descHash) : nullptr;
    D3D11_SAMPLER_DESC applied = *desc;
    bool modified = false;
    if (entry) {
        Bump(block, kDescHits);
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
        RecordTag(block, entry, *sampler, policy.hash, *desc);
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
    for (std::atomic<CreationBlock*>& slot : g_blocks) {
        if (CreationBlock* block = slot.load(std::memory_order_acquire)) {
            for (const MemoEntry& entry : block->memo) {
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
