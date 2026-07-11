#pragma once

#include <cstdint>

#include <d3d12.h>
#include <dxgi.h>

namespace ce::dx12_streamline_ui_overlay {

struct RecordRequest {
    ID3D12GraphicsCommandList* commandList = nullptr;
    ID3D12Resource* uiResource = nullptr;
    ID3D12CommandQueue* initializationQueue = nullptr;
    D3D12_RESOURCE_STATES resourceState = D3D12_RESOURCE_STATE_COMMON;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    uint32_t width = 0;
    uint32_t height = 0;
    bool hdr = false;
    const void* frameToken = nullptr;
};

// Starts one DLSS-G activation epoch. CE records the overlay into the first official
// UIColorAndAlpha resource offered for that epoch. The app's command list carries the draw,
// so no extra queue submission, copy, wait, or backbuffer ownership guess is introduced.
void BeginActivation(uint32_t maximumOutputPresents);
void EndActivation(const char* reason);

// Called for every slSetTagForFrame packet before attempting a bootstrap record. A new frame
// token ends the prior UI-coverage lifetime (eValidUntilPresent) so PostSL resumes normally.
// Returns true only for the one frame in the current activation that still needs a record.
// Callers use this as the hot-path gate before inspecting any tag resources or acquiring a queue.
bool OnFrameTag(const void* frameToken);
bool TryRecordBootstrap(const RecordRequest& request);

// The DX12 ECL detour brackets the real app submission with these calls. BeforeSubmit arms
// visible-output coverage before a wrapped queue can re-enter Present; AfterSubmit signals the
// exact submission queue so upload/descriptor slots are never reused without completion proof.
bool BeforeExecuteCommandLists(UINT count, ID3D12CommandList* const* commandLists);
void AfterExecuteCommandLists(ID3D12CommandQueue* queue, UINT count,
                              ID3D12CommandList* const* commandLists);

// Returns true when the current PostSL output is already covered by the official Streamline UI
// resource and must not receive a duplicate output-backbuffer draw.
bool ConsumePostSLCoverage();

void Shutdown(const char* reason);

}  // namespace ce::dx12_streamline_ui_overlay
