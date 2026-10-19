// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall
#include "tests/event_queue_test_support.h"

#include <gtest/gtest.h>

#include <string>
#include <thread>
#include <vector>

// The event queue's contract (llm-wiki/library/03 "Events", 07 "Commands/events") through its public
// interface only. Concurrency lives in test_engine_event_queue_concurrency.cpp.
namespace {

using ce::engine::EventDraft;
using ce::engine::EventQueue;
using namespace event_queue_test;

class EngineEventQueueTest : public ::testing::Test {
protected:
    void SetUp() override {
        HeapProbe::Reset();
        FakeClock::nowUs = 0;
    }
    void TearDown() override {
        EXPECT_EQ(HeapProbe::live.load(), 0) << "every record must be freed with its queue";
    }
};

TEST_F(EngineEventQueueTest, CreateRejectsBadOptions) {
    ce_status_t status = CE_OK;
    EventQueue::Options options;
    options.capacity = 0;
    EXPECT_EQ(EventQueue::Create(options, &status), nullptr);
    EXPECT_EQ(status, CE_E_INVALID_ARGUMENT);
    options.capacity = EventQueue::kMaxCapacity + 1;
    EXPECT_EQ(EventQueue::Create(options, &status), nullptr);
    EXPECT_EQ(status, CE_E_INVALID_ARGUMENT);
    options.capacity = 4;
    options.allocator.allocate = &HeapProbe::Allocate;  // release missing
    EXPECT_EQ(EventQueue::Create(options, &status), nullptr);
    EXPECT_EQ(status, CE_E_INVALID_ARGUMENT);
    options.allocator = {};
    EXPECT_NE(EventQueue::Create(options, &status), nullptr);
    EXPECT_EQ(status, CE_OK);
}

TEST_F(EngineEventQueueTest, EmptyQueuePollsTimeoutAndClearsOutput) {
    auto queue = MakeQueue(4);
    const ce_event_t junk{};
    const ce_event_t* out = &junk;
    EXPECT_EQ(queue->Next(0, &out), CE_E_TIMEOUT);
    EXPECT_EQ(out, nullptr);
    out = &junk;
    EXPECT_EQ(queue->Next(1, &out), CE_E_TIMEOUT);  // a bounded wait that expires
    EXPECT_EQ(out, nullptr);
    EXPECT_FALSE(Signaled(*queue));
}

TEST_F(EngineEventQueueTest, NextRejectsInfiniteTimeoutAndNullOutput) {
    auto queue = MakeQueue(4);
    ASSERT_TRUE(queue->PushState(ce::engine::HotkeyEvent(CE_HOTKEY_RECORD)));
    const ce_event_t junk{};
    const ce_event_t* out = &junk;
    EXPECT_EQ(queue->Next(UINT32_MAX, &out), CE_E_INVALID_ARGUMENT);
    EXPECT_EQ(out, nullptr);
    EXPECT_EQ(queue->Next(0, nullptr), CE_E_INVALID_ARGUMENT);
    // The rejected calls must not have consumed the event.
    EXPECT_EQ(queue->GetStats().pending, 1u);
    EXPECT_EQ(queue->GetStats().outstanding, 0u);
    EXPECT_EQ(Drain(*queue).size(), 1u);
}

TEST_F(EngineEventQueueTest, HandleIsSignaledExactlyWhileEventsArePending) {
    auto queue = MakeQueue(8);
    ASSERT_NE(queue->Handle(), nullptr);
    EXPECT_FALSE(Signaled(*queue));

    auto reservation = queue->Reserve();
    ASSERT_TRUE(reservation);
    EXPECT_FALSE(Signaled(*queue)) << "a reservation is not an event";
    ASSERT_TRUE(queue->PushReserved(std::move(reservation), ce::engine::CommandCompletedEvent(1, CE_OK)));
    EXPECT_TRUE(Signaled(*queue));
    ASSERT_TRUE(queue->PushState(ce::engine::HotkeyEvent(CE_HOTKEY_RECORD)));
    EXPECT_TRUE(Signaled(*queue));

    const ce_event_t* first = nullptr;
    ASSERT_EQ(queue->Next(0, &first), CE_OK);
    EXPECT_TRUE(Signaled(*queue)) << "one event is still pending";
    const ce_event_t* second = nullptr;
    ASSERT_EQ(queue->Next(0, &second), CE_OK);
    EXPECT_FALSE(Signaled(*queue)) << "delivered-but-unreleased events are not pending";
    EXPECT_EQ(queue->Release(first), CE_OK);
    EXPECT_EQ(queue->Release(second), CE_OK);
    EXPECT_FALSE(Signaled(*queue));

    auto cancelled = queue->Reserve();
    ASSERT_TRUE(cancelled);
    cancelled.Cancel();
    EXPECT_FALSE(Signaled(*queue));
}

TEST_F(EngineEventQueueTest, EventsKeepTheirFieldsAndOwnTheirStrings) {
    auto queue = MakeQueue(16);
    FakeClock::nowUs = 1500;
    {
        std::string message = "start rejected: no media helper";
        auto reservation = queue->Reserve();
        ASSERT_TRUE(
            queue->PushReserved(std::move(reservation), ce::engine::CommandCompletedEvent(77, CE_E_PROCESS, message)));
        message.assign(message.size(), 'X');  // the queue must have copied it
    }
    FakeClock::nowUs = 2500;
    {
        const std::string path = "C:\\captures\\rec 01.mkv";
        auto reservation = queue->Reserve(EventQueue::kDefaultReservedTextBytes, 0);
        ASSERT_TRUE(queue->PushReserved(std::move(reservation),
                                        ce::engine::RecordingFinalizedEvent(9, CE_E_IO, path, "disk full")));
    }
    FakeClock::nowUs = 3500;
    {
        const std::string a = "C:\\shots\\a.png";
        const std::string b = "C:\\shots\\b_hdr.png";
        const std::string_view paths[] = {a, b};
        auto reservation = queue->Reserve(EventQueue::kDefaultReservedTextBytes, 2);
        ASSERT_TRUE(queue->PushReserved(
            std::move(reservation), ce::engine::ScreenshotEvent(5, CE_OK, std::span<const std::string_view>(paths))));
    }
    ASSERT_TRUE(queue->PushState(ce::engine::RecordingStateEvent(3, 42, CE_RECORDING_LIVE, CE_MODE_VIDEO, 0)));
    ASSERT_TRUE(queue->PushState(ce::engine::SettingsChangedEvent(8, CE_SETTINGS_SOURCE_API)));
    ASSERT_TRUE(queue->PushState(ce::engine::HelperEvent(CE_HELPER_LOGGER, CE_HELPER_STATE_LOST, "logger exited")));

    const std::vector<Delivered> events = Drain(*queue);
    ASSERT_EQ(events.size(), 6u);

    EXPECT_EQ(events[0].type, CE_EVENT_COMMAND_COMPLETED);
    EXPECT_EQ(events[0].structSize, sizeof(ce_event_t));
    EXPECT_EQ(events[0].sequence, 1u);
    EXPECT_EQ(events[0].timeUs, 1500);
    EXPECT_EQ(events[0].request, 77u);
    EXPECT_EQ(events[0].status, CE_E_PROCESS);
    EXPECT_EQ(events[0].raw.reserved0, 0u);
    EXPECT_TRUE(events[0].hasMessage);
    EXPECT_EQ(events[0].message, "start rejected: no media helper");

    EXPECT_EQ(events[1].type, CE_EVENT_RECORDING_FINALIZED);
    EXPECT_EQ(events[1].timeUs, 2500);
    EXPECT_EQ(events[1].status, CE_E_IO);
    EXPECT_EQ(events[1].raw.data.finalized.recording_id, 9u);
    EXPECT_TRUE(events[1].hasOutputPath);
    EXPECT_EQ(events[1].outputPath, "C:\\captures\\rec 01.mkv");
    EXPECT_EQ(events[1].message, "disk full");

    EXPECT_EQ(events[2].type, CE_EVENT_SCREENSHOT);
    EXPECT_EQ(events[2].request, 5u);
    ASSERT_EQ(events[2].paths.size(), 2u);
    EXPECT_EQ(events[2].paths[0], "C:\\shots\\a.png");
    EXPECT_EQ(events[2].paths[1], "C:\\shots\\b_hdr.png");
    EXPECT_FALSE(events[2].hasMessage);
    EXPECT_EQ(events[2].raw.message, nullptr);

    EXPECT_EQ(events[3].type, CE_EVENT_RECORDING_STATE);
    EXPECT_EQ(events[3].request, 3u);
    EXPECT_EQ(events[3].raw.data.recording.recording_id, 42u);
    EXPECT_EQ(events[3].raw.data.recording.state, CE_RECORDING_LIVE);
    EXPECT_EQ(events[3].raw.data.recording.mode, CE_MODE_VIDEO);
    EXPECT_EQ(events[4].type, CE_EVENT_SETTINGS_CHANGED);
    EXPECT_EQ(events[4].raw.data.settings.revision, 8u);
    EXPECT_EQ(events[5].type, CE_EVENT_HELPER);
    EXPECT_EQ(events[5].raw.data.helper.helper, static_cast<uint32_t>(CE_HELPER_LOGGER));
    EXPECT_EQ(events[5].message, "logger exited");
    for (size_t i = 0; i < events.size(); ++i)
        EXPECT_EQ(events[i].sequence, i + 1) << "sequence counts deliveries from 1";
}

TEST_F(EngineEventQueueTest, DraftPointerMembersNeverLeakIntoEvents) {
    auto queue = MakeQueue(4);
    EventDraft finalized = ce::engine::RecordingFinalizedEvent(1, CE_OK, "");
    finalized.payload.finalized.output_path = reinterpret_cast<const char*>(0x1234);  // producer junk
    auto reservation = queue->Reserve();
    ASSERT_TRUE(queue->PushReserved(std::move(reservation), finalized));
    EventDraft shot = ce::engine::ScreenshotEvent(1, CE_OK, {});
    shot.payload.screenshot.path_count = 9;
    shot.payload.screenshot.paths = reinterpret_cast<const char* const*>(0x5678);
    auto second = queue->Reserve();
    ASSERT_TRUE(queue->PushReserved(std::move(second), shot));
    const std::vector<Delivered> events = Drain(*queue);
    ASSERT_EQ(events.size(), 2u);
    EXPECT_EQ(events[0].raw.data.finalized.output_path, nullptr);
    EXPECT_EQ(events[1].raw.data.screenshot.paths, nullptr);
    EXPECT_EQ(events[1].raw.data.screenshot.path_count, 0u);
}

TEST_F(EngineEventQueueTest, DefaultClockStampsNonDecreasingMicroseconds) {
    auto queue = MakeQueue(8, true, false);
    ASSERT_TRUE(queue->PushState(ce::engine::HotkeyEvent(1)));
    ASSERT_TRUE(queue->PushState(ce::engine::HotkeyEvent(2)));
    const std::vector<Delivered> events = Drain(*queue);
    ASSERT_EQ(events.size(), 2u);
    EXPECT_GT(events[0].timeUs, 0);
    EXPECT_GE(events[1].timeUs, events[0].timeUs);
}

TEST_F(EngineEventQueueTest, ReleaseOnlyAcceptsOutstandingEventsOfThisQueue) {
    auto queue = MakeQueue(8);
    auto other = MakeQueue(8);
    ASSERT_TRUE(queue->PushState(ce::engine::HotkeyEvent(1)));
    ASSERT_TRUE(other->PushState(ce::engine::HotkeyEvent(1)));
    const ce_event_t* mine = nullptr;
    const ce_event_t* foreign = nullptr;
    ASSERT_EQ(queue->Next(0, &mine), CE_OK);
    ASSERT_EQ(other->Next(0, &foreign), CE_OK);

    const ce_event_t onStack{};
    EXPECT_EQ(queue->Release(nullptr), CE_E_INVALID_ARGUMENT);
    EXPECT_EQ(queue->Release(&onStack), CE_E_INVALID_ARGUMENT);
    EXPECT_EQ(queue->Release(foreign), CE_E_INVALID_ARGUMENT);
    EXPECT_EQ(queue->GetStats().outstanding, 1u) << "refused releases must not touch the queue";
    EXPECT_EQ(other->GetStats().outstanding, 1u);

    EXPECT_EQ(queue->Release(mine), CE_OK);
    EXPECT_EQ(queue->Release(mine), CE_E_INVALID_ARGUMENT) << "a second release of the same event";
    EXPECT_EQ(queue->GetStats().outstanding, 0u);
    EXPECT_EQ(other->Release(foreign), CE_OK);
}

TEST_F(EngineEventQueueTest, UnreleasedEventsCountAgainstTheBudget) {
    auto queue = MakeQueue(3);
    for (int32_t i = 1; i <= 3; ++i)
        ASSERT_TRUE(queue->PushState(ce::engine::HotkeyEvent(i)));
    const ce_event_t* events[3] = {};
    for (const ce_event_t*& event : events)
        ASSERT_EQ(queue->Next(0, &event), CE_OK);
    EXPECT_FALSE(queue->Reserve()) << "three unreleased events fill a budget of three";
    EXPECT_EQ(queue->Release(events[0]), CE_OK);
    auto reservation = queue->Reserve();
    EXPECT_TRUE(reservation);
    EXPECT_FALSE(queue->Reserve());
    reservation.Cancel();
    EXPECT_EQ(queue->Release(events[1]), CE_OK);
    EXPECT_EQ(queue->Release(events[2]), CE_OK);
}

TEST_F(EngineEventQueueTest, DestructionFreesPendingAndUnreleasedEvents) {
    {
        auto queue = MakeQueue(16);
        for (int32_t i = 1; i <= 4; ++i)
            ASSERT_TRUE(queue->PushState(ce::engine::HotkeyEvent(i)));
        auto reservation = queue->Reserve();
        ASSERT_TRUE(queue->PushReserved(std::move(reservation), ce::engine::CommandCompletedEvent(1, CE_OK, "x")));
        const ce_event_t* delivered[2] = {};
        ASSERT_EQ(queue->Next(0, &delivered[0]), CE_OK);
        ASSERT_EQ(queue->Next(0, &delivered[1]), CE_OK);
        EXPECT_GT(HeapProbe::live.load(), 0);
    }
    EXPECT_EQ(HeapProbe::live.load(), 0);
}

TEST_F(EngineEventQueueTest, ReservationsConsumeAndReturnSlots) {
    auto queue = MakeQueue(3);
    auto a = queue->Reserve();
    auto b = queue->Reserve();
    auto c = queue->Reserve();
    ASSERT_TRUE(a && b && c);
    EXPECT_EQ(queue->GetStats().reserved, 3u);
    EXPECT_FALSE(queue->Reserve()) << "admission fails when no slot is left (the caller returns no_memory)";

    b = ce::engine::EventQueue::Reservation();  // assigning an empty one cancels b
    EXPECT_EQ(queue->GetStats().reserved, 2u);
    auto d = queue->Reserve();
    EXPECT_TRUE(d);
    EventQueue::Reservation moved(std::move(a));
    EXPECT_FALSE(a);
    EXPECT_TRUE(moved);
    EXPECT_EQ(queue->GetStats().reserved, 3u) << "a move transfers the slot, it does not duplicate it";
    moved.Cancel();
    moved.Cancel();  // idempotent
    EXPECT_EQ(queue->GetStats().reserved, 2u);

    ASSERT_TRUE(queue->PushReserved(std::move(c), ce::engine::CommandCompletedEvent(10, CE_OK)));
    EXPECT_EQ(queue->GetStats().reserved, 1u);
    EXPECT_EQ(queue->GetStats().pending, 1u);
    d.Cancel();
    EXPECT_EQ(Drain(*queue).size(), 1u);
    EXPECT_EQ(queue->GetStats().reserved, 0u);
}

TEST_F(EngineEventQueueTest, ReservationFailsWhenTheHeapFails) {
    auto queue = MakeQueue(4);
    HeapProbe::failing = true;
    EXPECT_FALSE(queue->Reserve());
    HeapProbe::failing = false;
    EXPECT_EQ(queue->GetStats().reserved, 0u) << "a failed allocation must not leak a slot";
    EXPECT_TRUE(queue->Reserve());
}

TEST_F(EngineEventQueueTest, ReservedPushNeverAllocatesWithinTheReservedRoom) {
    auto queue = MakeQueue(4);
    auto reservation = queue->Reserve();
    ASSERT_TRUE(reservation);
    const int before = HeapProbe::allocations.load();
    HeapProbe::failing = true;  // an allocation attempt would now fail loudly
    const std::string message(EventQueue::kDefaultReservedTextBytes - 1, 'm');
    ASSERT_TRUE(queue->PushReserved(std::move(reservation), ce::engine::CommandCompletedEvent(5, CE_E_IPC, message)));
    HeapProbe::failing = false;
    EXPECT_EQ(HeapProbe::allocations.load(), before) << "the push must run on the memory taken at admission";
    const std::vector<Delivered> events = Drain(*queue);
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].message, message);
    EXPECT_EQ(events[0].status, CE_E_IPC);
}

