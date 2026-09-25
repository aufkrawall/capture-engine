#pragma once

#include <windows.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

// Code address -> owning module (handle, image range, full path) without the loader.
//
// GetModuleFileNameA takes the loader lock, and GetModuleHandleExA(FROM_ADDRESS) takes the loader's
// module-table lock. CE classifies callers with them on the Present and ExecuteCommandLists paths,
// several times per call, so any DLL load on another thread (NGX, Streamline and FFX all map
// modules mid-session) stalled the frame-pacing thread behind the whole load, and every present
// paid several loader round trips even when nothing was loading. Module identity for a given
// address cannot change while the module stays mapped, so it is resolved once and then served
// from a small table.
//
// Correctness rests on one invariant: an entry is only served while no module has unloaded since
// it was resolved. Every unload bumps the generation (NoteModuleUnloaded, loader-safe, called from
// the loader's own unload notification before the image is unmapped); an entry records the
// generation it was resolved under and stops matching the moment it differs. Loads need no
// invalidation: only positive lookups are cached, and a newly mapped image cannot overlap the
// range of one that is still mapped. Without a registered unload notification the cache stays
// disabled and every lookup goes to the loader exactly as before.

namespace ce::overlay_compat {

namespace module_address_cache {

struct Entry {
    std::uintptr_t begin = 0;
    std::uintptr_t end = 0;
    HMODULE module = nullptr;
    std::uint64_t generation = 0;
    char path[MAX_PATH] = {};
};

// Pure table: lookup/insert against a caller-supplied generation. Unit-tested without the loader.
template <std::size_t N>
class Table {
public:
    static_assert(N > 0, "table needs at least one slot");

    const Entry* Find(std::uintptr_t address, std::uint64_t generation) const {
        for (const Entry& entry : entries_) {
            if (entry.module && entry.generation == generation && address >= entry.begin && address < entry.end) {
                return &entry;
            }
        }
        return nullptr;
    }

    // Returns false (and stores nothing) when the resolution raced an unload.
    bool Insert(const Entry& resolved, std::uint64_t currentGeneration) {
        if (!resolved.module || resolved.end <= resolved.begin || resolved.generation != currentGeneration) {
            return false;
        }
        for (Entry& entry : entries_) {
            if (entry.module == resolved.module && entry.begin == resolved.begin) {
                entry = resolved;
                return true;
            }
        }
        entries_[next_] = resolved;
        next_ = (next_ + 1) % N;
        return true;
    }

private:
    Entry entries_[N] = {};
    std::size_t next_ = 0;
};

constexpr std::size_t kCapacity = 32;

struct State {
    SRWLOCK lock = SRWLOCK_INIT;
    Table<kCapacity> table;
    std::atomic<std::uint64_t> generation{1};
    // Loads AND unloads: "has the loaded module set changed since X" for work that scans modules.
    std::atomic<std::uint64_t> moduleSetGeneration{1};
    std::atomic<bool> enabled{false};
    std::atomic<std::uint64_t> hits{0};
    std::atomic<std::uint64_t> misses{0};
};

inline State& GetState() {
    static State state;
    return state;
}

// Called once the process has a loader unload notification that will call NoteModuleUnloaded.
inline void Enable() {
    GetState().enabled.store(true, std::memory_order_release);
}

// Loader-safe: runs inside the loader's unload notification. Atomics only.
inline void NoteModuleUnloaded() {
    GetState().generation.fetch_add(1, std::memory_order_acq_rel);
    GetState().moduleSetGeneration.fetch_add(1, std::memory_order_acq_rel);
}

// Loader-safe: runs inside the loader's load notification. Atomics only.
inline void NoteModuleLoaded() {
    GetState().moduleSetGeneration.fetch_add(1, std::memory_order_acq_rel);
}

// Meaningful only while IsEnabled(): without the notification nothing bumps it.
inline std::uint64_t ModuleSetGeneration() {
    return GetState().moduleSetGeneration.load(std::memory_order_acquire);
}

inline bool IsEnabled() {
    return GetState().enabled.load(std::memory_order_acquire);
}

inline std::uint64_t Hits() {
    return GetState().hits.load(std::memory_order_relaxed);
}

inline std::uint64_t Misses() {
    return GetState().misses.load(std::memory_order_relaxed);
}

inline std::uintptr_t ImageEnd(HMODULE module) {
    const auto base = reinterpret_cast<std::uintptr_t>(module);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(module);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) {
        return 0;
    }
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + static_cast<std::uintptr_t>(dos->e_lfanew));
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.SizeOfImage == 0) {
        return 0;
    }
    return base + nt->OptionalHeader.SizeOfImage;
}

// Loader-backed resolution, identical to what callers did before the cache.
inline bool ResolveFromLoader(const void* codeAddress, Entry* out, bool wantPath) {
    HMODULE module = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCSTR>(codeAddress), &module) ||
        !module) {
        return false;
    }
    out->module = module;
    out->path[0] = '\0';
    if (wantPath && GetModuleFileNameA(module, out->path, MAX_PATH) == 0) {
        out->path[0] = '\0';
    }
    return true;
}

// Resolves the module owning codeAddress. pathOut may be null. Returns false when no module owns it.
// hasPathOut reports whether the loader produced a path (it can fail for a module being torn down).
inline bool Lookup(const void* codeAddress, HMODULE* moduleOut, char* pathOut, std::size_t pathOutCount,
                   bool* hasPathOut) {
    State& state = GetState();
    const auto address = reinterpret_cast<std::uintptr_t>(codeAddress);
    auto copyOut = [&](const Entry& entry) {
        if (moduleOut) {
            *moduleOut = entry.module;
        }
        if (pathOut && pathOutCount > 0) {
            strncpy_s(pathOut, pathOutCount, entry.path, _TRUNCATE);
        }
        if (hasPathOut) {
            *hasPathOut = entry.path[0] != '\0';
        }
    };

    if (state.enabled.load(std::memory_order_acquire)) {
        const std::uint64_t generation = state.generation.load(std::memory_order_acquire);
        AcquireSRWLockShared(&state.lock);
        const Entry* hit = state.table.Find(address, generation);
        if (hit) {
            copyOut(*hit);
            ReleaseSRWLockShared(&state.lock);
            state.hits.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
        ReleaseSRWLockShared(&state.lock);
        state.misses.fetch_add(1, std::memory_order_relaxed);

        Entry resolved;
        if (!ResolveFromLoader(codeAddress, &resolved, true)) {
            return false;
        }
        resolved.begin = reinterpret_cast<std::uintptr_t>(resolved.module);
        resolved.end = ImageEnd(resolved.module);
        resolved.generation = generation;
        // A failed path read is not cached: the next lookup asks the loader again.
        if (resolved.path[0] != '\0' && resolved.end > address && resolved.begin <= address) {
            AcquireSRWLockExclusive(&state.lock);
            state.table.Insert(resolved, state.generation.load(std::memory_order_acquire));
            ReleaseSRWLockExclusive(&state.lock);
        }
        copyOut(resolved);
        return true;
    }

    Entry resolved;
    if (!ResolveFromLoader(codeAddress, &resolved, pathOut && pathOutCount > 0)) {
        return false;
    }
    copyOut(resolved);
    return true;
}

}  // namespace module_address_cache

}  // namespace ce::overlay_compat
