#pragma once

#include <cstddef>
#include <windows.h>

class StreamlineInlineHookBatch;

namespace ce::streamline_lifecycle {

// Observe documented 2.x lifecycle calls without changing their arguments or result.
void QueueInterposerDiagnostics(HMODULE module, StreamlineInlineHookBatch& batch);
void QueueDlssgDestroyDiagnostic(HMODULE module, StreamlineInlineHookBatch& batch);
void OnModuleUnloaded(const void* moduleBase, size_t moduleSize);

}  // namespace ce::streamline_lifecycle