TEST_F(EngineEventQueueTest, ReservedPushEnlargesTheRecordWhenTheTextOutgrowsIt) {
    auto queue = MakeQueue(4);
    auto reservation = queue->Reserve(16, 0);
    ASSERT_TRUE(reservation);
    const std::string path(3000, 'p');
    ASSERT_TRUE(
        queue->PushReserved(std::move(reservation), ce::engine::RecordingFinalizedEvent(1, CE_OK, path, "finalized")));
    const std::vector<Delivered> events = Drain(*queue);
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].outputPath, path);
    EXPECT_EQ(events[0].message, "finalized");
    EXPECT_EQ(queue->GetStats().reserved, 0u);
}

TEST_F(EngineEventQueueTest, ReservedPushUnderMemoryExhaustionKeepsTheEventAndDegradesOnlyText) {
    auto queue = MakeQueue(8);
    // FINALIZED: a path that cannot fit is omitted, never cut; identity and status survive.
    auto finalized = queue->Reserve(64, 0);
    // SCREENSHOT: the second path does not fit; the first stays.
    auto shot = queue->Reserve(40, 2);
    // Message: cut on a UTF-8 boundary ("ä" is two bytes).
    auto text = queue->Reserve(8, 0);
    ASSERT_TRUE(finalized && shot && text);

    HeapProbe::failing = true;
    const std::string longPath(200, 'p');
    const std::string first(30, 'a');
    const std::string second(30, 'b');
    const std::string_view paths[] = {first, second};
    ASSERT_TRUE(
        queue->PushReserved(std::move(finalized), ce::engine::RecordingFinalizedEvent(21, CE_E_IO, longPath, "hello")));
    ASSERT_TRUE(queue->PushReserved(std::move(shot),
                                    ce::engine::ScreenshotEvent(22, CE_OK, std::span<const std::string_view>(paths))));
    ASSERT_TRUE(queue->PushReserved(std::move(text),
                                    ce::engine::CommandCompletedEvent(23, CE_OK, "\xC3\xA4\xC3\xA4\xC3\xA4\xC3\xA4")));
    HeapProbe::failing = false;

    const std::vector<Delivered> events = Drain(*queue);
    ASSERT_EQ(events.size(), 3u);
    EXPECT_EQ(events[0].type, CE_EVENT_RECORDING_FINALIZED);
    EXPECT_EQ(events[0].raw.data.finalized.recording_id, 21u);
    EXPECT_EQ(events[0].status, CE_E_IO);
    EXPECT_FALSE(events[0].hasOutputPath);
    EXPECT_EQ(events[0].message, "hello");
    EXPECT_EQ(events[1].type, CE_EVENT_SCREENSHOT);
    ASSERT_EQ(events[1].paths.size(), 1u);
    EXPECT_EQ(events[1].paths[0], first);
    EXPECT_EQ(events[2].request, 23u);
    EXPECT_EQ(events[2].message, "\xC3\xA4\xC3\xA4\xC3\xA4") << "7 usable bytes cut back to 6, never mid-character";
}

