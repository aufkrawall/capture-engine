#pragma once

#include <atomic>
#include <cstdint>
#include <limits>
#include <memory>
#include <utility>

struct WgcPoolLeaseState {
    std::unique_ptr<std::atomic<uint32_t>[]> slotLeases;
    uint32_t slotCount = 0;
    uint64_t generation = 0;
    std::atomic<uint32_t> leasedCurrent{0};
    std::atomic<uint32_t> leasedMax{0};
    std::atomic<uint32_t> freeMin{0};
    std::atomic<uint32_t> releaseMismatchCount{0};

    void Init(uint32_t count, uint64_t gen) {
        slotCount = count;
        generation = gen;
        slotLeases = count > 0 ? std::make_unique<std::atomic<uint32_t>[]>(count) : nullptr;
        for (uint32_t i = 0; i < count; ++i) {
            slotLeases[i].store(0, std::memory_order_relaxed);
        }
        leasedCurrent.store(0, std::memory_order_relaxed);
        leasedMax.store(0, std::memory_order_relaxed);
        freeMin.store(count, std::memory_order_relaxed);
        releaseMismatchCount.store(0, std::memory_order_relaxed);
    }

    bool TryAcquire(uint32_t slot) {
        if (!slotLeases || slot >= slotCount) {
            return false;
        }
        uint32_t expected = 0;
        if (!slotLeases[slot].compare_exchange_strong(expected, 1, std::memory_order_acq_rel,
                                                      std::memory_order_relaxed)) {
            return false;
        }
        const uint32_t active = leasedCurrent.fetch_add(1, std::memory_order_acq_rel) + 1;
        UpdateMax(leasedMax, active);
        const uint32_t freeSlots = active >= slotCount ? 0u : (slotCount - active);
        UpdateMin(freeMin, freeSlots);
        return true;
    }

    void Release(uint32_t slot, uint64_t gen) {
        if (gen != generation || !slotLeases || slot >= slotCount) {
            releaseMismatchCount.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        const uint32_t prev = slotLeases[slot].fetch_sub(1, std::memory_order_acq_rel);
        if (prev == 0) {
            slotLeases[slot].store(0, std::memory_order_release);
            releaseMismatchCount.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        leasedCurrent.fetch_sub(1, std::memory_order_acq_rel);
    }

private:
    static void UpdateMax(std::atomic<uint32_t>& value, uint32_t sample) {
        auto current = value.load(std::memory_order_relaxed);
        while (sample > current &&
               !value.compare_exchange_weak(current, sample, std::memory_order_relaxed, std::memory_order_relaxed)) {}
    }

    static void UpdateMin(std::atomic<uint32_t>& value, uint32_t sample) {
        auto current = value.load(std::memory_order_relaxed);
        while (sample < current &&
               !value.compare_exchange_weak(current, sample, std::memory_order_relaxed, std::memory_order_relaxed)) {}
    }
};

class WgcPoolSlotLease {
public:
    WgcPoolSlotLease() = default;
    WgcPoolSlotLease(std::shared_ptr<WgcPoolLeaseState> state, uint32_t slot, uint64_t generation)
        : state_(std::move(state)), slot_(slot), generation_(generation) {}
    ~WgcPoolSlotLease() {
        Reset();
    }

    WgcPoolSlotLease(const WgcPoolSlotLease&) = delete;
    WgcPoolSlotLease& operator=(const WgcPoolSlotLease&) = delete;
    WgcPoolSlotLease(WgcPoolSlotLease&& other) noexcept {
        *this = std::move(other);
    }
    WgcPoolSlotLease& operator=(WgcPoolSlotLease&& other) noexcept {
        if (this != &other) {
            Reset();
            state_ = std::move(other.state_);
            slot_ = other.slot_;
            generation_ = other.generation_;
            other.slot_ = std::numeric_limits<uint32_t>::max();
            other.generation_ = 0;
        }
        return *this;
    }

    void Reset() {
        if (state_) {
            state_->Release(slot_, generation_);
            state_.reset();
        }
        slot_ = std::numeric_limits<uint32_t>::max();
        generation_ = 0;
    }
    bool IsValid() const {
        return state_ != nullptr && slot_ != std::numeric_limits<uint32_t>::max() && generation_ != 0;
    }
    uint32_t Slot() const {
        return slot_;
    }
    uint64_t Generation() const {
        return generation_;
    }

private:
    std::shared_ptr<WgcPoolLeaseState> state_;
    uint32_t slot_ = std::numeric_limits<uint32_t>::max();
    uint64_t generation_ = 0;
};
