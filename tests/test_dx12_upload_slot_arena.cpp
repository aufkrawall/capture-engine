#include <gtest/gtest.h>

#include <filesystem>
#include <string>

#include "../hook/common/dx12_overlay_policy/upload_slot_arena.h"
#include "source_fragment_reader.h"

// GTA session 20260925_225006: every overlay renderer created 32 committed upload buffers (~6 ms), and at FSR FG
// activation every slot's 16384-byte index buffer overflowed (16416-16584 bytes needed), recreating 16 index
// buffers on AMD's presenter thread. The ring now carves all slots from one allocation with a 3:1 index budget.

namespace {

using ce::dx12_overlay_policy::kInitialUploadSlotIndices;
using ce::dx12_overlay_policy::kInitialUploadSlotVertices;
using ce::dx12_overlay_policy::MakeUploadSlotArenaLayout;

constexpr std::size_t kVertexStride = 20;  // DrawVertex: float2 pos, float2 uv, RGBA8

TEST(DX12UploadSlotArena, RegionsAreAlignedAndDisjoint) {
    const auto layout = MakeUploadSlotArenaLayout(16, kInitialUploadSlotVertices * kVertexStride,
                                                  kInitialUploadSlotIndices * sizeof(std::uint16_t));
    for (std::size_t slot = 0; slot < 16; ++slot) {
        EXPECT_EQ(layout.VertexOffset(slot) % 256, 0u);
        EXPECT_EQ(layout.IndexOffset(slot) % 256, 0u);
        EXPECT_GE(layout.IndexOffset(slot), layout.VertexOffset(slot) + layout.vertexBytes);
        if (slot + 1 < 16) {
            EXPECT_GE(layout.VertexOffset(slot + 1), layout.IndexOffset(slot) + layout.indexBytes);
        }
    }
    EXPECT_GE(layout.totalBytes, layout.IndexOffset(15) + layout.indexBytes);
}

TEST(DX12UploadSlotArena, UnalignedRegionSizesStillProduceAlignedOffsets) {
    const auto layout = MakeUploadSlotArenaLayout(3, 1000, 30, 256);
    EXPECT_EQ(layout.IndexOffset(0), 1024u);
    EXPECT_EQ(layout.slotStride, 1280u);
    EXPECT_EQ(layout.VertexOffset(2), 2560u);
    EXPECT_EQ(layout.totalBytes, 3840u);
}

TEST(DX12UploadSlotArena, IndexBudgetCoversTheFrameTimeGraphThatOverflowedIt) {
    constexpr std::size_t kObservedFgGraphIndexBytes = 16584;
    EXPECT_GE(kInitialUploadSlotIndices * sizeof(std::uint16_t), kObservedFgGraphIndexBytes);
    EXPECT_GE(kInitialUploadSlotIndices, 3 * kInitialUploadSlotVertices);
}

std::string ReadSource(const char* relativePath) {
    return ce::test_source::ReadFile(std::filesystem::current_path() / relativePath);
}

TEST(DX12UploadSlotArenaSource, RingIsOneAllocationAndDrawsBindSlotAddresses) {
    const std::string backend = ReadSource("hook/common/custom_overlay_dx12.cpp");
    const std::string render = ReadSource("hook/common/custom_overlay_dx12_render.cpp");
    ASSERT_FALSE(backend.empty());
    ASSERT_FALSE(render.empty());
    const size_t start = backend.find("bool DX12Backend::CreateBuffers()");
    const size_t stop = backend.find("bool DX12Backend::CreateFontTexture(", start);
    ASSERT_NE(start, std::string::npos);
    const std::string createBuffers = backend.substr(start, stop - start);

    size_t creates = 0;
    for (size_t at = createBuffers.find("CreateCommittedResource("); at != std::string::npos;
         at = createBuffers.find("CreateCommittedResource(", at + 1)) {
        ++creates;
    }
    EXPECT_EQ(creates, 1u);
    EXPECT_EQ(createBuffers.find("for (int i = 0; i < kFramePoolSize; i++) {\n        vertexBufferSize[i] = initVBSize;"),
              std::string::npos);
    EXPECT_NE(render.find("vbv.BufferLocation = vertexBufferGpu[slot];"), std::string::npos);
    EXPECT_NE(render.find("ibv.BufferLocation = indexBufferGpu[slot];"), std::string::npos);
    EXPECT_EQ(render.find("vertexBuffer[slot]->GetGPUVirtualAddress()"), std::string::npos)
        << "arena-backed slots own no resource of their own";
}

}  // namespace