TEST_F(EngineEventQueueTest, MisusedPushesAreRefusedAndReturnTheirSlot) {
    auto queue = MakeQueue(4);
    auto other = MakeQueue(4);

    EXPECT_FALSE(queue->PushState(ce::engine::CommandCompletedEvent(1, CE_OK))) << "needs a reservation";
    EXPECT_FALSE(queue->PushState(ce::engine::RecordingFinalizedEvent(1, CE_OK, "")));
    EventDraft invalid;
    invalid.type = 0;
    EXPECT_FALSE(queue->PushState(invalid));
    invalid.type = EventQueue::kMaxEventType + 1;
    EXPECT_FALSE(queue->PushState(invalid));
    invalid.type = CE_EVENT_EVENTS_DROPPED;
    EXPECT_FALSE(queue->PushState(invalid));
    EXPECT_EQ(queue->GetStats().pending, 0u);
    EXPECT_EQ(queue->GetStats().droppedTotal, 0u) << "a refused misuse is not a dropped event";

    auto reservation = queue->Reserve();
    ASSERT_TRUE(reservation);
    EXPECT_FALSE(queue->PushReserved(std::move(reservation), invalid));
    EXPECT_EQ(queue->GetStats().reserved, 0u) << "the slot of a refused reserved push is returned";

    auto foreign = other->Reserve();
    ASSERT_TRUE(foreign);
    EXPECT_FALSE(queue->PushReserved(std::move(foreign), ce::engine::CommandCompletedEvent(1, CE_OK)));
    EXPECT_EQ(other->GetStats().reserved, 0u);
    EXPECT_EQ(queue->GetStats().pending, 0u);
    EXPECT_EQ(other->GetStats().pending, 0u);
    EventQueue::Reservation empty;
    EXPECT_FALSE(queue->PushReserved(std::move(empty), ce::engine::CommandCompletedEvent(1, CE_OK)));
}

