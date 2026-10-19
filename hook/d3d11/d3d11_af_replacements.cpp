// CE's forced-AF replacement samplers, kept exactly as long as the game samplers they stand in
// for (d3d11_af_replacements.h). Slow-path work only: a clean draw never reaches it.

#include "d3d11_af_replacements.h"

#include <atomic>
#include <mutex>
#include <new>
#include <unordered_map>

#include "common/logging/log_meter.h"
#include "d3d11_af_creation.h"
#include "d3d11_af_engine.h"
#include "hook/overrides/sampler_override/creation_policy.h"
#include "hook/runtime/hook_common.h"

namespace ce::d3d11_af {

namespace {

// {CEAF1104-5A7D-4A4E-93B8-C3117E6DAF04} - the sentinel on a game sampler with an entry
const GUID kSentinelGuid = {0xceaf1104, 0x5a7d, 0x4a4e, {0x93, 0xb8, 0xc3, 0x11, 0x7e, 0x6d, 0xaf, 0x04}};
// {CEAF1105-5A7D-4A4E-93B8-C3117E6DAF05} - plain data: the epoch CE created a replacement in
const GUID kReplacementMarkGuid = {0xceaf1105, 0x5a7d, 0x4a4e, {0x93, 0xb8, 0xc3, 0x11, 0x7e, 0x6d, 0xaf, 0x05}};
constexpr UINT kSentinelMagic = 0x4E534643u;

struct Entry {
    uint64_t epoch = 0;
    ID3D11SamplerState* physical[2] = {};  // [AF allowed], references held
};

using EntryMap = std::unordered_map<ID3D11SamplerState*, Entry>;

// Never call into the runtime or release an object while holding `mutex`: a release can destroy
// a game sampler whose sentinel takes it.
struct Registry {
    std::mutex mutex;
    EntryMap entries;
    uint64_t epoch = 0;  // entries belong to this epoch or a newer one
};

// Leaked on purpose: a sentinel can be destroyed after static destruction at process exit.
Registry& TheRegistry() {
    static Registry* registry = new Registry();
    return *registry;
}

std::atomic<uint64_t> g_created{0};
std::atomic<uint64_t> g_hits{0};
std::atomic<uint64_t> g_createFailures{0};
std::atomic<uint64_t> g_registerFailures{0};
std::atomic<int64_t> g_entryCount{0};
std::atomic<int64_t> g_peakEntries{0};

void PublishCount(size_t count) {
    const auto live = static_cast<int64_t>(count);
    g_entryCount.store(live, std::memory_order_relaxed);
    int64_t peak = g_peakEntries.load(std::memory_order_relaxed);
    while (live > peak && !g_peakEntries.compare_exchange_weak(peak, live, std::memory_order_relaxed)) {
    }
}

// The references an entry gave up, released once the registry is unlocked.
struct Released {
    ID3D11SamplerState* objects[3] = {};
    int count = 0;

