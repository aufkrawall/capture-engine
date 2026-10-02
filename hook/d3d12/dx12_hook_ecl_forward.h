#pragma once

#include "dx12_hook_internal.h"

#include "hook/present/present_callback_association.h"

namespace ce::dx12_ecl_forward {

extern thread_local int recursionDepth;

ExecuteCommandListsPtr ResolveRecursionBreakTarget(ID3D12CommandQueue* queue);
void TransparentNativeFSRCallback(ID3D12CommandQueue* queue, UINT numCommandLists,
                                  ID3D12CommandList* const* commandLists);

// Whether inject capture may copy this Present's image, from the command lists
// counted since the previous Present and the verdict the present callback
// staged for it (dx12_overlay_policy::IsPresentedFrameForCapture). Logs,
// rate-limited, the generated/application split of callback-proven outputs.
bool IsPresentedFrameForCapture(int eclSubmissionCount, const present_association::PresentFrameVerdict& verdict);

}  // namespace ce::dx12_ecl_forward
