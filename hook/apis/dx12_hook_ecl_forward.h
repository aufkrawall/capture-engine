#pragma once

#include "dx12_hook_internal.h"

namespace ce::dx12_ecl_forward {

extern thread_local int recursionDepth;

ExecuteCommandListsPtr ResolveRecursionBreakTarget(ID3D12CommandQueue* queue);
void TransparentNativeFSRCallback(ID3D12CommandQueue* queue, UINT numCommandLists,
                                  ID3D12CommandList* const* commandLists);

}  // namespace ce::dx12_ecl_forward
