#include "mediaengine_handles.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <iterator>
#include <mutex>
#include <vector>

#include "mediaengine.h"
#include "mediaengine_instance.h"

// A handle is generation << 32 | (slot number), so a stale handle names a slot whose generation has moved on.
static_assert(sizeof(MediaEngineHandle) == 8, "a handle carries a 32-bit generation and a 32-bit slot number");

// One pipeline slot. Pinning reads only these atomics; Register and Retire write them under g_registryMutex.
struct PipelineSlot {
    // Generation of the live pipeline; 0 while the slot is free or retiring.
    std::atomic<uint32_t> generation{0};
    // Calls and thread holds pinning the pipeline. Retire waits for zero before it hands the pipeline back.
    std::atomic<uint32_t> pins{0};
    // Set by Retire while it waits, so the pin that releases last wakes it. Pins released before that see false and
    // never take the mutex, which keeps pinning lock-free.
    std::atomic<bool> retiring{false};
    // Written before `generation` is published, and read only by a pin that saw that generation.
    MediaEngineInstance* instance = nullptr;
};

namespace {

constexpr uint32_t kMaxPipelines = 16;

PipelineSlot g_slots[kMaxPipelines];
std::mutex g_registryMutex;  // Register, Retire and the wake-up of a retiring pipeline
std::condition_variable g_pinsReleased;
uint32_t g_lastGeneration[kMaxPipelines] = {};  // guarded by g_registryMutex

thread_local MediaEngineLogRoute t_currentRoute;

struct ThreadHold {
    MediaEngineHandle handle;
    PipelineSlot* slot;
};
thread_local std::vector<ThreadHold> t_holds;

uint32_t SlotNumberOf(MediaEngineHandle handle) {
    return static_cast<uint32_t>(handle & 0xffffffffu);
}

uint32_t GenerationOf(MediaEngineHandle handle) {
    return static_cast<uint32_t>(handle >> 32);
}

MediaEngineHandle HandleOf(uint32_t slotNumber, uint32_t generation) {
    return (static_cast<MediaEngineHandle>(generation) << 32) | slotNumber;
}

void Unpin(PipelineSlot& slot) {
    if (slot.pins.fetch_sub(1) == 1 && slot.retiring.load()) {
        std::lock_guard<std::mutex> lock(g_registryMutex);
        g_pinsReleased.notify_all();
    }
}

// Pins the live pipeline `handle` names, or returns null when there is none. The caller owes one Unpin.
PipelineSlot* PinPipeline(MediaEngineHandle handle) {
    const uint32_t slotNumber = SlotNumberOf(handle);
    const uint32_t generation = GenerationOf(handle);
    if (slotNumber == 0 || slotNumber > kMaxPipelines || generation == 0)
        return nullptr;
    PipelineSlot& slot = g_slots[slotNumber - 1];
    slot.pins.fetch_add(1);
    if (slot.generation.load() == generation)
        return &slot;
    Unpin(slot);
    return nullptr;
}

void LogRefusalOnce(MediaEngineHandle handle) {
    static std::atomic<bool> s_logged{false};
    if (s_logged.exchange(true))
        return;
    DLL_Log("[MediaEngine] Call refused: handle 0x%llx is zero, unknown or destroyed (later refusals are not logged)",
            static_cast<unsigned long long>(handle));
}

}  // namespace

MediaEngineLogRoute CurrentMediaEngineLogRoute() {
    return t_currentRoute;
}

MediaEngineLogRouteScope::MediaEngineLogRouteScope(MediaEngineLogRoute route) : previous_(t_currentRoute) {
    t_currentRoute = route;
}

MediaEngineLogRouteScope::~MediaEngineLogRouteScope() {
    t_currentRoute = previous_;
}

MediaEngineLease::MediaEngineLease(MediaEngineHandle handle) : previousRoute_(t_currentRoute) {
    t_currentRoute = {};
    slot_ = PinPipeline(handle);
    if (slot_) {
        instance_ = slot_->instance;
        t_currentRoute = instance_->logRoute;
    } else {
        LogRefusalOnce(handle);
    }
}

MediaEngineLease::~MediaEngineLease() {
    t_currentRoute = previousRoute_;
    if (slot_)
        Unpin(*slot_);
}

MediaEngineInstance* HoldPipeline(MediaEngineHandle handle) {
    PipelineSlot* slot = PinPipeline(handle);
    if (!slot) {
        LogRefusalOnce(handle);
        return nullptr;
    }
    t_holds.push_back({handle, slot});
    return slot->instance;
}

MediaEngineInstance* HeldPipeline(MediaEngineHandle handle) {
    for (auto hold = t_holds.rbegin(); hold != t_holds.rend(); ++hold) {
        if (hold->handle == handle)
            return hold->slot->instance;
    }
    return nullptr;
}

void ReleasePipelineHold(MediaEngineHandle handle) {
    for (auto hold = t_holds.rbegin(); hold != t_holds.rend(); ++hold) {
        if (hold->handle != handle)
            continue;
        PipelineSlot* slot = hold->slot;
        t_holds.erase(std::next(hold).base());
        Unpin(*slot);
        return;
    }
}

MediaEngineHandle RegisterMediaEngineInstance(std::unique_ptr<MediaEngineInstance> instance) {
    std::lock_guard<std::mutex> lock(g_registryMutex);
    for (uint32_t index = 0; index < kMaxPipelines; ++index) {
        PipelineSlot& slot = g_slots[index];
        // A retiring slot is busy until its Retire has finished waiting: Retire releases the registry lock meanwhile.
        if (slot.generation.load() != 0 || slot.retiring.load())
            continue;
        if (++g_lastGeneration[index] == 0)  // generation zero is never a live handle
            ++g_lastGeneration[index];
        const uint32_t generation = g_lastGeneration[index];
        slot.instance = instance.release();
        slot.generation.store(generation);
        return HandleOf(index + 1, generation);
    }
    DLL_Log("[MediaEngine] Cannot create a pipeline: all %u handle slots are in use", kMaxPipelines);
    return 0;
}

std::unique_ptr<MediaEngineInstance> RetireMediaEngineInstance(MediaEngineHandle handle) {
    const uint32_t slotNumber = SlotNumberOf(handle);
    if (slotNumber == 0 || slotNumber > kMaxPipelines)
        return nullptr;
    PipelineSlot& slot = g_slots[slotNumber - 1];
    std::unique_lock<std::mutex> lock(g_registryMutex);
    uint32_t expected = GenerationOf(handle);
    if (expected == 0 || !slot.generation.compare_exchange_strong(expected, 0))
        return nullptr;  // unknown or already retired
    // New pins now fail the generation check. The seq_cst order of the generation change, the retiring flag and the
    // pin counter guarantees that either a pin is counted here or its release sees the flag and wakes this wait.
    slot.retiring.store(true);
    g_pinsReleased.wait(lock, [&slot] { return slot.pins.load() == 0; });
    slot.retiring.store(false);
    MediaEngineInstance* instance = slot.instance;
    slot.instance = nullptr;
    return std::unique_ptr<MediaEngineInstance>(instance);
}
