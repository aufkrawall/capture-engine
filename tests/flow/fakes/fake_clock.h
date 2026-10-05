#pragma once

// The scenario's virtual clock as the fake runtimes see it: the flow-test hook DLL owns it
// (CEFlow_AdvanceClock), the game sets the frame interval (CE_FLOW_FRAME_INTERVAL_US). A presenter that shows
// several frames per game frame spaces them evenly inside the interval, as DLSS-G and FFX pace their output.
// Every Present on a real swapchain is also tallied in the game (CEFlowGame_CountPhysicalPresent), so a
// scenario can tell presents CE never accounted from presents it covered.

#include <windows.h>

#include <cstdint>
#include <cstdlib>

namespace ce::flow::fake {

inline int64_t FrameIntervalMicroseconds() {
    char value[32] = {};
    if (GetEnvironmentVariableA("CE_FLOW_FRAME_INTERVAL_US", value, sizeof(value)))
        return std::strtoll(value, nullptr, 10);
    return 6944;
}

inline void AdvanceClock(int64_t microseconds) {
    using Advance = void (*)(int64_t);
    static const Advance advance = [] {
        HMODULE hook = GetModuleHandleA("capture_hook_x64.dll");
        return hook ? reinterpret_cast<Advance>(GetProcAddress(hook, "CEFlow_AdvanceClock")) : nullptr;
    }();
    if (advance && microseconds > 0)
        advance(microseconds);
}

// A frame generation runtime about to present an output of the game's `frame`-th Present on `presenter`; the flow
// entry compares it with CE's attributed frame within that presenter lifetime, even when addresses are reused.
inline void NoteRuntimeOutputFrame(const void* presenter, uint64_t lifetime, uint64_t frame) {
    using Note = void (*)(const void*, uint64_t, uint64_t);
    static const Note note = [] {
        HMODULE hook = GetModuleHandleA("capture_hook_x64.dll");
        return hook ? reinterpret_cast<Note>(GetProcAddress(hook, "CEFlow_NoteRuntimeOutputFrame")) : nullptr;
    }();
    if (note)
        note(presenter, lifetime, frame);
}

inline void CountPhysicalPresent() {
    using Count = void (*)();
    static const Count count = reinterpret_cast<Count>(
        GetProcAddress(GetModuleHandleA(nullptr), "CEFlowGame_CountPhysicalPresent"));
    if (count)
        count();
}

}  // namespace ce::flow::fake
