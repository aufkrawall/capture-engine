#pragma once

#include <d3d12.h>
#include <wrl/client.h>

#include <atomic>

namespace ce::flow {

// Two genuine COM implementations over the same native queue/device. Their ECL methods differ:
// device equality cannot establish which saved method accepts a particular receiver.
inline std::atomic<uint32_t> g_queueProbeObjects{0};

class QueueDispatchProbe : public ID3D12CommandQueue {
public:
    explicit QueueDispatchProbe(ID3D12CommandQueue* native) : native_(native) {
        ++g_queueProbeObjects;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** out) override {
        if (!out)
            return E_POINTER;
        *out = nullptr;
        if (id == __uuidof(IUnknown) || id == __uuidof(ID3D12Object) || id == __uuidof(ID3D12DeviceChild) ||
            id == __uuidof(ID3D12Pageable) || id == __uuidof(ID3D12CommandQueue)) {
            *out = static_cast<ID3D12CommandQueue*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {
        return ++references_;
    }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG remaining = --references_;
        if (!remaining)
            delete this;
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
    HRESULT STDMETHODCALLTYPE SetName(LPCWSTR name) override {
        return native_->SetName(name);
    }
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID id, void** out) override {
        return native_->GetDevice(id, out);
    }
    void STDMETHODCALLTYPE UpdateTileMappings(ID3D12Resource* resource, UINT regionCount,
                                              const D3D12_TILED_RESOURCE_COORDINATE* starts,
                                              const D3D12_TILE_REGION_SIZE* sizes, ID3D12Heap* heap, UINT rangeCount,
                                              const D3D12_TILE_RANGE_FLAGS* rangeFlags, const UINT* offsets,
                                              const UINT* counts, D3D12_TILE_MAPPING_FLAGS flags) override {
        native_->UpdateTileMappings(resource, regionCount, starts, sizes, heap, rangeCount, rangeFlags, offsets, counts,
                                    flags);
    }
    void STDMETHODCALLTYPE CopyTileMappings(ID3D12Resource* destination,
                                            const D3D12_TILED_RESOURCE_COORDINATE* destinationStart,
                                            ID3D12Resource* source, const D3D12_TILED_RESOURCE_COORDINATE* sourceStart,
                                            const D3D12_TILE_REGION_SIZE* size,
                                            D3D12_TILE_MAPPING_FLAGS flags) override {
        native_->CopyTileMappings(destination, destinationStart, source, sourceStart, size, flags);
    }
    void STDMETHODCALLTYPE SetMarker(UINT metadata, const void* data, UINT size) override {
        native_->SetMarker(metadata, data, size);
    }
    void STDMETHODCALLTYPE BeginEvent(UINT metadata, const void* data, UINT size) override {
        native_->BeginEvent(metadata, data, size);
    }
    void STDMETHODCALLTYPE EndEvent() override {
        native_->EndEvent();
    }
    HRESULT STDMETHODCALLTYPE Signal(ID3D12Fence* fence, UINT64 value) override {
        return native_->Signal(fence, value);
    }
    HRESULT STDMETHODCALLTYPE Wait(ID3D12Fence* fence, UINT64 value) override {
        return native_->Wait(fence, value);
    }
    HRESULT STDMETHODCALLTYPE GetTimestampFrequency(UINT64* frequency) override {
        return native_->GetTimestampFrequency(frequency);
    }
    HRESULT STDMETHODCALLTYPE GetClockCalibration(UINT64* gpu, UINT64* cpu) override {
        return native_->GetClockCalibration(gpu, cpu);
    }
#ifdef WIDL_EXPLICIT_AGGREGATE_RETURNS
    D3D12_COMMAND_QUEUE_DESC* STDMETHODCALLTYPE GetDesc(D3D12_COMMAND_QUEUE_DESC* out) override {
        *out = native_->GetDesc();
        return out;
    }
#else
    D3D12_COMMAND_QUEUE_DESC STDMETHODCALLTYPE GetDesc() override {
        return native_->GetDesc();
    }
#endif

    uint32_t Calls() const {
        return calls_.load();
    }
    void* CurrentECL() {
        return (*reinterpret_cast<void***>(this))[10];
    }
    void* CurrentSignal() {
        return (*reinterpret_cast<void***>(this))[14];
    }
    HRESULT InvokeSignal(UINT64 value) {
        using Method = HRESULT(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, ID3D12Fence*, UINT64);
        return reinterpret_cast<Method>(CurrentSignal())(this, nullptr, value);
    }
    uint32_t SignalCalls() const {
        return signalCalls_.load();
    }
    UINT64 LastSignalValue() const {
        return lastSignalValue_.load();
    }

protected:
    HRESULT RecordSignal(ID3D12Fence* fence, UINT64 value, HRESULT result) {
        ++signalCalls_;
        lastSignalValue_.store(value);
        return fence ? native_->Signal(fence, value) : result;
    }
    virtual ~QueueDispatchProbe() {
        --g_queueProbeObjects;
    }
    void Submit(UINT count, ID3D12CommandList* const* lists, uint32_t implementation) {
        lastImplementation_.store(implementation);
        ++calls_;
        // Empty calls characterize CPU forwarding without submitting a GPU batch or changing frame state.
        if (count)
            native_->ExecuteCommandLists(count, lists);
    }

private:
    std::atomic<ULONG> references_{1};
    std::atomic<uint32_t> calls_{0};
    std::atomic<uint32_t> lastImplementation_{0};
    std::atomic<uint32_t> signalCalls_{0};
    std::atomic<UINT64> lastSignalValue_{0};
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> native_;
};

class FirstQueueDispatchProbe final : public QueueDispatchProbe {
public:
    using QueueDispatchProbe::QueueDispatchProbe;
    void STDMETHODCALLTYPE ExecuteCommandLists(UINT count, ID3D12CommandList* const* lists) override {
        Submit(count, lists, 1);
    }
    HRESULT STDMETHODCALLTYPE Signal(ID3D12Fence* fence, UINT64 value) override {
        return RecordSignal(fence, value, S_OK);
    }
};

class SecondQueueDispatchProbe final : public QueueDispatchProbe {
public:
    using QueueDispatchProbe::QueueDispatchProbe;
    void STDMETHODCALLTYPE ExecuteCommandLists(UINT count, ID3D12CommandList* const* lists) override {
        Submit(count, lists, 2);
    }
    HRESULT STDMETHODCALLTYPE Signal(ID3D12Fence* fence, UINT64 value) override {
        return RecordSignal(fence, value, S_FALSE);
    }
};

}  // namespace ce::flow
