#pragma once

#include "tests/flow/queue_device_view_probe.h"

namespace ce::flow {

// Model a graphics proxy that answers GetDevice with its own view of the
// native device for resources or queues sharing a particular vtable.
class DeviceViewAnswers {
public:
    DeviceViewAnswers(ID3D12DeviceChild* child, ID3D12Device* native, bool allowCommandListCreation = true) {
        view_.Attach(new QueueDeviceViewProbe(native, allowCommandListCreation));
        activeView_ = view_.Get();
        constexpr size_t kGetDeviceSlot = 7;  // IUnknown 3, ID3D12Object 4
        slot_ = &(*reinterpret_cast<void***>(child))[kGetDeviceSlot];
        original_ = *slot_;
        Replace(reinterpret_cast<void*>(&AnswerWithDeviceView));
    }
    ~DeviceViewAnswers() {
        Replace(original_);
        activeView_ = nullptr;
    }
    DeviceViewAnswers(const DeviceViewAnswers&) = delete;
    DeviceViewAnswers& operator=(const DeviceViewAnswers&) = delete;

private:
    static HRESULT STDMETHODCALLTYPE AnswerWithDeviceView(void*, REFIID id, void** out) {
        return activeView_->QueryInterface(id, out);
    }

    void Replace(void* method) {
        DWORD protection = 0;
        VirtualProtect(static_cast<void*>(slot_), sizeof(void*), PAGE_READWRITE, &protection);
        *slot_ = method;
        VirtualProtect(static_cast<void*>(slot_), sizeof(void*), protection, &protection);
    }

    inline static ID3D12Device* activeView_ = nullptr;
    Microsoft::WRL::ComPtr<QueueDeviceViewProbe> view_;
    void** slot_ = nullptr;
    void* original_ = nullptr;
};

}  // namespace ce::flow
