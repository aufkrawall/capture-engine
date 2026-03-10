#pragma once

#include <windows.h>

namespace StreamlineHook {

void Init();
bool IsInitialized();
void Shutdown();

// Returns true when slDLSSGSetOptions has been called with FG enabled.
// More timely than IsActualFrameGenerationActive() which relies on heuristics.
bool IsDLSSFGRequestedViaStreamline();

}  // namespace StreamlineHook
