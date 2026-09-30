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

// Which slot carries the inject fence handle. Producer and media must agree, so
// both key on useEncoderTextures: it states that the producer actually hands the
// encoder its own textures. encoderTextures.ready only says the media created
// them, which it does at every recording start - even for producers that never
// adopt them.
inline bool InjectFenceUsesEncoderTextureSlot(const SharedMemoryLayout& sharedMem) {
    return sharedMem.useEncoderTextures.load(std::memory_order_acquire);
}

// Stores the producer's fence handle in the slot ReadInjectTransportSnapshot
// reads it from. The caller begins the transport generation first. Returns
// whether the encoder-texture slot was written.
inline bool PublishInjectFenceHandle(SharedMemoryLayout& sharedMem, uint64_t fenceHandle) {
    const bool encoderTextureSlot = InjectFenceUsesEncoderTextureSlot(sharedMem);
    if (encoderTextureSlot) {
        sharedMem.encoderTextures.SetFenceHandle(fenceHandle);
    } else {
        sharedMem.SetFenceShareHandle(fenceHandle);
    }
    return encoderTextureSlot;
}

// Switches media to the encoder textures a producer has just adopted. Until
// then the encoder-texture fence slot holds the handle media stored for itself:
// a handle value in media's own process that no producer signals. Publish the
// producer's fence before the flag, so every frame media reads through that
// slot finds it. The caller begins the transport generation first.
inline void AdoptEncoderTexturesWithFence(SharedMemoryLayout& sharedMem, uint64_t fenceHandle) {
    sharedMem.encoderTextures.SetFenceHandle(fenceHandle);
    sharedMem.useEncoderTextures.store(true, std::memory_order_release);
}

// Whether a producer that publishes into `sharedMem` must start a new transport
// generation before its next frame. That is the case whenever its capture is about
// to be (re)initialized - Initialize() closes the previous handles, so the
// generation has to begin BEFORE it, or a frame of the outgoing generation that
// media reads in between still passes the generation check against a reused handle
// value - and when the mapping it stamps frames for is not the one it last stamped
// (a replacement host's mapping starts at generation 0, possibly at the same
// address).
inline bool ShouldBeginInjectTransportGeneration(bool captureInitialized, bool sameMappingAsLastPublish,
                                                 uint64_t mappingGeneration, uint32_t lastPublishedGeneration) {
    return !captureInitialized || !sameMappingAsLastPublish ||
           static_cast<uint32_t>(mappingGeneration) != lastPublishedGeneration;
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
