#pragma once

// GPU time of CE's DX11 overlay draw, for the perf CSV's overlay_gpu_us (until 2026-10-17 DX11 left
// it at zero). Timestamp queries bracket the draw on the game's immediate context, only while the
// perf CSV is being written; results are read back frames later with DONOTFLUSH, so neither the
// overlay nor the present ever waits for them. A frame whose query set is still in flight is not
// timed rather than waited for.

#include <d3d11.h>

#include <cstdint>

namespace ce::dx11_overlay_gpu {

// Around the overlay's draw calls. `device` creates the queries on first use (and again after a
// device change).
void BeginOverlay(ID3D11Device* device, ID3D11DeviceContext* context);
void EndOverlay(ID3D11DeviceContext* context);
// Polls finished query sets; the GPU microseconds of the newest one, 0 before the first result.
int32_t PollLatestUs(ID3D11DeviceContext* context);
// Drops the queries. `releaseObjects` false: the device is being torn down, forget them unreleased.
void Release(bool releaseObjects);

}  // namespace ce::dx11_overlay_gpu