// ---- overflow -----------------------------------------------------------------------------------

TEST_F(EngineEventQueueTest, OverflowCollapsesOlderStateEventsIntoTheNewestAndAnnouncesTheDrop) {
    auto queue = MakeQueue(4);
    for (int32_t state = 1; state <= 4; ++state)
        ASSERT_TRUE(queue->PushState(ce::engine::RuntimeStateEvent(state)));
    EXPECT_EQ(queue->GetStats().droppedTotal, 0u) << "no overflow yet";
    ASSERT_TRUE(queue->PushState(ce::engine::RuntimeStateEvent(5)));  // full: 1..3 collapse into 4

    EXPECT_EQ(queue->GetStats().unannounced, 3u);
    EXPECT_TRUE(Signaled(*queue));
    const std::vector<Delivered> events = Drain(*queue);
    ASSERT_EQ(events.size(), 3u);
    EXPECT_EQ(events[0].type, CE_EVENT_EVENTS_DROPPED) << "the notice is the next delivered event";
    EXPECT_EQ(events[0].raw.data.dropped.count, 3u);
    EXPECT_EQ(events[1].raw.data.runtime.state, 4);
    EXPECT_EQ(events[2].raw.data.runtime.state, 5);
    EXPECT_EQ(queue->GetStats().unannounced, 0u);
}

