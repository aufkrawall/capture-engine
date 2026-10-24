#pragma once

#include <windows.h>

#include <cstddef>

#include "crash_stack_overflow_summary.h"

namespace ce::crash_stack {

// More than any real stack holds at the smallest x64 frame of a 1 MiB stack, enough for 75k-frame recursions on
// the 12 MiB stacks Unreal games use, and still a bounded walk inside the crash handler.
inline constexpr size_t kMaxOverflowWalkFrames = 262144;

// Unwinds a thread's stack from `context` with the images' own unwind data, x64 only (false elsewhere). Takes no
// loader lock and allocates nothing, and reads only inside the stack reservation that holds the context's RSP:
// a frame whose unwind would read outside it ends the walk instead (`*complete` false). `ownImageBase` names the
// image counted as CE's (the module that runs this code when 0).
bool WalkStack(const CONTEXT& context, size_t maxFrames, uintptr_t ownImageBase, OverflowFrameSummary* summary,
               bool* complete);

// One crash.log line: frame count, the repeating cycle at the fault, CE's share and the outermost frames, with
// each address as <image export name>+offset - the name the image was built with, read without the loader lock.
void FormatOverflowAttribution(const OverflowFrameSummary& summary, bool complete, char* out, size_t outSize);

}  // namespace ce::crash_stack
