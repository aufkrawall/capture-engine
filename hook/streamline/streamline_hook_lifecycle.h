#pragma once

#include <cstddef>
#include <windows.h>

class StreamlineInlineHookBatch;

namespace ce::streamline_lifecycle {

// Observe documented 2.x lifecycle calls without changing their arguments or result.
void QueueInterposerDiagnostics(HMODULE module, StreamlineInlineHookBatch& batch);
void QueueDlssgDestroyDiagnostic(HMODULE module, StreamlineInlineHookBatch& batch);
void OnModuleUnloaded(const void* moduleBase, size_t moduleSize);

// Creation provenance for unload ordering: the actual DLSS-G plugin must be on the create stack.
// Streamline's interposer/common/SR frames also surround ordinary application and auxiliary swapchains.
bool IsDlssgCreatingSwapchain();

}  // namespace ce::streamline_lifecycle
