#pragma once

#include "tests/flow/queue_dispatch_probe.h"

namespace ce::flow {

// A distinct COM device view over the same WARP device, as an interposer can expose.
// Methods delegate normally; no new adapter, GPU, thread, or delayed work is created.
inline std::atomic<uint32_t> g_deviceViewProbeObjects{0};

class QueueDeviceViewProbe final : public ID3D12Device {
public:
    explicit QueueDeviceViewProbe(ID3D12Device* native) : native_(native) { ++g_deviceViewProbeObjects; }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (id == __uuidof(IUnknown) || id == __uuidof(ID3D12Object) || id == __uuidof(ID3D12Device)) {
            *out = static_cast<ID3D12Device*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG remaining = --references_;
        if (!remaining) delete this;
        return remaining;
    }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID id, UINT* size, void* data) override {
        return native_->GetPrivateData(id, size, data);
    }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID id, UINT size, const void* data) override {
        return native_->SetPrivateData(id, size, data);
    }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID id, const IUnknown* data) override {
        return native_->SetPrivateDataInterface(id, data);
    }
    HRESULT STDMETHODCALLTYPE SetName(LPCWSTR name) override { return native_->SetName(name); }
    UINT STDMETHODCALLTYPE GetNodeCount() override {
        return native_->GetNodeCount();
    }
    HRESULT STDMETHODCALLTYPE CreateCommandQueue(
        const D3D12_COMMAND_QUEUE_DESC *desc,
        REFIID riid,
        void **command_queue) override {
        return native_->CreateCommandQueue(desc, riid, command_queue);
    }
    HRESULT STDMETHODCALLTYPE CreateCommandAllocator(
        D3D12_COMMAND_LIST_TYPE type,
        REFIID riid,
        void **command_allocator) override {
        return native_->CreateCommandAllocator(type, riid, command_allocator);
    }
    HRESULT STDMETHODCALLTYPE CreateGraphicsPipelineState(
        const D3D12_GRAPHICS_PIPELINE_STATE_DESC *desc,
        REFIID riid,
        void **pipeline_state) override {
        return native_->CreateGraphicsPipelineState(desc, riid, pipeline_state);
    }
    HRESULT STDMETHODCALLTYPE CreateComputePipelineState(
        const D3D12_COMPUTE_PIPELINE_STATE_DESC *desc,
        REFIID riid,
        void **pipeline_state) override {
        return native_->CreateComputePipelineState(desc, riid, pipeline_state);
    }
    HRESULT STDMETHODCALLTYPE CreateCommandList(
        UINT node_mask,
        D3D12_COMMAND_LIST_TYPE type,
        ID3D12CommandAllocator *command_allocator,
        ID3D12PipelineState *initial_pipeline_state,
        REFIID riid,
        void **command_list) override {
        return native_->CreateCommandList(node_mask, type, command_allocator, initial_pipeline_state, riid,
            command_list);
    }
    HRESULT STDMETHODCALLTYPE CheckFeatureSupport(
        D3D12_FEATURE feature,
        void *feature_data,
        UINT feature_data_size) override {
        return native_->CheckFeatureSupport(feature, feature_data, feature_data_size);
    }
    HRESULT STDMETHODCALLTYPE CreateDescriptorHeap(
        const D3D12_DESCRIPTOR_HEAP_DESC *desc,
        REFIID riid,
        void **descriptor_heap) override {
        return native_->CreateDescriptorHeap(desc, riid, descriptor_heap);
    }
    UINT STDMETHODCALLTYPE GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE descriptor_heap_type) override {
        return native_->GetDescriptorHandleIncrementSize(descriptor_heap_type);
    }
    HRESULT STDMETHODCALLTYPE CreateRootSignature(
        UINT node_mask,
        const void *bytecode,
        SIZE_T bytecode_length,
        REFIID riid,
        void **root_signature) override {
        return native_->CreateRootSignature(node_mask, bytecode, bytecode_length, riid, root_signature);
    }
    void STDMETHODCALLTYPE CreateConstantBufferView(
        const D3D12_CONSTANT_BUFFER_VIEW_DESC *desc,
        D3D12_CPU_DESCRIPTOR_HANDLE descriptor) override {
        native_->CreateConstantBufferView(desc, descriptor);
    }
    void STDMETHODCALLTYPE CreateShaderResourceView(
        ID3D12Resource *resource,
        const D3D12_SHADER_RESOURCE_VIEW_DESC *desc,
        D3D12_CPU_DESCRIPTOR_HANDLE descriptor) override {
        native_->CreateShaderResourceView(resource, desc, descriptor);
    }
    void STDMETHODCALLTYPE CreateUnorderedAccessView(
        ID3D12Resource *resource,
        ID3D12Resource *counter_resource,
        const D3D12_UNORDERED_ACCESS_VIEW_DESC *desc,
        D3D12_CPU_DESCRIPTOR_HANDLE descriptor) override {
        native_->CreateUnorderedAccessView(resource, counter_resource, desc, descriptor);
    }
    void STDMETHODCALLTYPE CreateRenderTargetView(
        ID3D12Resource *resource,
        const D3D12_RENDER_TARGET_VIEW_DESC *desc,
        D3D12_CPU_DESCRIPTOR_HANDLE descriptor) override {
        native_->CreateRenderTargetView(resource, desc, descriptor);
    }
    void STDMETHODCALLTYPE CreateDepthStencilView(
        ID3D12Resource *resource,
        const D3D12_DEPTH_STENCIL_VIEW_DESC *desc,
        D3D12_CPU_DESCRIPTOR_HANDLE descriptor) override {
        native_->CreateDepthStencilView(resource, desc, descriptor);
    }
    void STDMETHODCALLTYPE CreateSampler(
        const D3D12_SAMPLER_DESC *desc,
        D3D12_CPU_DESCRIPTOR_HANDLE descriptor) override {
        native_->CreateSampler(desc, descriptor);
    }
    void STDMETHODCALLTYPE CopyDescriptors(
        UINT dst_descriptor_range_count,
        const D3D12_CPU_DESCRIPTOR_HANDLE *dst_descriptor_range_offsets,
        const UINT *dst_descriptor_range_sizes,
        UINT src_descriptor_range_count,
        const D3D12_CPU_DESCRIPTOR_HANDLE *src_descriptor_range_offsets,
        const UINT *src_descriptor_range_sizes,
        D3D12_DESCRIPTOR_HEAP_TYPE descriptor_heap_type) override {
        native_->CopyDescriptors(dst_descriptor_range_count, dst_descriptor_range_offsets,
            dst_descriptor_range_sizes, src_descriptor_range_count, src_descriptor_range_offsets,
            src_descriptor_range_sizes, descriptor_heap_type);
    }
    void STDMETHODCALLTYPE CopyDescriptorsSimple(
        UINT descriptor_count,
        const D3D12_CPU_DESCRIPTOR_HANDLE dst_descriptor_range_offset,
        const D3D12_CPU_DESCRIPTOR_HANDLE src_descriptor_range_offset,
        D3D12_DESCRIPTOR_HEAP_TYPE descriptor_heap_type) override {
        native_->CopyDescriptorsSimple(descriptor_count, dst_descriptor_range_offset, src_descriptor_range_offset,
            descriptor_heap_type);
    }
