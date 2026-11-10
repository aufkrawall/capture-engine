#include "swapchain_lifetime_witness.h"

#include <dxgi.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "hook/runtime/hook_clock.h"
#include "hook/runtime/hook_common.h"

namespace ce::swapchain_lifetime_witness {

namespace {

// {6B3D1E2A-5C47-4F0B-9A61-2E7C4D8B5F13}
constexpr GUID kWitnessGuid = {0x6b3d1e2a, 0x5c47, 0x4f0b, {0x9a, 0x61, 0x2e, 0x7c, 0x4d, 0x8b, 0x5f, 0x13}};

// Enough for every chain a session creates (GTA: a handful); the oldest destroyed record goes first.
constexpr size_t kMaxRecords = 64;
constexpr USHORT kMaxDestructionFrames = 32;

struct Record {
    uint64_t generation = 0;
    ChainState state = ChainState::kAlive;
    HWND window = nullptr;
    uint64_t watchedAtMs = 0;
    uint64_t destroyedAtMs = 0;
    DWORD destroyingThread = 0;
    char destroyedBy[64] = {};
};

struct Registry {
    std::mutex mutex;
    std::unordered_map<uintptr_t, Record> records;
    uint64_t nextGeneration = 1;
};

// Never destroyed: DXGI may release a witness during process exit, after static destructors ran.
Registry& TheRegistry() {
    static Registry* registry = new Registry();
    return *registry;
}

// The chain witnessed last while it is alive: ProcessFrame asks for its presenting chain every frame, and that
// answer must not take a lock. Cleared when that chain is destroyed or its record evicted.
std::atomic<uintptr_t> g_LastWitnessedAlive{0};

void ForgetFastPath(uintptr_t key) {
    uintptr_t expected = key;
    g_LastWitnessedAlive.compare_exchange_strong(expected, 0, std::memory_order_acq_rel);
}

// The image an address belongs to, named from its own export directory - no loader lock and no allocation, so it
// is safe on the thread that is inside DXGI's destruction of a swapchain. Images without one (the game's
// executable) are named by their base address.
void DescribeFrame(void* frame, char* name, size_t nameSize, uintptr_t* offset) {
    *offset = 0;
    snprintf(name, nameSize, "unknown");
    void* base = nullptr;
    if (!frame || !RtlPcToFileHeader(frame, &base) || !base) {
        return;
    }
    *offset = reinterpret_cast<uintptr_t>(frame) - reinterpret_cast<uintptr_t>(base);
    const auto* image = static_cast<const BYTE*>(base);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return;
    }
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(image + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        return;
    }
    const IMAGE_DATA_DIRECTORY& exports = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (exports.VirtualAddress != 0 && exports.Size >= sizeof(IMAGE_EXPORT_DIRECTORY)) {
        const auto* directory = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(image + exports.VirtualAddress);
        if (directory->Name != 0) {
            snprintf(name, nameSize, "%s", reinterpret_cast<const char*>(image + directory->Name));
            return;
        }
    }
    snprintf(name, nameSize, "image@%p", base);
}

void NameReleaser(char* out, size_t outSize) {
    snprintf(out, outSize, "unknown");
    void* frames[kMaxDestructionFrames] = {};
    const USHORT count = CaptureStackBackTrace(0, kMaxDestructionFrames, frames, nullptr);
    char names[kMaxDestructionFrames][40] = {};
    uintptr_t offsets[kMaxDestructionFrames] = {};
    const char* views[kMaxDestructionFrames] = {};
    for (USHORT i = 0; i < count; ++i) {
        DescribeFrame(frames[i], names[i], sizeof(names[i]), &offsets[i]);
        views[i] = names[i];
    }
    const int releaser = SelectReleaserFrame(views, count);
    if (releaser >= 0) {
        snprintf(out, outSize, "%s+0x%llX", names[releaser], static_cast<unsigned long long>(offsets[releaser]));
    }
}

void NoteDestroyed(uintptr_t key, uint64_t generation) {
    if (IsProcessTerminating()) {
        return;
    }
    char releaser[64];
    NameReleaser(releaser, sizeof(releaser));
    const uint64_t now = ce::hook_clock::TickCount64();
    HWND window = nullptr;
    uint64_t lifetimeMs = 0;
    {
        Registry& registry = TheRegistry();
        std::lock_guard<std::mutex> lock(registry.mutex);
        const auto it = registry.records.find(key);
        // A witness replaced on a later chain at this address reports its own, earlier generation: ignore it.
        if (it == registry.records.end() || it->second.generation != generation) {
            return;
        }
        ForgetFastPath(key);
        Record& record = it->second;
        record.state = ChainState::kDestroyed;
        record.destroyedAtMs = now;
        record.destroyingThread = GetCurrentThreadId();
        snprintf(record.destroyedBy, sizeof(record.destroyedBy), "%s", releaser);
        window = record.window;
        lifetimeMs = now - record.watchedAtMs;
    }
    static std::atomic<uint32_t> s_destroyedLogs{0};
    const uint32_t logNumber = s_destroyedLogs.fetch_add(1, std::memory_order_relaxed) + 1;
    if (logNumber <= 32 || (logNumber % 64) == 0) {
        HookLogImportant(
            "[SwapchainLifetime] destroyed sc=%p window=%p after %llums thread=%lu lastReleaseBy=%s (#%u)",
            reinterpret_cast<void*>(key), window, static_cast<unsigned long long>(lifetimeMs), GetCurrentThreadId(),
            releaser, logNumber);
    }
}

// DXGI holds the only lasting reference; its final Release is the chain's destruction.
class Witness final : public IUnknown {
public:
    Witness(uintptr_t key, uint64_t generation) : key_(key), generation_(generation) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (!out) {
            return E_POINTER;
        }
        if (riid == __uuidof(IUnknown)) {
            *out = static_cast<IUnknown*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return refs_.fetch_add(1, std::memory_order_relaxed) + 1; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG remaining = refs_.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (remaining == 0) {
            if (armed_.load(std::memory_order_acquire)) {
                NoteDestroyed(key_, generation_);
            }
            delete this;
        }
        return remaining;
    }
    void Disarm() { armed_.store(false, std::memory_order_release); }

private:
    ~Witness() = default;
    std::atomic<ULONG> refs_{1};
    std::atomic<bool> armed_{true};
    uintptr_t key_;
    uint64_t generation_;
};

void EvictOneLocked(Registry& registry) {
    auto victim = registry.records.end();
    for (auto it = registry.records.begin(); it != registry.records.end(); ++it) {
        const bool destroyed = it->second.state == ChainState::kDestroyed;
        if (victim == registry.records.end()) {
            victim = it;
            continue;
        }
        const bool victimDestroyed = victim->second.state == ChainState::kDestroyed;
        if (destroyed != victimDestroyed) {
            if (destroyed) {
                victim = it;
            }
            continue;
        }
        const uint64_t age = destroyed ? it->second.destroyedAtMs : it->second.watchedAtMs;
        const uint64_t victimAge = victimDestroyed ? victim->second.destroyedAtMs : victim->second.watchedAtMs;
        if (age < victimAge) {
            victim = it;
        }
    }
    if (victim != registry.records.end()) {
        ForgetFastPath(victim->first);
        registry.records.erase(victim);
    }
}

bool EqualsIgnoreCase(const char* a, const char* b) {
    for (; *a && *b; ++a, ++b) {
        char ca = *a;
        char cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca = static_cast<char>(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = static_cast<char>(cb - 'A' + 'a');
        if (ca != cb) {
            return false;
        }
    }
    return *a == *b;
}

}  // namespace

bool IsReleaseMachineryModule(const char* imageName) {
    if (!imageName || !imageName[0]) {
        return false;
    }
    static constexpr const char* kMachinery[] = {
        "dxgi.dll",     "d3d12.dll",        "d3d12core.dll",  "d3d11.dll",    "d3d10warp.dll",
        "dxcore.dll",   "ntdll.dll",        "kernelbase.dll", "kernel32.dll", "ucrtbase.dll",
        "msvcrt.dll",   "vcruntime140.dll", "vcruntime140_1.dll", "msvcp140.dll", "combase.dll",
    };
    for (const char* name : kMachinery) {
        if (EqualsIgnoreCase(imageName, name)) {
            return true;
        }
    }
    return false;
}

int SelectReleaserFrame(const char* const* imageNames, size_t count) {
    if (!imageNames) {
        return -1;
    }
    bool enteredMachinery = false;
    for (size_t i = 0; i < count; ++i) {
        const bool machinery = IsReleaseMachineryModule(imageNames[i]);
        if (machinery) {
            enteredMachinery = true;
        } else if (enteredMachinery) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

const char* StateName(ChainState state) {
    switch (state) {
        case ChainState::kAlive:
            return "alive";
        case ChainState::kDestroyed:
            return "destroyed";
        case ChainState::kUnwatched:
        default:
            return "unwatched";
    }
}

bool Watch(IDXGISwapChain* swapchain, HWND window) {
    if (!swapchain) {
        return false;
    }
    const uintptr_t key = reinterpret_cast<uintptr_t>(swapchain);
    if (g_LastWitnessedAlive.load(std::memory_order_acquire) == key) {
        return true;
    }
    Registry& registry = TheRegistry();
    uint64_t generation = 0;
    {
        std::lock_guard<std::mutex> lock(registry.mutex);
        const auto it = registry.records.find(key);
        if (it != registry.records.end() && it->second.state == ChainState::kAlive) {
            if (!it->second.window && window) {
                it->second.window = window;
            }
            g_LastWitnessedAlive.store(key, std::memory_order_release);
            return true;
        }
        if (it == registry.records.end() && registry.records.size() >= kMaxRecords) {
            EvictOneLocked(registry);
        }
        // Recorded before DXGI holds the witness: a destruction racing this attach finds its generation.
        generation = registry.nextGeneration++;
        Record& record = registry.records[key];
        record = Record{};
        record.generation = generation;
        record.window = window;
        record.watchedAtMs = ce::hook_clock::TickCount64();
    }

    auto* witness = new Witness(key, generation);
    const HRESULT hr = swapchain->SetPrivateDataInterface(kWitnessGuid, witness);
    if (FAILED(hr)) {
        witness->Disarm();
        witness->Release();
        {
            std::lock_guard<std::mutex> lock(registry.mutex);
            const auto it = registry.records.find(key);
            if (it != registry.records.end() && it->second.generation == generation) {
                registry.records.erase(it);
            }
        }
        static std::atomic<uint32_t> s_refusedLogs{0};
        if (s_refusedLogs.fetch_add(1, std::memory_order_relaxed) < 8) {
            HookLogImportant("[SwapchainLifetime] DXGI refused the lifetime witness for sc=%p window=%p hr=0x%08X",
                             swapchain, window, static_cast<unsigned>(hr));
        }
        return false;
    }
    witness->Release();  // DXGI's private data keeps it until the chain is destroyed
    {
        // Published only while the record still names this witness: a destruction racing the attach wins.
        std::lock_guard<std::mutex> lock(registry.mutex);
        const auto it = registry.records.find(key);
        if (it != registry.records.end() && it->second.generation == generation &&
            it->second.state == ChainState::kAlive) {
            g_LastWitnessedAlive.store(key, std::memory_order_release);
        }
    }
    return true;
}

ChainReport Describe(const IDXGISwapChain* swapchain) {
    ChainReport report;
    if (!swapchain) {
        return report;
    }
    Registry& registry = TheRegistry();
    std::lock_guard<std::mutex> lock(registry.mutex);
    const auto it = registry.records.find(reinterpret_cast<uintptr_t>(swapchain));
    if (it == registry.records.end()) {
        return report;
    }
    const Record& record = it->second;
    report.state = record.state;
    report.window = record.window;
    report.watchedAtMs = record.watchedAtMs;
    report.generation = record.generation;
    report.destroyedAtMs = record.destroyedAtMs;
    report.destroyingThread = record.destroyingThread;
    snprintf(report.destroyedBy, sizeof(report.destroyedBy), "%s", record.destroyedBy);
    return report;
}

size_t LogWindowChains(HWND window, const char* stage) {
    struct Line {
        uintptr_t key;
        Record record;
    };
    std::vector<Line> lines;
    {
        Registry& registry = TheRegistry();
        std::lock_guard<std::mutex> lock(registry.mutex);
        for (const auto& [key, record] : registry.records) {
            if (record.window == window) {
                lines.push_back({key, record});
            }
        }
    }
    const uint64_t now = ce::hook_clock::TickCount64();
    size_t alive = 0;
    for (const Line& line : lines) {
        if (line.record.state == ChainState::kAlive) {
            ++alive;
        }
    }
    HookLogImportant("[SwapchainLifetime] %s window=%p witnessedChains=%zu alive=%zu",
                     stage ? stage : "report", window, lines.size(), alive);
    size_t logged = 0;
    for (const Line& line : lines) {
        if (++logged > 8) {
            break;
        }
        if (line.record.state == ChainState::kAlive) {
            HookLogImportant("[SwapchainLifetime] %s window=%p sc=%p state=alive watchedFor=%llums",
                             stage ? stage : "report", window, reinterpret_cast<void*>(line.key),
                             static_cast<unsigned long long>(now - line.record.watchedAtMs));
        } else {
            HookLogImportant(
                "[SwapchainLifetime] %s window=%p sc=%p state=destroyed %llums ago thread=%lu lastReleaseBy=%s",
                stage ? stage : "report", window, reinterpret_cast<void*>(line.key),
                static_cast<unsigned long long>(now - line.record.destroyedAtMs), line.record.destroyingThread,
                line.record.destroyedBy);
        }
    }
    return alive;
}

void ResetForTesting() {
    Registry& registry = TheRegistry();
    std::lock_guard<std::mutex> lock(registry.mutex);
    registry.records.clear();
    g_LastWitnessedAlive.store(0, std::memory_order_release);
}

}  // namespace ce::swapchain_lifetime_witness
