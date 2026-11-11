// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall
// ReShade's parts of the Present-chain overlay stand-in (present_chain_layer.cpp): its add-on API exports, by which CE
// identifies a renamed ReShade copy as well (hook/runtime/main_overlay_detect.cpp), and its command queue wrapper:
// ReShade hands every queue its device creates out as its own object, which forwards submissions to the native queue
// and answers a native-interface query with it. A frame generation runtime's composition then reaches the native queue
// from ReShade's module (GTA V Enhanced with ReShade, session gtafsroverlaystutteryghosty).

#include <d3d12.h>
#include <windows.h>

#include <cstdint>

#include "hook/d3d12/dx12_queue_identity.h"
#include "tests/flow/queue_dispatch_probe.h"

namespace {

class ReShadeQueue final : public ce::flow::QueueDispatchProbe {
public:
    explicit ReShadeQueue(ID3D12CommandQueue* native) : QueueDispatchProbe(native), native_(native) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** out) override {
        if (id == ce::dx12_queue_identity::kReShadeNative)
            return native_->QueryInterface(__uuidof(ID3D12CommandQueue), out);
        return QueueDispatchProbe::QueryInterface(id, out);
    }

    [[clang::noinline, clang::disable_tail_calls]] void STDMETHODCALLTYPE ExecuteCommandLists(
        UINT count, ID3D12CommandList* const* lists) override {
        Submit(count, lists, 4);
    }

private:
    ID3D12CommandQueue* native_;  // retained by QueueDispatchProbe
};

}  // namespace

extern "C" __declspec(dllexport) bool ReShadeRegisterAddon(HMODULE /*module*/, uint32_t /*apiVersion*/) {
    return true;
}

extern "C" __declspec(dllexport) void ReShadeUnregisterAddon(HMODULE /*module*/) {}

// ReShade's object for a queue the device created; returns a new reference.
extern "C" __declspec(dllexport) ID3D12CommandQueue* CEFakeOverlay_WrapQueue(ID3D12CommandQueue* native) {
    return native ? new ReShadeQueue(native) : nullptr;
}
