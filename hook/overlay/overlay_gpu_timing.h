#pragma once

#include <cstdint>

struct ID3D12Device;
struct ID3D12CommandQueue;
struct ID3D12GraphicsCommandList;

namespace ce::overlay_gpu_timing {

// GPU-side timing for the overlay commands CE appends to the frame-generation
// runtime's own command list.
//
// Every measurement CE has for the FSR FG degradation is a CPU span, and all of
// them are identical between a healthy and a degraded start. What separates the
// two is that the frame reaches the screen about 2.5 ms later while the GPU
// draws less power, which is a stall rather than added work - but "where the
// GPU is between the callback and the flip" has never been observable in
// process. Two timestamps around CE's own commands answer it: if they execute
// late, the delay is upstream of CE (the runtime's or the game's GPU work); if
// they execute on time and the flip is still late, it is downstream.
//
// Diagnostic only, and opt-in through the CE_FG_GPU_TIMING environment
// variable, because writing timestamp queries into a foreign runtime's command
// list is not something a normal run should do. When it is off, nothing is
// allocated and no command is ever recorded.
//
// The read path never waits. A slot is read only after its MARKER_OUT retires,
// the same completion transport the callback upload pool already uses, so no
// fence is signalled, waited on, or queried per frame.
bool Enabled();

// The queue whose timestamp domain the resolved values belong to. Calibration
// runs off the hot path, so this only records which queue to use.
void SetCalibrationQueue(ID3D12CommandQueue* queue);

// Records the opening timestamp for this callback's CE commands. Returns the
// slot, or -1 when timing is off, unavailable, or every slot is still in
// flight. A refused slot means no commands are recorded at all.
int Begin(ID3D12Device* device, ID3D12GraphicsCommandList* list, bool generated, int64_t callbackEndUs);

// Records the closing timestamp, the resolve and the completion marker. Must be
// called with the slot returned by Begin and on the same command list.
void End(ID3D12GraphicsCommandList* list, int slot, int64_t callbackEndUs);

// Hook-service thread only: refreshes the clock calibration and publishes any
// retired slot into the pacing trace. Cheap and allocation-free when off.
void Service();

// Releases device-owned objects. Safe when nothing was ever created.
void Shutdown();

}  // namespace ce::overlay_gpu_timing
