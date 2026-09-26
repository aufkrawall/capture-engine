#pragma once

#include <stdint.h>

#include "shared_defs.h"

namespace ce {

// Handles an inject frame is read with, plus whether they provably belong to
// the transport generation the producer stamped on that frame.
struct InjectTransportSnapshot {
    uint64_t sharedHandle = 0;
    uint64_t fenceHandle = 0;
    uint64_t generation = 0;  // generation observed before and after the handle reads
    bool consistent = false;
};

// True when both generation reads agree and match the frame's stamp. Only then
// are the handles read in between the frame's own: a producer that re-creates
// its transport starts the next generation before storing any new handle, so a
// newer handle observed in between always shows up in the second read.
inline bool IsInjectTransportSnapshotConsistent(uint64_t generationBefore, uint64_t generationAfter,
                                                uint32_t frameGeneration) {
    return generationBefore == generationAfter && static_cast<uint32_t>(generationBefore) == frameGeneration;
}

// Reads the texture/fence handles for one inject frame under the transport
// generation protocol (see SharedMemoryLayout::BeginTransportGeneration).
inline InjectTransportSnapshot ReadInjectTransportSnapshot(const SharedMemoryLayout& sharedMem, int textureIndex,
                                                           bool useEncoderTextureFence, uint32_t frameGeneration) {
    InjectTransportSnapshot snapshot;
    const uint64_t generationBefore = sharedMem.GetTransportGeneration();
    snapshot.sharedHandle = sharedMem.GetSharedHandle(textureIndex);
    snapshot.fenceHandle = useEncoderTextureFence ? sharedMem.encoderTextures.GetFenceHandle()
                                                  : sharedMem.GetFenceShareHandle();
    const uint64_t generationAfter = sharedMem.GetTransportGeneration();
    snapshot.generation = generationAfter;
    snapshot.consistent = IsInjectTransportSnapshotConsistent(generationBefore, generationAfter, frameGeneration);
    return snapshot;
}

}  // namespace ce
