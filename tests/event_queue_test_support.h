// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall
#pragma once

#include "runtime/core/event_queue.h"

#include <gtest/gtest.h>
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

// Shared fixtures of the event queue tests: an allocator that counts and can fail, an injectable
// clock, and a copy of a delivered event that stays readable after its Release.
namespace event_queue_test {

struct HeapProbe {
    static inline std::atomic<int> live{0};         // blocks handed out and not yet released
    static inline std::atomic<int> allocations{0};  // calls, including failed ones
    static inline std::atomic<bool> failing{false};

    static void Reset() {
        live = 0;
        allocations = 0;
        failing = false;
    }
    static void* Allocate(size_t bytes) noexcept {
        allocations.fetch_add(1);
        if (failing.load())
            return nullptr;
        void* block = std::malloc(bytes);
        if (block)
            live.fetch_add(1);
        return block;
    }
    static void Release(void* block) noexcept {
        if (block) {
            live.fetch_sub(1);
            std::free(block);
        }
    }
    static ce::engine::EventQueue::Allocator Allocator() {
        return {&Allocate, &Release};
    }
};

struct FakeClock {
    static inline std::atomic<int64_t> nowUs{0};
    static int64_t Now() noexcept {
        return nowUs.load();
    }
};

struct Delivered {
    int32_t type = 0;
    uint32_t structSize = 0;
    uint64_t sequence = 0;
    int64_t timeUs = 0;
    ce_request_t request = 0;
    ce_status_t status = 0;
    bool hasMessage = false;
    std::string message;
    bool hasOutputPath = false;
    std::string outputPath;
    std::vector<std::string> paths;
    uint32_t pathCount = 0;
    ce_event_t raw{};  // scalar union members; its pointers dangle after Release
};

inline Delivered Copy(const ce_event_t& event) {
    Delivered copy;
    copy.type = event.type;
    copy.structSize = event.struct_size;
    copy.sequence = event.sequence;
    copy.timeUs = event.time_us;
    copy.request = event.request;
    copy.status = event.status;
    copy.raw = event;
    if (event.message) {
        copy.hasMessage = true;
        copy.message = event.message;
    }
    if (event.type == CE_EVENT_RECORDING_FINALIZED && event.data.finalized.output_path) {
        copy.hasOutputPath = true;
        copy.outputPath = event.data.finalized.output_path;
    }
    if (event.type == CE_EVENT_SCREENSHOT) {
        copy.pathCount = event.data.screenshot.path_count;
        for (uint32_t i = 0; i < event.data.screenshot.path_count; ++i)
            copy.paths.emplace_back(event.data.screenshot.paths[i]);
    }
    return copy;
}

inline std::unique_ptr<ce::engine::EventQueue> MakeQueue(uint32_t capacity, bool probe = true, bool fakeClock = true) {
    ce::engine::EventQueue::Options options;
    options.capacity = capacity;
    if (probe)
        options.allocator = HeapProbe::Allocator();
    if (fakeClock)
        options.clock = &FakeClock::Now;
    ce_status_t status = CE_E_INTERNAL;
    auto queue = ce::engine::EventQueue::Create(options, &status);
    EXPECT_EQ(status, CE_OK);
    return queue;
}

// Delivers and releases everything currently deliverable, in delivery order.
inline std::vector<Delivered> Drain(ce::engine::EventQueue& queue) {
    std::vector<Delivered> all;
    for (;;) {
        const ce_event_t* event = nullptr;
        const ce_status_t status = queue.Next(0, &event);
        if (status == CE_E_TIMEOUT)
            break;
        EXPECT_EQ(status, CE_OK);
        if (status != CE_OK || !event)
            break;
        all.push_back(Copy(*event));
        EXPECT_EQ(queue.Release(event), CE_OK);
    }
    return all;
}

inline bool Signaled(const ce::engine::EventQueue& queue) {
    return WaitForSingleObject(queue.Handle(), 0) == WAIT_OBJECT_0;
}

}  // namespace event_queue_test