TEST_F(EngineEventQueueTest, OverflowKeepsTheNewestOfEveryTypeAndLosesNothingSilently) {
    auto queue = MakeQueue(6);
    int pushes = 0;
    for (int32_t n = 1; n <= 10; ++n) {
        ASSERT_TRUE(queue->PushState(ce::engine::RuntimeStateEvent(n)));
        ASSERT_TRUE(queue->PushState(ce::engine::HotkeyEvent(n)));
        pushes += 2;
    }
    const std::vector<Delivered> events = Drain(*queue);
    ASSERT_FALSE(events.empty());
    ASSERT_EQ(events.front().type, CE_EVENT_EVENTS_DROPPED);

    int64_t newestRuntime = 0;
    int64_t newestHotkey = 0;
    int64_t lastRuntime = 0;
    int64_t lastHotkey = 0;
    size_t delivered = 0;
    for (size_t i = 1; i < events.size(); ++i) {
        ++delivered;
        if (events[i].type == CE_EVENT_RUNTIME_STATE) {
            newestRuntime = events[i].raw.data.runtime.state;
            EXPECT_GT(newestRuntime, lastRuntime) << "collapsing must not reorder a type";
            lastRuntime = newestRuntime;
        } else {
            ASSERT_EQ(events[i].type, CE_EVENT_HOTKEY);
            newestHotkey = events[i].raw.data.hotkey.action;
            EXPECT_GT(newestHotkey, lastHotkey);
            lastHotkey = newestHotkey;
        }
    }
    EXPECT_EQ(newestRuntime, 10);
    EXPECT_EQ(newestHotkey, 10);
    EXPECT_LE(delivered, 6u);
    EXPECT_EQ(events.front().raw.data.dropped.count + delivered, static_cast<uint64_t>(pushes))
        << "every push is either delivered or counted in the notice";
}

