#pragma once

#include <d3d11.h>
#include <cstdint>

// Rebuilds the exact P010 codes of a packed P010 retained-reservoir slot
// (common/capture/packed_p010_layout.h) in the encoder's input frame
// (PS_UnpackP010Y/UV). It replaces the slot copy of the NV12/P010 route: a
// full-frame and a quarter-frame pass in the same submission. The caller
// serializes immediate-context use, as for every other conversion pass.
class PackedP010Unpacker {
public:
    PackedP010Unpacker() = default;
    ~PackedP010Unpacker();
    PackedP010Unpacker(const PackedP010Unpacker&) = delete;
    PackedP010Unpacker& operator=(const PackedP010Unpacker&) = delete;

    // Compiles the device-independent bytecode once per module and keeps it
    // across device teardown. Call before video frames; Init never compiles.
    static bool PrepareShaders();
    bool Init(ID3D11Device* device);
    void Release();

    // The slot's R16_UINT SRV -> P010 plane RTVs (R16_UNORM luma, R16G16_UNORM chroma).
    bool Unpack(ID3D11DeviceContext* context, ID3D11ShaderResourceView* packed, ID3D11RenderTargetView* luma,
                ID3D11RenderTargetView* chroma, UINT width, UINT height);

private:
    ID3D11Device* device_ = nullptr;
    ID3D11VertexShader* vertexShader_ = nullptr;
    ID3D11PixelShader* lumaShader_ = nullptr;
    ID3D11PixelShader* chromaShader_ = nullptr;
};
