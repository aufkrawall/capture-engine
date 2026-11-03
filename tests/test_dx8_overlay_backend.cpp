#define CINTERFACE 1

#include <windows.h>

#include <d3d8.h>

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <utility>
#include <vector>

#include "hook/overlay/custom_overlay_dx8.h"

// The D3D8 overlay backend's upload and draw. Direct3D 8 takes the base vertex in SetIndices, not in the
// draw, so the two runs of a frame that kept its unchanged part and wrote only its tail go through a
// different call than on D3D9 (tests/test_dx9_overlay_backend.cpp).
//
// There is no 64-bit d3d8.dll and this binary is 64-bit, so the device is a fake: its buffers are plain
// memory, and a draw resolves each index through the bound base vertex to what the buffer holds there,
// as a GPU would. That is the whole contract here.
namespace {

static_assert(sizeof(void*) == 8, "AnyCall relies on the x64 convention: the caller cleans up its arguments");

using CustomOverlay::DrawCommand;
using CustomOverlay::DrawVertex;
using CustomOverlay::DX8Backend;

// DX8Vertex in custom_overlay_dx8.cpp.
struct UploadedVertex {
    float x, y, z, rhw;
    DWORD color;
    float u, v;
};
static_assert(sizeof(UploadedVertex) == 28);

struct FakeResource {
    void* const* vtbl = nullptr;
    std::vector<uint8_t> memory;
    int locks = 0;
    UINT lockedOffset = 0;
    UINT lockedSize = 0;
    DWORD lockedFlags = 0;
};

struct Resolved {
    float x;
    DWORD color;
};

struct FakeDevice {
    void* const* vtbl = nullptr;
    FakeResource vertices;
    FakeResource indices;
    FakeResource texture;
    UINT boundBase = 0;
    int draws = 0;
    bool indexOutsideDeclaredRange = false;
    std::vector<Resolved> drawn;
};

// Everything else the backend calls: saving and restoring state, which is not what this file tests.
HRESULT STDMETHODCALLTYPE AnyCall() {
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE BufferLock(FakeResource* self, UINT offset, UINT size, BYTE** out, DWORD flags) {
    ++self->locks;
    self->lockedOffset = offset;
    self->lockedSize = size;
    self->lockedFlags = flags;
    *out = self->memory.data() + offset;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE TextureLockRect(FakeResource* self, UINT, D3DLOCKED_RECT* rect, const RECT*, DWORD) {
    rect->pBits = self->memory.data();
    rect->Pitch = 4 * 4;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE CreateTexture(FakeDevice* self, UINT width, UINT height, UINT, DWORD, D3DFORMAT, D3DPOOL,
                                        IDirect3DTexture8** out) {
    self->texture.memory.resize(static_cast<size_t>(width) * height * 4u);
    *out = reinterpret_cast<IDirect3DTexture8*>(&self->texture);
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE CreateVertexBuffer(FakeDevice* self, UINT length, DWORD, DWORD, D3DPOOL,
                                             IDirect3DVertexBuffer8** out) {
    self->vertices.memory.resize(length);
    *out = reinterpret_cast<IDirect3DVertexBuffer8*>(&self->vertices);
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE CreateIndexBuffer(FakeDevice* self, UINT length, DWORD, D3DFORMAT, D3DPOOL,
                                            IDirect3DIndexBuffer8** out) {
    self->indices.memory.resize(length);
    *out = reinterpret_cast<IDirect3DIndexBuffer8*>(&self->indices);
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE SetIndices(FakeDevice* self, IDirect3DIndexBuffer8*, UINT base) {
    self->boundBase = base;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE DrawIndexedPrimitive(FakeDevice* self, D3DPRIMITIVETYPE, UINT minIndex, UINT vertexCount,
                                               UINT startIndex, UINT primitiveCount) {
    ++self->draws;
    const auto* indices = reinterpret_cast<const uint16_t*>(self->indices.memory.data()) + startIndex;
    const auto* vertices = reinterpret_cast<const UploadedVertex*>(self->vertices.memory.data());
    for (UINT i = 0; i < primitiveCount * 3; ++i) {
        const UINT index = indices[i];
        if (index < minIndex || index >= minIndex + vertexCount)
            self->indexOutsideDeclaredRange = true;
        const UploadedVertex& vertex = vertices[self->boundBase + index];
        self->drawn.push_back({vertex.x, vertex.color});
    }
    return D3D_OK;
}

#define SLOT(Vtbl, Method) (offsetof(Vtbl, Method) / sizeof(void*))

template <typename Vtbl>
std::vector<void*> MakeTable(std::initializer_list<std::pair<size_t, void*>> overrides) {
    std::vector<void*> table(sizeof(Vtbl) / sizeof(void*), reinterpret_cast<void*>(&AnyCall));
    for (const auto& [slot, function] : overrides)
        table[slot] = function;
    return table;
}

constexpr uint32_t kKeptRed = 0xFF0000FF;    // ABGR
constexpr uint32_t kTailGreen = 0xFF00FF00;  // ABGR
constexpr uint32_t kProbeBlue = 0xFFFF0000;  // ABGR
constexpr DWORD kRedArgb = 0xFFFF0000u;
constexpr DWORD kGreenArgb = 0xFF00FF00u;
constexpr DWORD kBlueArgb = 0xFF0000FFu;
constexpr uint32_t kKeptVertices = 4;
constexpr uint32_t kKeptIndices = 6;
constexpr UINT kFrameBytes = 8 * sizeof(UploadedVertex);
constexpr UINT kTailBytes = 4 * sizeof(UploadedVertex);

class DX8OverlayUploadTest : public testing::Test {
protected:
    void SetUp() override {
        deviceTable_ = MakeTable<IDirect3DDevice8Vtbl>({
            {SLOT(IDirect3DDevice8Vtbl, CreateTexture), reinterpret_cast<void*>(&CreateTexture)},
            {SLOT(IDirect3DDevice8Vtbl, CreateVertexBuffer), reinterpret_cast<void*>(&CreateVertexBuffer)},
            {SLOT(IDirect3DDevice8Vtbl, CreateIndexBuffer), reinterpret_cast<void*>(&CreateIndexBuffer)},
            {SLOT(IDirect3DDevice8Vtbl, SetIndices), reinterpret_cast<void*>(&SetIndices)},
            {SLOT(IDirect3DDevice8Vtbl, DrawIndexedPrimitive), reinterpret_cast<void*>(&DrawIndexedPrimitive)},
        });
        vertexTable_ = MakeTable<IDirect3DVertexBuffer8Vtbl>(
            {{SLOT(IDirect3DVertexBuffer8Vtbl, Lock), reinterpret_cast<void*>(&BufferLock)}});
        indexTable_ = MakeTable<IDirect3DIndexBuffer8Vtbl>(
            {{SLOT(IDirect3DIndexBuffer8Vtbl, Lock), reinterpret_cast<void*>(&BufferLock)}});
        textureTable_ = MakeTable<IDirect3DTexture8Vtbl>(
            {{SLOT(IDirect3DTexture8Vtbl, LockRect), reinterpret_cast<void*>(&TextureLockRect)}});
        device_.vtbl = deviceTable_.data();
        device_.vertices.vtbl = vertexTable_.data();
        device_.indices.vtbl = indexTable_.data();
        device_.texture.vtbl = textureTable_.data();

        backend_ = std::make_unique<DX8Backend>(reinterpret_cast<IDirect3DDevice8*>(&device_));
        const std::vector<uint8_t> font(4 * 4 * 4, 0xFF);
        ASSERT_TRUE(backend_->Initialize(4, 4, font.data()));
    }

    // A quad at x=0 in the kept part and one at x=tailLeft in the tail, in one command across both.
    void SetFrame(float tailLeft, uint32_t keptColor) {
        vertices_.clear();
        const auto addQuad = [&](float left, uint32_t color) {
            vertices_.push_back({left, 0, 0, 0, color});
            vertices_.push_back({left + 8, 0, 1, 0, color});
            vertices_.push_back({left + 8, 8, 1, 1, color});
            vertices_.push_back({left, 8, 0, 1, color});
        };
        addQuad(0, keptColor);
        addQuad(tailLeft, kTailGreen);
        indices_ = {0, 1, 2, 0, 2, 3, 4, 5, 6, 4, 6, 7};
        DrawCommand all = {};
        all.vertexCount = 8;
        all.indexCount = 12;
        all.useTexture = true;
        commands_ = {all};
    }

    void Render() {
        device_.drawn.clear();
        device_.draws = 0;
        backend_->Render(vertices_, indices_, commands_, 64, 64);
    }

    // What the draws read: for every index, the frame's vertex with that number.
    void ExpectDrawn(DWORD keptColor, const char* what) {
        ASSERT_EQ(device_.drawn.size(), indices_.size()) << what;
        EXPECT_FALSE(device_.indexOutsideDeclaredRange) << what;
        for (size_t i = 0; i < indices_.size(); ++i) {
            EXPECT_FLOAT_EQ(device_.drawn[i].x, vertices_[indices_[i]].x - 0.5f) << what << ", index " << i;
            EXPECT_EQ(device_.drawn[i].color, i < kKeptIndices ? keptColor : kGreenArgb) << what << ", index " << i;
        }
    }

    FakeDevice device_;
    std::vector<void*> deviceTable_, vertexTable_, indexTable_, textureTable_;
    std::unique_ptr<DX8Backend> backend_;
    std::vector<DrawVertex> vertices_;
    std::vector<uint16_t> indices_;
    std::vector<DrawCommand> commands_;
};

}  // namespace

// Changing the part the frame claims is unchanged is a probe: the old color reaching the draw shows it
// read the vertices an earlier frame left there.
TEST_F(DX8OverlayUploadTest, ATailOnlyFrameWritesTheTailAndDrawsEachRunThroughItsOwnBase) {
    SetFrame(16, kKeptRed);
    backend_->OnDrawDataChanged();
    Render();
    EXPECT_EQ(device_.vertices.locks, 1);
    EXPECT_EQ(device_.vertices.lockedSize, kFrameBytes);
    EXPECT_EQ(device_.vertices.lockedFlags, static_cast<DWORD>(D3DLOCK_DISCARD));
    EXPECT_EQ(device_.draws, 1);
    ExpectDrawn(kRedArgb, "the whole frame");

    UINT expectedOffset = kFrameBytes;
    for (int frame = 1; frame < 4; ++frame) {
        SetFrame(16.0f + 8.0f * static_cast<float>(frame), kProbeBlue);
        backend_->OnDrawDataTailChanged(kKeptVertices, kKeptIndices);
        Render();
        EXPECT_EQ(device_.vertices.locks, 1 + frame);
        EXPECT_EQ(device_.vertices.lockedOffset, expectedOffset);
        EXPECT_EQ(device_.vertices.lockedSize, kTailBytes) << "only the tail is written";
        EXPECT_EQ(device_.vertices.lockedFlags, static_cast<DWORD>(D3DLOCK_NOOVERWRITE));
        EXPECT_EQ(device_.draws, 2);
        ExpectDrawn(kRedArgb, "a tail-only frame");
        expectedOffset += kTailBytes;
    }
    EXPECT_EQ(device_.indices.locks, 1) << "the indices did not change";

    Render();
    EXPECT_EQ(device_.vertices.locks, 4) << "a cached frame writes nothing";
    EXPECT_EQ(device_.draws, 2);
    ExpectDrawn(kRedArgb, "a cached frame");

    SetFrame(40, kProbeBlue);
    backend_->OnDrawDataChanged();
    Render();
    EXPECT_EQ(device_.vertices.lockedOffset, expectedOffset);
    EXPECT_EQ(device_.vertices.lockedSize, kFrameBytes);
    EXPECT_EQ(device_.draws, 1);
    ExpectDrawn(kBlueArgb, "a whole frame replaces the kept part");
}

TEST_F(DX8OverlayUploadTest, ARecreatedBufferHoldsNothingToKeep) {
    SetFrame(16, kKeptRed);
    backend_->OnDrawDataChanged();
    Render();

    backend_->Shutdown();
    const std::vector<uint8_t> font(4 * 4 * 4, 0xFF);
    ASSERT_TRUE(backend_->Initialize(4, 4, font.data()));
    SetFrame(24, kProbeBlue);
    backend_->OnDrawDataTailChanged(kKeptVertices, kKeptIndices);
    Render();
    EXPECT_EQ(device_.vertices.lockedSize, kFrameBytes);
    EXPECT_EQ(device_.vertices.lockedFlags, static_cast<DWORD>(D3DLOCK_DISCARD));
    ExpectDrawn(kBlueArgb, "the first frame in the new buffer");
}