TEST_F(EngineEventQueueTest, CompletionsAndFinalizationsSurviveAStateFlood) {
    auto queue = MakeQueue(8);
    auto c1 = queue->Reserve();
    auto c2 = queue->Reserve();
    auto f = queue->Reserve(EventQueue::kDefaultReservedTextBytes, 0);
    ASSERT_TRUE(c1 && c2 && f);

    const int32_t types[] = {CE_EVENT_RUNTIME_STATE, CE_EVENT_RECORDING_STATE, CE_EVENT_HELPER, CE_EVENT_HOTKEY};
    for (int i = 0; i < 400; ++i) {
        EventDraft draft;
        draft.type = types[i % 4];
        (void)queue->PushState(draft);
        if (i == 150)
            ASSERT_TRUE(queue->PushReserved(std::move(c1), ce::engine::CommandCompletedEvent(1001, CE_OK)));
        if (i == 300)
            ASSERT_TRUE(queue->PushReserved(std::move(f), ce::engine::RecordingFinalizedEvent(7, CE_OK, "out.mkv")));
    }
    ASSERT_TRUE(queue->PushReserved(std::move(c2), ce::engine::CommandCompletedEvent(1002, CE_E_PROCESS)));

    const std::vector<Delivered> events = Drain(*queue);
    int completions1001 = 0, completions1002 = 0, finalized = 0;
    uint64_t dropped = 0;
    size_t stateEvents = 0;
    for (const Delivered& event : events) {
        if (event.type == CE_EVENT_COMMAND_COMPLETED && event.request == 1001)
            ++completions1001;
        else if (event.type == CE_EVENT_COMMAND_COMPLETED && event.request == 1002)
            ++completions1002;
        else if (event.type == CE_EVENT_RECORDING_FINALIZED && event.outputPath == "out.mkv")
            ++finalized;
        else if (event.type == CE_EVENT_EVENTS_DROPPED)
            dropped += event.raw.data.dropped.count;
        else
            ++stateEvents;
    }
    EXPECT_EQ(completions1001, 1);
    EXPECT_EQ(completions1002, 1);
    EXPECT_EQ(finalized, 1);
    EXPECT_GT(dropped, 0u);
    EXPECT_EQ(dropped + stateEvents, 400u);
    EXPECT_EQ(queue->GetStats().droppedTotal, dropped);
}

