#pragma once

#include <d3d12.h>

#include "common/graphics/sharpen_policy.h"

namespace ce::sharpen {

bool RenderPresentedPostProcess(ID3D12Device* device, ID3D12CommandQueue* queue, ID3D12Resource* output,
                                 DXGI_FORMAT viewFormat, const Request& request, Route route, TargetEncoding encoding);
bool RecordRuntimePostProcess(ID3D12Device* device, ID3D12GraphicsCommandList* list, ID3D12Resource* output,
                              D3D12_RESOURCE_STATES state, const Request& request, TargetEncoding encoding);
// Call only AFTER forwarding these exact lists, never on an unsubmitted path.
void NotifyRuntimePostProcessSubmitted(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists);
void CollectRuntimePostProcess(bool trim);

}  // namespace ce::sharpen