#ifdef WIDL_EXPLICIT_AGGREGATE_RETURNS
    D3D12_RESOURCE_ALLOCATION_INFO* STDMETHODCALLTYPE GetResourceAllocationInfo(
        D3D12_RESOURCE_ALLOCATION_INFO *__ret,
        UINT visible_mask,
        UINT reource_desc_count,
        const D3D12_RESOURCE_DESC *resource_descs) override {
        return native_->GetResourceAllocationInfo(__ret, visible_mask, reource_desc_count, resource_descs);
    }
#else
    D3D12_RESOURCE_ALLOCATION_INFO STDMETHODCALLTYPE GetResourceAllocationInfo(
        UINT visible_mask,
        UINT reource_desc_count,
        const D3D12_RESOURCE_DESC *resource_descs) override {
        return native_->GetResourceAllocationInfo(visible_mask, reource_desc_count, resource_descs);
    }
#endif
#ifdef WIDL_EXPLICIT_AGGREGATE_RETURNS
    D3D12_HEAP_PROPERTIES* STDMETHODCALLTYPE GetCustomHeapProperties(
        D3D12_HEAP_PROPERTIES *__ret,
        UINT node_mask,
        D3D12_HEAP_TYPE heap_type) override {
        return native_->GetCustomHeapProperties(__ret, node_mask, heap_type);
    }
