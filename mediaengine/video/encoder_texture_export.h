// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall
#pragma once

// The encoder input textures a producer renders into directly, and the shared fence it
// signals: the DXVK zero-copy path, where the Vulkan layer imports their KMT handles.
//
// One pipeline owns one export, outside the codec. The inject thread (which the stop path
// joins before it acknowledges) and the media main loop (which handles the stop command)
// create and release it, so it must never wait for an encode. It therefore owns its own
// D3D11 device on the producer's adapter and its own locks, and touches no encoder state.
// The encoder reaches the textures by handle and opens them on its own device, as it opens
// any producer texture (ExportedTextureViews). An opened view aliases the same allocation,
// so the path stays zero-copy.
//
// Locks: Ensure and Release serialize on their own lock, which covers the device work,
// and take the state lock only to swap the set. The encoder takes only the state lock, for
// one OpenSharedResource per slot and generation. Neither lock is held while waiting for
// anything else; the publication guard is taken after them and only for atomic stores.

#include <d3d11_4.h>
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>

#include "common/capture/recording_memory.h"
#include "common/ipc/shared_defs.h"
#include "common/logging/log_meter.h"
#include "common/platform/raii_helpers.h"

namespace ce::media {

struct EncoderTextureRequest {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t format = 0;  // DXGI_FORMAT the producer renders
    int32_t luidLow = 0;  // the producer's adapter; zero selects the default hardware adapter
    int32_t luidHigh = 0;
};

class EncoderTextureExport {
public:
    explicit EncoderTextureExport(RecordingMemory& route);
    // Releases the set and its device without touching the publication: the route may be
    // gone by then, and a destroyed pipeline never withdrew its textures.
    ~EncoderTextureExport();
    EncoderTextureExport(const EncoderTextureExport&) = delete;
    EncoderTextureExport& operator=(const EncoderTextureExport&) = delete;

    // Publishes a texture set for the request. `binding` becomes the route only when the
    // route has none yet: a detached recording keeps publishing privately. A set of the
    // same format is kept, since producers re-request it; a format change retires the old
    // set (and its publication) first. False when no set could be created.
    bool Ensure(const EncoderTextureRequest& request, SharedMemoryLayout* binding);

    // Withdraws the publication and releases the set and its device. False when nothing
    // was exported.
    bool Release(const char* reason);

    bool Exported() const { return exported_.load(std::memory_order_acquire); }
    // Changes with every created and every released set; 0 before the first.
    uint64_t Generation() const { return generation_.load(std::memory_order_acquire); }

    struct Handles {
        uint64_t generation = 0;
        HANDLE kmt[ENCODER_TEXTURE_SLOT_COUNT] = {};
    };
    Handles Snapshot() const;

    // A new reference to slot `slot` of `generation`, opened on `device`; null when that
    // generation is gone or the slot cannot be opened there.
    ID3D11Texture2D* OpenOn(ID3D11Device* device, uint64_t generation, int slot) const;

private:
    struct Set;

    ID3D11Device5* DeviceFor(const EncoderTextureRequest& request, const char** failedStage, HRESULT* hr);
    void Withdraw();
    void Publish(const Set& set, uint64_t generation, bool fresh);
    void LogFailure(const EncoderTextureRequest& request, const char* stage, HRESULT hr);

    RecordingMemory& route_;
    std::mutex serialize_;  // Ensure/Release; guards the device
    mutable std::mutex state_;  // guards set_
    ComGuard<ID3D11Device5> device_;
    int32_t deviceLuidLow_ = 0;
    int32_t deviceLuidHigh_ = 0;
    std::unique_ptr<Set> set_;
    std::atomic<uint64_t> generation_{0};
    std::atomic<bool> exported_{false};
    log_meter::ChangeGate failureGate_;
    mutable log_meter::ChangeGate openFailureGate_;
};

// The encoder's views of the exported textures on its own device. Encoder thread only.
class ExportedTextureViews {
public:
    ExportedTextureViews() = default;
    ~ExportedTextureViews() { Reset(); }
    ExportedTextureViews(const ExportedTextureViews&) = delete;
    ExportedTextureViews& operator=(const ExportedTextureViews&) = delete;

    // A new reference to the view of the exported texture that carries `kmtHandle`, null
    // when no exported slot does. `slot` receives the slot it matched.
    ID3D11Texture2D* Acquire(const EncoderTextureExport& source, ID3D11Device* device, HANDLE kmtHandle,
                             int* slot = nullptr);
    // A new reference to the view of exported slot `slot`, null when it is not exported.
    ID3D11Texture2D* AcquireSlot(const EncoderTextureExport& source, ID3D11Device* device, int slot);
    // Releases every view. Call before the device they were opened on goes away.
    void Reset();

private:
    void Refresh(const EncoderTextureExport& source, ID3D11Device* device);
    ID3D11Texture2D* View(const EncoderTextureExport& source, int slot);

    uint64_t generation_ = 0;
    ID3D11Device* device_ = nullptr;  // identity of the device the views belong to
    HANDLE handles_[ENCODER_TEXTURE_SLOT_COUNT] = {};
    ID3D11Texture2D* views_[ENCODER_TEXTURE_SLOT_COUNT] = {};
    bool unopenable_[ENCODER_TEXTURE_SLOT_COUNT] = {};  // not retried within a generation
};

}  // namespace ce::media
