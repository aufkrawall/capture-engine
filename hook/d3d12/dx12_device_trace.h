#pragma once

#include <d3d12.h>

namespace ce::dx12_device_trace {

// Callers retain the receiver; this boundary owns exact-vtable installation and forwarding evidence.
void HookDevice(ID3D12Device* device);
HRESULT CreateCommandQueue(ID3D12Device* device, const D3D12_COMMAND_QUEUE_DESC* desc, REFIID id, void** out);
HRESULT CreateDescriptorHeap(ID3D12Device* device, const D3D12_DESCRIPTOR_HEAP_DESC* desc, REFIID id, void** out);
HRESULT CreateCommittedResource(ID3D12Device* device, const D3D12_HEAP_PROPERTIES* properties, D3D12_HEAP_FLAGS flags,
                                const D3D12_RESOURCE_DESC* desc, D3D12_RESOURCE_STATES state,
                                const D3D12_CLEAR_VALUE* clearValue, REFIID id, void** out);

// Retires cached dispatch evidence; physical slots still recover only their own saved predecessor.
void Reset();

}  // namespace ce::dx12_device_trace