#else
    D3D12_HEAP_PROPERTIES STDMETHODCALLTYPE GetCustomHeapProperties(
        UINT node_mask,
        D3D12_HEAP_TYPE heap_type) override {
        return native_->GetCustomHeapProperties(node_mask, heap_type);
    }
#endif
    HRESULT STDMETHODCALLTYPE CreateCommittedResource(
        const D3D12_HEAP_PROPERTIES *heap_properties,
        D3D12_HEAP_FLAGS heap_flags,
        const D3D12_RESOURCE_DESC *desc,
        D3D12_RESOURCE_STATES initial_state,
        const D3D12_CLEAR_VALUE *optimized_clear_value,
        REFIID riid,
        void **resource) override {
        return native_->CreateCommittedResource(heap_properties, heap_flags, desc, initial_state,
            optimized_clear_value, riid, resource);
    }
    HRESULT STDMETHODCALLTYPE CreateHeap(
        const D3D12_HEAP_DESC *desc,
        REFIID riid,
        void **heap) override {
        return native_->CreateHeap(desc, riid, heap);
    }
    HRESULT STDMETHODCALLTYPE CreatePlacedResource(
        ID3D12Heap *heap,
        UINT64 heap_offset,
        const D3D12_RESOURCE_DESC *desc,
        D3D12_RESOURCE_STATES initial_state,
        const D3D12_CLEAR_VALUE *optimized_clear_value,
        REFIID riid,
        void **resource) override {
        return native_->CreatePlacedResource(heap, heap_offset, desc, initial_state, optimized_clear_value, riid,
            resource);
    }
    HRESULT STDMETHODCALLTYPE CreateReservedResource(
        const D3D12_RESOURCE_DESC *desc,
        D3D12_RESOURCE_STATES initial_state,
        const D3D12_CLEAR_VALUE *optimized_clear_value,
        REFIID riid,
        void **resource) override {
        return native_->CreateReservedResource(desc, initial_state, optimized_clear_value, riid, resource);
    }
    HRESULT STDMETHODCALLTYPE CreateSharedHandle(
        ID3D12DeviceChild *object,
        const SECURITY_ATTRIBUTES *attributes,
        DWORD access,
        const WCHAR *name,
        HANDLE *handle) override {
        return native_->CreateSharedHandle(object, attributes, access, name, handle);
    }
    HRESULT STDMETHODCALLTYPE OpenSharedHandle(
        HANDLE handle,
        REFIID riid,
        void **object) override {
        return native_->OpenSharedHandle(handle, riid, object);
    }
    HRESULT STDMETHODCALLTYPE OpenSharedHandleByName(
        const WCHAR *name,
        DWORD access,
        HANDLE *handle) override {
        return native_->OpenSharedHandleByName(name, access, handle);
    }
    HRESULT STDMETHODCALLTYPE MakeResident(
        UINT object_count,
        ID3D12Pageable *const *objects) override {
        return native_->MakeResident(object_count, objects);
    }
    HRESULT STDMETHODCALLTYPE Evict(
        UINT object_count,
        ID3D12Pageable *const *objects) override {
        return native_->Evict(object_count, objects);
    }
    HRESULT STDMETHODCALLTYPE CreateFence(
        UINT64 initial_value,
        D3D12_FENCE_FLAGS flags,
        REFIID riid,
        void **fence) override {
        return native_->CreateFence(initial_value, flags, riid, fence);
    }
    HRESULT STDMETHODCALLTYPE GetDeviceRemovedReason() override {
        return native_->GetDeviceRemovedReason();
    }
    void STDMETHODCALLTYPE GetCopyableFootprints(
        const D3D12_RESOURCE_DESC *desc,
        UINT first_sub_resource,
        UINT sub_resource_count,
        UINT64 base_offset,
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT *layouts,
        UINT *row_count,
        UINT64 *row_size,
        UINT64 *total_bytes) override {
        native_->GetCopyableFootprints(desc, first_sub_resource, sub_resource_count, base_offset, layouts,
            row_count, row_size, total_bytes);
    }
    HRESULT STDMETHODCALLTYPE CreateQueryHeap(
        const D3D12_QUERY_HEAP_DESC *desc,
        REFIID riid,
        void **heap) override {
        return native_->CreateQueryHeap(desc, riid, heap);
    }
    HRESULT STDMETHODCALLTYPE SetStablePowerState(
        WINBOOL enable) override {
        return native_->SetStablePowerState(enable);
    }
    HRESULT STDMETHODCALLTYPE CreateCommandSignature(
        const D3D12_COMMAND_SIGNATURE_DESC *desc,
        ID3D12RootSignature *root_signature,
        REFIID riid,
        void **command_signature) override {
        return native_->CreateCommandSignature(desc, root_signature, riid, command_signature);
    }
    void STDMETHODCALLTYPE GetResourceTiling(
        ID3D12Resource *resource,
        UINT *total_tile_count,
        D3D12_PACKED_MIP_INFO *packed_mip_info,
        D3D12_TILE_SHAPE *standard_tile_shape,
        UINT *sub_resource_tiling_count,
        UINT first_sub_resource_tiling,
        D3D12_SUBRESOURCE_TILING *sub_resource_tilings) override {
        native_->GetResourceTiling(resource, total_tile_count, packed_mip_info, standard_tile_shape,
            sub_resource_tiling_count, first_sub_resource_tiling, sub_resource_tilings);
    }
