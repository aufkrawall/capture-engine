#pragma once

// File-scope variables defined in the first part (extern for the rest).


// File-scope functions shared by the split parts (definitions in the first part).

#include "dx12_hook_internal.h"

// This declaration used to sit on the same line as the #include above, where the
// preprocessor discarded it as extra tokens after the header name (-Wextra-tokens). The
// header therefore did not declare what it claimed to; it only compiled because callers
// picked the declaration up from dx12_hook_internal.h.
void STDMETHODCALLTYPE DetourExecuteCommandLists(ID3D12CommandQueue* pThis, UINT NumCommandLists,
                                                 ID3D12CommandList* const* ppCommandLists);
__attribute__((noinline)) void DX12_HookQueueVTable(ID3D12CommandQueue* queue);