TEST_F(EngineEventQueueTest, AdmissionCollapsesStateEventsToMakeRoomForAReservation) {
    auto queue = MakeQueue(4);
    for (int32_t i = 1; i <= 4; ++i)
        ASSERT_TRUE(queue->PushState(ce::engine::HotkeyEvent(i)));
    auto reservation = queue->Reserve();
    EXPECT_TRUE(reservation) << "a client that never reads must not block commands";
    EXPECT_EQ(queue->GetStats().unannounced, 3u);
    ASSERT_TRUE(queue->PushReserved(std::move(reservation), ce::engine::CommandCompletedEvent(9, CE_OK)));
    const std::vector<Delivered> events = Drain(*queue);
    ASSERT_EQ(events.size(), 3u);
    EXPECT_EQ(events[0].type, CE_EVENT_EVENTS_DROPPED);
    EXPECT_EQ(events[0].raw.data.dropped.count, 3u);
    EXPECT_EQ(events[1].type, CE_EVENT_HOTKEY);
    EXPECT_EQ(events[2].type, CE_EVENT_COMMAND_COMPLETED);
}

TEST_F(EngineEventQueueTest, AdmissionFailsOnlyWhenNothingCoalescibleRemains) {
    auto queue = MakeQueue(3);
    auto a = queue->Reserve();
    auto b = queue->Reserve();
    ASSERT_TRUE(queue->PushState(ce::engine::HotkeyEvent(1)));
    ASSERT_TRUE(a && b);
    EXPECT_FALSE(queue->Reserve()) << "one pending state event cannot be collapsed further";
    EXPECT_EQ(queue->GetStats().unannounced, 0u) << "a refused reservation drops nothing";
    EXPECT_EQ(queue->GetStats().pending, 1u);
}

TEST_F(EngineEventQueueTest, NewStateEventSupersedesItsOwnTypeWhenNothingElseCanGiveWay) {
    auto queue = MakeQueue(2);
    ASSERT_TRUE(queue->PushState(ce::engine::RuntimeStateEvent(1)));
    auto reservation = queue->Reserve();  // budget exhausted: one pending, one reserved
    ASSERT_TRUE(reservation);
    ASSERT_TRUE(queue->PushState(ce::engine::RuntimeStateEvent(2)));  // supersedes state 1
    EXPECT_EQ(queue->GetStats().pending, 1u);
    EXPECT_EQ(queue->GetStats().unannounced, 1u);

    // The notice is outside the budget: even a full queue says what it lost, and says it first.
    const std::vector<Delivered> events = Drain(*queue);
    ASSERT_EQ(events.size(), 2u);
    EXPECT_EQ(events[0].type, CE_EVENT_EVENTS_DROPPED);
    EXPECT_EQ(events[0].raw.data.dropped.count, 1u);
    EXPECT_EQ(events[1].type, CE_EVENT_RUNTIME_STATE);
    EXPECT_EQ(events[1].raw.data.runtime.state, 2);
}

TEST_F(EngineEventQueueTest, StateEventIsDroppedWhenTheBudgetHoldsOnlyGuaranteedWork) {
    auto queue = MakeQueue(2);
    auto a = queue->Reserve();
    auto b = queue->Reserve();
    ASSERT_TRUE(a && b);
    EXPECT_FALSE(queue->PushState(ce::engine::RuntimeStateEvent(1)));
    EXPECT_EQ(queue->GetStats().droppedTotal, 1u);
    EXPECT_EQ(queue->GetStats().unannounced, 1u);

    // The budget holds nothing but reservations, yet the loss is reported at once.
    EXPECT_TRUE(Signaled(*queue));
    const std::vector<Delivered> events = Drain(*queue);
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].type, CE_EVENT_EVENTS_DROPPED);
    EXPECT_EQ(events[0].raw.data.dropped.count, 1u);
}