#ifdef WIDL_EXPLICIT_AGGREGATE_RETURNS
    LUID* STDMETHODCALLTYPE GetAdapterLuid(
        LUID *__ret) override {
        return native_->GetAdapterLuid(__ret);
    }
#else
    LUID STDMETHODCALLTYPE GetAdapterLuid() override {
        return native_->GetAdapterLuid();
    }
#endif
private:
    ~QueueDeviceViewProbe() { --g_deviceViewProbeObjects; }
    std::atomic<ULONG> references_{1};
    Microsoft::WRL::ComPtr<ID3D12Device> native_;
};

class DeviceViewQueueProbe final : public QueueDispatchProbe {
public:
    DeviceViewQueueProbe(ID3D12CommandQueue* native, ID3D12Device* view) : QueueDispatchProbe(native), view_(view) {}
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID id, void** out) override {
        ++deviceQueries_;
        if (!view_) {
            if (out) *out = nullptr;
            return E_FAIL;
        }
        return view_->QueryInterface(id, out);
    }
    void STDMETHODCALLTYPE ExecuteCommandLists(UINT count, ID3D12CommandList* const* lists) override {
        Submit(count, lists, 3);
    }
    uint32_t DeviceQueries() const { return deviceQueries_.load(); }
private:
    Microsoft::WRL::ComPtr<ID3D12Device> view_;
    std::atomic<uint32_t> deviceQueries_{0};
};
}  // namespace ce::flow