    void Take(Entry& entry) {
        for (ID3D11SamplerState*& physical : entry.physical) {
            if (physical) {
                objects[count++] = physical;
                physical = nullptr;
            }
        }
    }
    void Add(ID3D11SamplerState* object) { objects[count++] = object; }
    ~Released() {
        for (int i = 0; i < count; ++i) {
            DeferredReleaseScope::Release(objects[i]);
        }
    }
};

// Removes the entry of a game sampler the runtime is destroying.
void DropEntry(ID3D11SamplerState* owner) {
    Released released;
    Registry& registry = TheRegistry();
    std::lock_guard<std::mutex> lock(registry.mutex);
    const auto it = registry.entries.find(owner);
    if (it == registry.entries.end()) {
        return;
    }
    released.Take(it->second);
    registry.entries.erase(it);
    PublishCount(registry.entries.size());
}  // `lock` is destroyed before `released`

// Private data of a game sampler CE remembers: with a registry entry, or vouched for by a
// creation memo (d3d11_af_creation.h). Holds no reference: the runtime destroys it while
// destroying the sampler, before the sampler's address can be reused.
struct Sentinel final : IUnknown {
    explicit Sentinel(ID3D11SamplerState* sampler) : owner(sampler) {}
    ~Sentinel() {
        ForgetCreatedSampler(owner);
        DropEntry(owner);
    }
    Sentinel(const Sentinel&) = delete;
    Sentinel& operator=(const Sentinel&) = delete;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override {
        if (!object) {
            return E_POINTER;
        }
        *object = nullptr;
        if (riid == IID_IUnknown) {
            AddRef();
            *object = static_cast<IUnknown*>(this);
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return refs_.fetch_add(1, std::memory_order_relaxed) + 1; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG remaining = refs_.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (remaining == 0) {
            delete this;
        }
        return remaining;
    }

    const UINT magic = kSentinelMagic;
    ID3D11SamplerState* const owner;

private:
    std::atomic<ULONG> refs_{1};
};

// Private data set by SetPrivateDataInterface reads back as the interface pointer itself
// (AddRef'd), so the expected size is that of a pointer, not of the object.
constexpr UINT kSentinelDataSize = sizeof(IUnknown*);

bool HasSentinel(ID3D11SamplerState* sampler) {
    IUnknown* object = nullptr;
    UINT size = kSentinelDataSize;
    if (FAILED(sampler->GetPrivateData(kSentinelGuid, &size, static_cast<void*>(&object))) ||
        size != kSentinelDataSize || !object) {
        return false;
    }
    const auto* sentinel = static_cast<Sentinel*>(object);
    const bool ours = sentinel->magic == kSentinelMagic && sentinel->owner == sampler;
    object->Release();
    return ours;
}

}  // namespace

// The sentinel goes on before the entry, so no entry can outlive its game sampler.
bool AttachSamplerSentinel(ID3D11SamplerState* sampler) {
    if (HasSentinel(sampler)) {
        return true;
    }
    auto* sentinel = new (std::nothrow) Sentinel(sampler);
    if (!sentinel) {
        return false;
    }
    // Two threads attaching at once: the later sentinel replaces the earlier one, whose
    // destruction drops the entry if one was made already. That costs one more creation.
    const HRESULT hr = sampler->SetPrivateDataInterface(kSentinelGuid, sentinel);
    sentinel->Release();
    if (FAILED(hr)) {
        static ce::log_meter::ChangeGate failures;
        if (const auto verdict = failures.Observe(ce::log_meter::FieldKey(hr))) {
            HookLogImportant("DX11 AF: attaching the sentinel to the game's sampler failed hr=0x%08X; its "
                             "replacement lives only while bound or memoized, its creation tag is read back%s",
                             hr, ce::log_meter::SuppressedNote(verdict.suppressed).c_str());
        }
        return false;
    }
    return true;
}

namespace {

// Created through CE's CreateSamplerState hook, i.e. by the game.
bool IsGameSampler(ID3D11SamplerState* sampler) {
    // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization) - filled by GetPrivateData
    ce::sampler_override::D3D11SamplerCreationPolicy tag = {};
    UINT size = sizeof(tag);
    return SUCCEEDED(sampler->GetPrivateData(ce::sampler_override::kD3D11SamplerCreationPolicyGuid, &size, &tag)) &&
           size == sizeof(tag);
}

}  // namespace

ID3D11SamplerState* AcquireRegisteredReplacement(ID3D11SamplerState* logical, uint64_t epoch, bool allowAF) {
    if (!logical) {
        return nullptr;
    }
    ID3D11SamplerState* found = nullptr;
    {
        Registry& registry = TheRegistry();
        std::lock_guard<std::mutex> lock(registry.mutex);
        const auto it = registry.entries.find(logical);
        if (it != registry.entries.end() && it->second.epoch == epoch) {
            found = it->second.physical[allowAF ? 1 : 0];
            if (found) {
                found->AddRef();
            }
        }
    }
    if (found) {
        g_hits.fetch_add(1, std::memory_order_relaxed);
    }
    return found;
}

void RegisterReplacement(ID3D11SamplerState* logical, uint64_t epoch, bool allowAF, ID3D11SamplerState* replacement) {
    if (!logical || !replacement) {
        return;
    }
    // For an identical descriptor the runtime hands out the game's own object. That one stays a
    // game sampler: CE's shortcut for its own replacements must not apply to it.
    if (!IsGameSampler(replacement)) {
        (void)replacement->SetPrivateData(kReplacementMarkGuid, sizeof(epoch), &epoch);
    }
    if (!AttachSamplerSentinel(logical)) {
        g_registerFailures.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    Released released;
    replacement->AddRef();
    bool registered = false;
    {
        Registry& registry = TheRegistry();
        std::lock_guard<std::mutex> lock(registry.mutex);
        if (epoch >= registry.epoch) {
            try {
                Entry& entry = registry.entries[logical];
                if (entry.epoch != epoch) {
                    released.Take(entry);
                    entry.epoch = epoch;
                }
                ID3D11SamplerState*& slot = entry.physical[allowAF ? 1 : 0];
                if (!slot) {
                    slot = replacement;
                    registered = true;
                }
                PublishCount(registry.entries.size());
            } catch (...) {
                g_registerFailures.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
    if (!registered) {
        // A stale epoch, another thread first (the runtime's same object), or out of memory.
        released.Add(replacement);
    }
}

bool IsReplacementInEpoch(ID3D11SamplerState* sampler, uint64_t epoch) {
    uint64_t marked = 0;
    UINT size = sizeof(marked);
    return sampler && SUCCEEDED(sampler->GetPrivateData(kReplacementMarkGuid, &size, &marked)) &&
           size == sizeof(marked) && marked == epoch;
}

void DropRegisteredReplacements(uint64_t epoch) {
    EntryMap dropped;
    {
        Registry& registry = TheRegistry();
        std::lock_guard<std::mutex> lock(registry.mutex);
        if (epoch > registry.epoch) {
            registry.epoch = epoch;
        }
        dropped.swap(registry.entries);
        PublishCount(0);
    }
    // The sentinels stay; each finds no entry when its sampler goes.
    for (auto& [logical, entry] : dropped) {
        for (ID3D11SamplerState* physical : entry.physical) {
            if (physical) {
                DeferredReleaseScope::Release(physical);
            }
        }
    }
}

void CountReplacementCreated() {
    g_created.fetch_add(1, std::memory_order_relaxed);
}

void CountReplacementCreateFailed() {
    g_createFailures.fetch_add(1, std::memory_order_relaxed);
}

ReplacementStats SnapshotReplacementStats() {
    ReplacementStats stats;
    stats.created = g_created.load(std::memory_order_relaxed);
    stats.hits = g_hits.load(std::memory_order_relaxed);
    stats.createFailures = g_createFailures.load(std::memory_order_relaxed);
    stats.registerFailures = g_registerFailures.load(std::memory_order_relaxed);
    stats.entries = g_entryCount.load(std::memory_order_relaxed);
    stats.peakEntries = g_peakEntries.load(std::memory_order_relaxed);
    return stats;
}

}  // namespace ce::d3d11_af