TEST_F(EngineEventQueueTest, DropNoticeAccumulatesAcrossBurstsAndComesFirst) {
    auto queue = MakeQueue(2);
    ASSERT_TRUE(queue->PushState(ce::engine::HotkeyEvent(1)));
    for (int32_t i = 2; i <= 6; ++i)
        ASSERT_TRUE(queue->PushState(ce::engine::HotkeyEvent(i)));  // each supersedes/collapses
    ASSERT_TRUE(queue->PushState(ce::engine::RuntimeStateEvent(1)));
    const std::vector<Delivered> first = Drain(*queue);
    ASSERT_GE(first.size(), 2u);
    EXPECT_EQ(first[0].type, CE_EVENT_EVENTS_DROPPED);
    const uint64_t firstCount = first[0].raw.data.dropped.count;
    EXPECT_GT(firstCount, 0u);

    // A second burst after the notice was delivered starts a fresh count.
    for (int32_t i = 7; i <= 12; ++i)
        ASSERT_TRUE(queue->PushState(ce::engine::HotkeyEvent(i)));
    const std::vector<Delivered> second = Drain(*queue);
    ASSERT_FALSE(second.empty());
    EXPECT_EQ(second[0].type, CE_EVENT_EVENTS_DROPPED);
    EXPECT_EQ(second[0].raw.data.dropped.count + firstCount, queue->GetStats().droppedTotal);
    EXPECT_GT(second[0].sequence, first.back().sequence);
}

TEST_F(EngineEventQueueTest, SequenceIncreasesStrictlyInDeliveryOrderAcrossCoalescingAndNotices) {
    auto queue = MakeQueue(4);
    uint64_t last = 0;
    for (int round = 0; round < 5; ++round) {
        for (int32_t i = 0; i < 9; ++i)
            (void)queue->PushState(ce::engine::HotkeyEvent(i));
        for (const Delivered& event : Drain(*queue)) {
            EXPECT_GT(event.sequence, last);
            last = event.sequence;
        }
    }
    EXPECT_GT(last, 5u);
}

TEST_F(EngineEventQueueTest, StateEventIsDroppedAndCountedWhenItsRecordCannotBeAllocated) {
    auto queue = MakeQueue(4);
    HeapProbe::failing = true;
    EXPECT_FALSE(queue->PushState(ce::engine::RuntimeStateEvent(1)));
    HeapProbe::failing = false;
    EXPECT_EQ(queue->GetStats().unannounced, 1u);
    const std::vector<Delivered> events = Drain(*queue);
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].type, CE_EVENT_EVENTS_DROPPED);
    EXPECT_EQ(events[0].raw.data.dropped.count, 1u);
}

TEST_F(EngineEventQueueTest, OwedDropNoticeSurvivesAllocationFailureAtDelivery) {
    auto queue = MakeQueue(4);
    HeapProbe::failing = true;
    ASSERT_FALSE(queue->PushState(ce::engine::RuntimeStateEvent(1)));
    const ce_event_t* out = nullptr;
    EXPECT_EQ(queue->Next(0, &out), CE_E_NO_MEMORY) << "the notice is owed but cannot be built right now";
    EXPECT_EQ(out, nullptr);
    HeapProbe::failing = false;
    EXPECT_EQ(Drain(*queue).size(), 1u) << "the count is kept and delivered once memory returns";
}

// ---- blocking -----------------------------------------------------------------------------------

TEST_F(EngineEventQueueTest, BlockedNextWakesOnPushFromAnotherThread) {
    auto queue = MakeQueue(4);
    ce_status_t status = CE_E_INTERNAL;
    std::thread consumer([&] {
        const ce_event_t* event = nullptr;
        status = queue->Next(30000, &event);  // the guard against a lost wake-up, not an expectation
        if (status == CE_OK)
            EXPECT_EQ(queue->Release(event), CE_OK);
    });
    ASSERT_TRUE(queue->PushState(ce::engine::HotkeyEvent(1)));
    consumer.join();
    EXPECT_EQ(status, CE_OK);
}

TEST_F(EngineEventQueueTest, HandleWaitReleasesWhenEventsArrive) {
    auto queue = MakeQueue(4);
    DWORD waited = WAIT_FAILED;
    std::thread consumer([&] { waited = WaitForSingleObject(queue->Handle(), 30000); });
    ASSERT_TRUE(queue->PushState(ce::engine::HotkeyEvent(1)));
    consumer.join();
    EXPECT_EQ(waited, WAIT_OBJECT_0);
    EXPECT_EQ(Drain(*queue).size(), 1u);
}

}  // namespace
