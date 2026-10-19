// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall
#include "tests/event_queue_test_support.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <latch>
#include <random>
#include <thread>
#include <vector>

// Exactly-once completion delivery (llm-wiki/library/07: "every accepted request yields exactly one
// COMMAND_COMPLETED") under random interleavings of reserve / push / next / release from several
// threads. No sleeps: threads start on a latch and consumers block in Next(), whose timeout only
// bounds a failure.
namespace {

using ce::engine::EventQueue;
using namespace event_queue_test;

class EngineEventQueueConcurrencyTest : public ::testing::Test {
protected:
    void SetUp() override {
        HeapProbe::Reset();
    }
    void TearDown() override {
        EXPECT_EQ(HeapProbe::live.load(), 0);
    }
};

struct RunShape {
    uint32_t seed;
    int producers;
    int consumers;
    int requestsPerProducer;
    uint32_t capacity;
};

constexpr int32_t kStateTypes[] = {CE_EVENT_RUNTIME_STATE, CE_EVENT_RECORDING_STATE, CE_EVENT_SETTINGS_CHANGED,
                                   CE_EVENT_HELPER, CE_EVENT_HOTKEY};

void RunExactlyOnceProperty(const RunShape& shape) {
    SCOPED_TRACE(::testing::Message() << "seed=" << shape.seed << " producers=" << shape.producers
                                      << " consumers=" << shape.consumers << " capacity=" << shape.capacity);
    auto queue = MakeQueue(shape.capacity, true, false);
    ASSERT_NE(queue, nullptr);
    const size_t total = static_cast<size_t>(shape.producers) * shape.requestsPerProducer;

    std::vector<uint8_t> admitted(total + 1, 0);  // each producer writes its own ids; read after join
    std::vector<std::atomic<uint32_t>> completions(total + 1);
    std::atomic<uint64_t> statePushes{0};
    std::atomic<uint64_t> stateDelivered{0};
    std::atomic<uint64_t> announcedDrops{0};
    std::atomic<int> producersDone{0};
    std::atomic<int> consumerFailures{0};
    std::latch start(shape.producers + shape.consumers);

    std::vector<std::vector<uint64_t>> sequences(shape.consumers);
    std::vector<std::thread> threads;
    for (int c = 0; c < shape.consumers; ++c) {
        threads.emplace_back([&, c] {
            std::mt19937 rng(shape.seed * 7919u + static_cast<uint32_t>(c));
            std::vector<const ce_event_t*> held;
            uint64_t lastSequence = 0;
            start.arrive_and_wait();
            for (;;) {
                const bool producersFinished = producersDone.load() == shape.producers;
                const ce_event_t* event = nullptr;
                const ce_status_t status = queue->Next(20, &event);
                if (status == CE_E_TIMEOUT) {
                    if (producersFinished)
                        break;
                    continue;
                }
                if (status != CE_OK || !event) {
                    consumerFailures.fetch_add(1);
                    break;
                }
                if (event->sequence <= lastSequence)
                    consumerFailures.fetch_add(1);  // a consumer's own deliveries must increase
                lastSequence = event->sequence;
                sequences[c].push_back(event->sequence);
                if (event->type == CE_EVENT_COMMAND_COMPLETED) {
                    if (event->request >= 1 && event->request <= total)
                        completions[event->request].fetch_add(1);
                    else
                        consumerFailures.fetch_add(1);
                } else if (event->type == CE_EVENT_EVENTS_DROPPED) {
                    announcedDrops.fetch_add(event->data.dropped.count);
                } else {
                    stateDelivered.fetch_add(1);
                }
                held.push_back(event);
                while (held.size() > rng() % 4) {  // a client that sometimes lags behind with Release
                    if (queue->Release(held.front()) != CE_OK)
                        consumerFailures.fetch_add(1);
                    held.erase(held.begin());
                }
            }
            for (const ce_event_t* event : held) {
                if (queue->Release(event) != CE_OK)
                    consumerFailures.fetch_add(1);
            }
        });
    }
    for (int p = 0; p < shape.producers; ++p) {
        threads.emplace_back([&, p] {
            std::mt19937 rng(shape.seed * 104729u + static_cast<uint32_t>(p));
            start.arrive_and_wait();
            for (int r = 0; r < shape.requestsPerProducer; ++r) {
                const size_t id = static_cast<size_t>(p) * shape.requestsPerProducer + r + 1;
                auto reservation = queue->Reserve();
                const uint32_t burst = rng() % 4;
                for (uint32_t k = 0; k < burst; ++k) {
                    ce::engine::EventDraft draft;
                    draft.type = kStateTypes[rng() % std::size(kStateTypes)];
                    (void)queue->PushState(draft);
                    statePushes.fetch_add(1);
                }
                if (!reservation)
                    continue;  // admission refused: the command would be rejected, no completion owed
                if (rng() % 6 == 0) {
                    reservation.Cancel();  // admission failed after the reservation: no completion owed
                    continue;
                }
                admitted[id] = 1;
                EXPECT_TRUE(
                    queue->PushReserved(std::move(reservation), ce::engine::CommandCompletedEvent(id, CE_OK, "done")));
            }
            producersDone.fetch_add(1);
        });
    }
    for (std::thread& thread : threads)
        thread.join();

    EXPECT_EQ(consumerFailures.load(), 0);
    size_t admittedCount = 0;
    for (size_t id = 1; id <= total; ++id) {
        EXPECT_EQ(completions[id].load(), admitted[id] ? 1u : 0u) << "request " << id;
        admittedCount += admitted[id];
    }
    EXPECT_GT(admittedCount, 0u);

    std::vector<uint64_t> all;
    for (const auto& list : sequences)
        all.insert(all.end(), list.begin(), list.end());
    std::sort(all.begin(), all.end());
    EXPECT_TRUE(std::adjacent_find(all.begin(), all.end()) == all.end()) << "sequence numbers must be unique";

    const EventQueue::Stats stats = queue->GetStats();
    EXPECT_EQ(stats.pending, 0u);
    EXPECT_EQ(stats.outstanding, 0u);
    EXPECT_EQ(stats.reserved, 0u);
    EXPECT_EQ(stats.unannounced, 0u) << "every dropped event must have been announced";
    EXPECT_EQ(announcedDrops.load(), stats.droppedTotal);
    EXPECT_EQ(statePushes.load(), stateDelivered.load() + stats.droppedTotal)
        << "every state push is either delivered or counted as dropped";
}

TEST_F(EngineEventQueueConcurrencyTest, EveryAdmittedRequestCompletesExactlyOnceRoomyQueue) {
    for (uint32_t seed = 1; seed <= 3; ++seed)
        RunExactlyOnceProperty({seed, 4, 3, 1500, EventQueue::kDefaultCapacity});
}

TEST_F(EngineEventQueueConcurrencyTest, EveryAdmittedRequestCompletesExactlyOnceUnderConstantOverflow) {
    for (uint32_t seed = 1; seed <= 4; ++seed)
        RunExactlyOnceProperty({seed, 4, 2, 1200, 8});
}

TEST_F(EngineEventQueueConcurrencyTest, EveryAdmittedRequestCompletesExactlyOnceWithATinyBudget) {
    for (uint32_t seed = 1; seed <= 3; ++seed)
        RunExactlyOnceProperty({seed, 3, 1, 800, 3});
    RunExactlyOnceProperty({9, 2, 4, 1000, 64});
}

TEST_F(EngineEventQueueConcurrencyTest, EveryBlockedConsumerWakesForExactlyOneEvent) {
    constexpr int kConsumers = 6;
    auto queue = MakeQueue(32, true, false);
    std::vector<ce_status_t> status(kConsumers, CE_E_INTERNAL);
    std::vector<uint64_t> sequence(kConsumers, 0);
    std::latch started(kConsumers);
    std::vector<std::thread> consumers;
    for (int c = 0; c < kConsumers; ++c) {
        consumers.emplace_back([&, c] {
            started.arrive_and_wait();
            const ce_event_t* event = nullptr;
            status[c] = queue->Next(30000, &event);  // bounds a lost wake-up; not an expected duration
            if (status[c] == CE_OK) {
                sequence[c] = event->sequence;
                EXPECT_EQ(queue->Release(event), CE_OK);
            }
        });
    }
    for (int i = 0; i < kConsumers; ++i)
        ASSERT_TRUE(queue->PushState(ce::engine::HotkeyEvent(i)));
    for (std::thread& consumer : consumers)
        consumer.join();
    for (int c = 0; c < kConsumers; ++c)
        EXPECT_EQ(status[c], CE_OK) << "consumer " << c;
    std::sort(sequence.begin(), sequence.end());
    EXPECT_TRUE(std::adjacent_find(sequence.begin(), sequence.end()) == sequence.end());
    EXPECT_EQ(queue->GetStats().pending, 0u);
}

TEST_F(EngineEventQueueConcurrencyTest, StalledClientLosesOnlyStateEventsAndNeverBlocksTheProducer) {
    auto queue = MakeQueue(16, true, false);  // nobody calls Next while the producer works
    // Budget 16 = 11 completions + the newest state event of each of the five types, which keeps its slot.
    std::vector<ce_request_t> expected;
    for (int round = 0; round < 11; ++round) {
        for (int i = 0; i < 500; ++i) {
            ce::engine::EventDraft draft;
            draft.type = kStateTypes[static_cast<size_t>(i) % std::size(kStateTypes)];
            (void)queue->PushState(draft);
        }
        auto reservation = queue->Reserve();
        ASSERT_TRUE(reservation) << "round " << round << ": state events must give way to admission";
        const ce_request_t id = 5000 + round;
        expected.push_back(id);
        ASSERT_TRUE(queue->PushReserved(std::move(reservation), ce::engine::CommandCompletedEvent(id, CE_OK)));
    }
    for (int i = 0; i < 500; ++i) {
        ce::engine::EventDraft draft;
        draft.type = kStateTypes[static_cast<size_t>(i) % std::size(kStateTypes)];
        (void)queue->PushState(draft);
    }
    EXPECT_FALSE(queue->Reserve()) << "the twelfth command finds the budget really full";
    std::vector<ce_request_t> completed;
    for (const Delivered& event : Drain(*queue)) {
        if (event.type == CE_EVENT_COMMAND_COMPLETED)
            completed.push_back(event.request);
    }
    EXPECT_EQ(completed, expected) << "completions arrive complete and in admission order";
}

}  // namespace
