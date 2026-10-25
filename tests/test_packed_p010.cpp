#include <gtest/gtest.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11_4.h>
#include <mmsystem.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <set>
#include <vector>

#include "common/capture/packed_p010_layout.h"
#include "common/platform/raii_helpers.h"
#include "mediaengine/video/packed_p010_unpacker.h"
#include "mediaengine/video/retained_yuv_converter.h"
#include "mediaengine/video/yuv_overlay_patch.h"

// Packed P010 retained slots (common/capture/packed_p010_layout.h). The
// contract is exactness: a packed slot must reconstruct, bit for bit, the P010
// the planar retained route writes for the same frame, for every source format
// and range the route accepts, including the overlay patch that reads it.

namespace {

namespace pp = ce::packed_p010;
using ce::ComGuard;
using ce::video_format::RgbColorTransform;

TEST(PackedP010LayoutTest, EveryCodeRoundTripsThroughTheBlockTexels) {
    for (uint32_t cb = 0; cb < 1024; ++cb) {
        for (uint32_t cr = 0; cr < 1024; cr += 7) {
            const uint32_t word = pp::BlockWord(cb, cr);
            uint16_t texels[4] = {};
            for (uint32_t piece = 0; piece < 4; ++piece) {
                const uint32_t luma = (cb * 31u + cr * 17u + piece * 255u) & pp::kCodeMask;
                texels[piece] = pp::PackTexel(luma, word, piece);
                ASSERT_EQ(pp::LumaCode(texels[piece]), luma);
                ASSERT_EQ(texels[piece] & 0x8000u, 0u);  // bit 15 stays clear
            }
            uint32_t outCb = 0;
            uint32_t outCr = 0;
            pp::ChromaCodes(texels, &outCb, &outCr);
            ASSERT_EQ(outCb, cb);
            ASSERT_EQ(outCr, cr);
        }
    }
}

TEST(PackedP010LayoutTest, NeutralChromaFramesAreOneUniformTexel) {
    // Privacy black clears a packed slot with one value: neutral chroma must
    // contribute nothing to any of the four texels of a block.
    for (uint32_t luma : {0u, 64u, 940u, 1023u}) {
        for (uint32_t piece = 0; piece < 4; ++piece)
            EXPECT_EQ(pp::PackTexel(luma, pp::BlockWord(512, 512), piece), pp::NeutralTexel(luma));
    }
    EXPECT_EQ(pp::PieceIndex(0, 0), 0u);
    EXPECT_EQ(pp::PieceIndex(1, 0), 1u);
    EXPECT_EQ(pp::PieceIndex(0, 1), 2u);
    EXPECT_EQ(pp::PieceIndex(3, 5), 3u);
}

struct Gpu {
    ComGuard<ID3D11Device> device;
    ComGuard<ID3D11DeviceContext> context;
    bool Create() {
        return SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                           nullptr, 0, D3D11_SDK_VERSION, device.addressof(), nullptr,
                                           context.addressof()));
    }
};

constexpr UINT kYuvBind = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

ComGuard<ID3D11Texture2D> MakeTexture(ID3D11Device* device, UINT w, UINT h, DXGI_FORMAT format, UINT bind,
                                      const void* data = nullptr, UINT pitch = 0) {
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = w;
    desc.Height = h;
    desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
    desc.Format = format;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = bind;
    D3D11_SUBRESOURCE_DATA init = {data, pitch, 0};
    ComGuard<ID3D11Texture2D> texture;
    EXPECT_TRUE(SUCCEEDED(device->CreateTexture2D(&desc, data ? &init : nullptr, texture.addressof())));
    return texture;
}

struct PlaneRtvs {
    ComGuard<ID3D11RenderTargetView1> luma;
    ComGuard<ID3D11RenderTargetView1> chroma;
};

bool MakeP010Rtvs(ID3D11Device* device, ID3D11Texture2D* texture, PlaneRtvs* out) {
    ComGuard<ID3D11Device3> device3;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(device3.addressof()))))
        return false;
    auto view = [&](DXGI_FORMAT format, UINT plane, ComGuard<ID3D11RenderTargetView1>& rtv) {
        D3D11_RENDER_TARGET_VIEW_DESC1 desc = {};
        desc.Format = format;
        desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        desc.Texture2D.PlaneSlice = plane;
        return SUCCEEDED(device3->CreateRenderTargetView1(texture, &desc, rtv.addressof()));
    };
    return view(DXGI_FORMAT_R16_UNORM, 0, out->luma) && view(DXGI_FORMAT_R16G16_UNORM, 1, out->chroma);
}

// Every 16-bit sample of a P010 texture (luma rows, then interleaved Cb/Cr rows)
// or an R16_UINT slot, exactly as stored.
std::vector<uint16_t> ReadSamples(Gpu& gpu, ID3D11Texture2D* texture) {
    D3D11_TEXTURE2D_DESC desc = {};
    texture->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;
    ComGuard<ID3D11Texture2D> staging;
    std::vector<uint16_t> out;
    if (FAILED(gpu.device->CreateTexture2D(&desc, nullptr, staging.addressof())))
        return out;
    gpu.context->CopyResource(staging.get(), texture);
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (FAILED(gpu.context->Map(staging.get(), 0, D3D11_MAP_READ, 0, &mapped)))
        return out;
    const auto* bytes = static_cast<const uint8_t*>(mapped.pData);
    const UINT rows = desc.Format == DXGI_FORMAT_P010 ? desc.Height + desc.Height / 2 : desc.Height;
    out.resize(static_cast<size_t>(rows) * desc.Width);
    for (UINT y = 0; y < rows; ++y)
        std::memcpy(&out[static_cast<size_t>(y) * desc.Width], bytes + static_cast<size_t>(y) * mapped.RowPitch,
                    desc.Width * sizeof(uint16_t));
    gpu.context->Unmap(staging.get(), 0);
    return out;
}

struct Case {
    const char* name;
    DXGI_FORMAT rgbFormat;
    RgbColorTransform normalize;
    bool hdr;
    bool bt2020;
    bool fullRange;
    UINT width;
    UINT height;
};

// Per-pixel noise: every 2x2 block carries its own chroma, and the codes
// spread over the whole range, so rounding edges of both passes are hit.
std::vector<uint8_t> NoiseSource(const Case& c, UINT* pitch) {
    const bool fp16 = c.rgbFormat == DXGI_FORMAT_R16G16B16A16_FLOAT;
    const size_t bpp = fp16 ? 8 : 4;
    *pitch = static_cast<UINT>(c.width * bpp);
    std::vector<uint8_t> bytes(static_cast<size_t>(c.width) * c.height * bpp);
    uint32_t state = 0x9E3779B9u ^ c.width ^ (c.height << 16);
    auto next = [&] {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return state;
    };
    for (size_t i = 0; i < static_cast<size_t>(c.width) * c.height; ++i) {
        if (fp16) {
            // Non-negative halves up to 1.0 (SDR) or 12.5 = 1000 nits (scRGB HDR).
            const uint32_t maxBits = c.hdr ? 0x4A40u : 0x3C00u;
            const uint16_t half[4] = {static_cast<uint16_t>(next() % (maxBits + 1)),
                                      static_cast<uint16_t>(next() % (maxBits + 1)),
                                      static_cast<uint16_t>(next() % (maxBits + 1)), 0x3C00u};
            std::memcpy(&bytes[i * 8], half, 8);
        } else if (c.rgbFormat == DXGI_FORMAT_R10G10B10A2_UNORM) {
            const uint32_t packed = (next() & 0x3FFu) | ((next() & 0x3FFu) << 10) | ((next() & 0x3FFu) << 20) | (3u << 30);
            std::memcpy(&bytes[i * 4], &packed, 4);
        } else {
            const uint32_t bgra = (next() & 0x00FFFFFFu) | 0xFF000000u;
            std::memcpy(&bytes[i * 4], &bgra, 4);
        }
    }
    return bytes;
}

struct Converted {
    ComGuard<ID3D11Texture2D> planar;  // today's retained P010 slot
    ComGuard<ID3D11Texture2D> packed;  // the packed slot of the same frame
};

bool ConvertBoth(Gpu& gpu, RetainedYuvConverter& converter, const Case& c, Converted* out) {
    UINT pitch = 0;
    const std::vector<uint8_t> bytes = NoiseSource(c, &pitch);
    ComGuard<ID3D11Texture2D> rgb = MakeTexture(gpu.device.get(), c.width, c.height, c.rgbFormat,
                                                D3D11_BIND_SHADER_RESOURCE, bytes.data(), pitch);
    ComGuard<ID3D11ShaderResourceView> view;
    if (!rgb || FAILED(gpu.device->CreateShaderResourceView(rgb.get(), nullptr, view.addressof())))
        return false;
    out->planar = MakeTexture(gpu.device.get(), c.width, c.height, DXGI_FORMAT_P010, kYuvBind);
    out->packed = MakeTexture(gpu.device.get(), c.width, c.height, pp::kSlotFormat, kYuvBind);
    PlaneRtvs planes;
    ComGuard<ID3D11RenderTargetView> packedRtv;
    if (!out->planar || !out->packed || !MakeP010Rtvs(gpu.device.get(), out->planar.get(), &planes) ||
        FAILED(gpu.device->CreateRenderTargetView(out->packed.get(), nullptr, packedRtv.addressof())))
        return false;
    RetainedYuvConverter::Request request;
    request.source = view.get();
    request.width = c.width;
    request.height = c.height;
    request.normalize = c.normalize;
    request.hdrOutput = c.hdr;
    request.bt2020 = c.bt2020;
    request.fullRange = c.fullRange;
    RetainedYuvConverter::Request planarRequest = request;
    planarRequest.lumaTarget = planes.luma.get();
    planarRequest.chromaTarget = planes.chroma.get();
    RetainedYuvConverter::Request packedRequest = request;
    packedRequest.packedTarget = packedRtv.get();
    return converter.Convert(gpu.context.get(), planarRequest) && converter.Convert(gpu.context.get(), packedRequest);
}

ComGuard<ID3D11Texture2D> Unpack(Gpu& gpu, PackedP010Unpacker& unpacker, ID3D11Texture2D* packed, UINT w, UINT h) {
    ComGuard<ID3D11Texture2D> output = MakeTexture(gpu.device.get(), w, h, DXGI_FORMAT_P010, kYuvBind);
    PlaneRtvs planes;
    ComGuard<ID3D11ShaderResourceView> packedView;
    if (!output || !MakeP010Rtvs(gpu.device.get(), output.get(), &planes) ||
        FAILED(gpu.device->CreateShaderResourceView(packed, nullptr, packedView.addressof())) ||
        !unpacker.Unpack(gpu.context.get(), packedView.get(), planes.luma.get(), planes.chroma.get(), w, h))
        return {};
    return output;
}

// Index of the first differing sample, or -1.
long long FirstMismatch(const std::vector<uint16_t>& a, const std::vector<uint16_t>& b) {
    if (a.size() != b.size())
        return 0;
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i] != b[i])
            return static_cast<long long>(i);
    }
    return -1;
}

void ExpectExactRoundTrip(const Case& c) {
    Gpu gpu;
    ASSERT_TRUE(gpu.Create());
    RetainedYuvConverter converter;
    PackedP010Unpacker unpacker;
    ASSERT_TRUE(converter.EnsureKernels(gpu.device.get()));
    ASSERT_TRUE(PackedP010Unpacker::PrepareShaders());
    ASSERT_TRUE(unpacker.Init(gpu.device.get()));
    Converted converted;
    ASSERT_TRUE(ConvertBoth(gpu, converter, c, &converted)) << c.name;
    ComGuard<ID3D11Texture2D> rebuilt = Unpack(gpu, unpacker, converted.packed.get(), c.width, c.height);
    ASSERT_TRUE(rebuilt) << c.name;

    const std::vector<uint16_t> reference = ReadSamples(gpu, converted.planar.get());
    const std::vector<uint16_t> unpacked = ReadSamples(gpu, rebuilt.get());
    const std::vector<uint16_t> texels = ReadSamples(gpu, converted.packed.get());
    ASSERT_EQ(reference.size(), static_cast<size_t>(c.width) * (c.height + c.height / 2)) << c.name;
    ASSERT_EQ(texels.size(), static_cast<size_t>(c.width) * c.height) << c.name;

    // 1. The rebuilt P010 is the planar route's P010, every bit of every sample.
    const long long mismatch = FirstMismatch(reference, unpacked);
    EXPECT_EQ(mismatch, -1) << c.name << ": sample " << mismatch << " reference="
                            << (mismatch >= 0 ? reference[static_cast<size_t>(mismatch)] : 0)
                            << " unpacked=" << (mismatch >= 0 ? unpacked[static_cast<size_t>(mismatch)] : 0);

    // 2. The slot itself follows the documented layout (HLSL == packed_p010_layout.h).
    const size_t chromaBase = static_cast<size_t>(c.width) * c.height;
    size_t layoutMismatches = 0;
    size_t lowBitsSet = 0;
    std::set<uint32_t> distinctChroma;
    for (UINT y = 0; y < c.height; ++y) {
        for (UINT x = 0; x < c.width; ++x) {
            const uint16_t luma = reference[static_cast<size_t>(y) * c.width + x];
            const size_t chroma = chromaBase + static_cast<size_t>(y / 2) * c.width + (x / 2) * 2;
            const uint32_t word = pp::BlockWord(reference[chroma] >> 6, reference[chroma + 1] >> 6);
            lowBitsSet += (luma & 0x3Fu) != 0;
            if ((x & 1u) == 0 && (y & 1u) == 0)
                distinctChroma.insert(word);
            const uint16_t expected = pp::PackTexel(luma >> 6, word, pp::PieceIndex(x, y));
            layoutMismatches += texels[static_cast<size_t>(y) * c.width + x] != expected;
        }
    }
    EXPECT_EQ(layoutMismatches, 0u) << c.name;
    EXPECT_EQ(lowBitsSet, 0u) << c.name << ": P010 codes must leave the low six bits clear";
    // The noise must actually exercise per-block chroma, or the test proves nothing.
    // Averaged noise concentrates near neutral, so large frames repeat chroma pairs.
    const size_t blocks = static_cast<size_t>(c.width / 2) * (c.height / 2);
    EXPECT_GT(distinctChroma.size(), std::min<size_t>(blocks / 4, 100000)) << c.name;
    std::printf("[PackedP010] %s %ux%u: samples=%zu exact=%d distinctChroma=%zu\n", c.name, c.width, c.height,
                reference.size(), mismatch == -1 ? 1 : 0, distinctChroma.size());
}

// A premultiplied FP32 overlay layer: opaque core, alpha ramp, transparent rest.
ComGuard<ID3D11ShaderResourceView> OverlayLayer(Gpu& gpu, UINT w, UINT h, float scale) {
    std::vector<float> texels(static_cast<size_t>(w) * h * 4);
    for (UINT y = 0; y < h; ++y) {
        for (UINT x = 0; x < w; ++x) {
            const float a = x < w / 2 ? 1.0f : static_cast<float>(w - x) / static_cast<float>(w / 2);
            float* t = &texels[(static_cast<size_t>(y) * w + x) * 4];
            t[0] = scale * 0.9f * a;
            t[1] = scale * (0.2f + 0.6f * static_cast<float>(y) / static_cast<float>(h)) * a;
            t[2] = scale * 0.3f * a;
            t[3] = a;
        }
    }
    ComGuard<ID3D11Texture2D> texture = MakeTexture(gpu.device.get(), w, h, DXGI_FORMAT_R32G32B32A32_FLOAT,
                                                    D3D11_BIND_SHADER_RESOURCE, texels.data(), w * 16);
    ComGuard<ID3D11ShaderResourceView> view;
    if (texture)
        gpu.device->CreateShaderResourceView(texture.get(), nullptr, view.addressof());
    return view;
}

// The cursor/camera patch reads its background from the retained slot: from a
// packed slot it must write exactly what it writes from the planar one.
void ExpectPatchIdentical(const Case& c) {
    Gpu gpu;
    ASSERT_TRUE(gpu.Create());
    RetainedYuvConverter converter;
    PackedP010Unpacker unpacker;
    YuvOverlayPatcher patcher;
    ASSERT_TRUE(converter.EnsureKernels(gpu.device.get()));
    ASSERT_TRUE(PackedP010Unpacker::PrepareShaders());
    ASSERT_TRUE(unpacker.Init(gpu.device.get()));
    ASSERT_TRUE(patcher.PrepareShaders());
    ASSERT_TRUE(patcher.Init(gpu.device.get(), gpu.context.get()));
    Converted converted;
    ASSERT_TRUE(ConvertBoth(gpu, converter, c, &converted)) << c.name;

    ComGuard<ID3D11Texture2D> fromPlanar = MakeTexture(gpu.device.get(), c.width, c.height, DXGI_FORMAT_P010,
                                                       kYuvBind);
    ASSERT_TRUE(fromPlanar);
    gpu.context->CopyResource(fromPlanar.get(), converted.planar.get());
    ComGuard<ID3D11Texture2D> fromPacked = Unpack(gpu, unpacker, converted.packed.get(), c.width, c.height);
    ASSERT_TRUE(fromPacked);

    const float scale = c.hdr ? 2.5f : 1.0f;
    const RECT cameraRect = {9, 7, 121, 95};
    const RECT cursorRect = {100, 80, 133, 117};  // overlaps the camera's ramp
    ComGuard<ID3D11ShaderResourceView> camera =
        OverlayLayer(gpu, cameraRect.right - cameraRect.left, cameraRect.bottom - cameraRect.top, scale);
    ComGuard<ID3D11ShaderResourceView> cursor =
        OverlayLayer(gpu, cursorRect.right - cursorRect.left, cursorRect.bottom - cursorRect.top, scale);
    YuvOverlayPatcher::Params params;
    params.hdr = c.hdr;
    params.bt2020 = c.bt2020;
    params.fullRange = c.fullRange;
    params.rgbLevels = 1023.0f;
    PlaneRtvs planarTargets;
    PlaneRtvs packedTargets;
    ASSERT_TRUE(MakeP010Rtvs(gpu.device.get(), fromPlanar.get(), &planarTargets));
    ASSERT_TRUE(MakeP010Rtvs(gpu.device.get(), fromPacked.get(), &packedTargets));
    ASSERT_TRUE(patcher.Apply(converted.planar.get(), planarTargets.luma.get(), planarTargets.chroma.get(), c.width,
                              c.height, {camera.get(), cameraRect}, {cursor.get(), cursorRect}, params));
    ASSERT_TRUE(patcher.Apply(converted.packed.get(), packedTargets.luma.get(), packedTargets.chroma.get(), c.width,
                              c.height, {camera.get(), cameraRect}, {cursor.get(), cursorRect}, params));

    const std::vector<uint16_t> expected = ReadSamples(gpu, fromPlanar.get());
    const std::vector<uint16_t> got = ReadSamples(gpu, fromPacked.get());
    const std::vector<uint16_t> background = ReadSamples(gpu, converted.planar.get());
    EXPECT_EQ(FirstMismatch(expected, got), -1) << c.name;
    // The overlays really changed the frame (the comparison is not of two copies).
    EXPECT_NE(FirstMismatch(expected, background), -1) << c.name;
}

}  // namespace

TEST(PackedP010GpuTest, OverlayPatchReadsAPackedBackgroundExactlyLikePlanar) {
    ExpectPatchIdentical(
        {"SDR full", DXGI_FORMAT_R10G10B10A2_UNORM, RgbColorTransform::kNone, false, false, true, 322, 182});
    ExpectPatchIdentical(
        {"HDR PQ", DXGI_FORMAT_R16G16B16A16_FLOAT, RgbColorTransform::kScRgbToHdr10, true, false, false, 322, 182});
}

TEST(PackedP010GpuTest, EverySdrSourceRebuildsThePlanarP010Exactly) {
    // 322x182: odd chroma width and height (161x91), so edge blocks are covered.
    const Case cases[] = {
        {"R10 BT.709 limited", DXGI_FORMAT_R10G10B10A2_UNORM, RgbColorTransform::kNone, false, false, false, 322, 182},
        {"R10 BT.709 full", DXGI_FORMAT_R10G10B10A2_UNORM, RgbColorTransform::kNone, false, false, true, 322, 182},
        {"R10 BT.2020 full", DXGI_FORMAT_R10G10B10A2_UNORM, RgbColorTransform::kNone, false, true, true, 322, 182},
        {"BGRA8 BT.709 limited", DXGI_FORMAT_B8G8R8A8_UNORM, RgbColorTransform::kNone, false, false, false, 322, 182},
        {"FP16 SDR linear-to-sRGB", DXGI_FORMAT_R16G16B16A16_FLOAT, RgbColorTransform::kLinearToSrgb, false, false,
         true, 322, 182},
    };
    for (const Case& c : cases)
        ExpectExactRoundTrip(c);
}

TEST(PackedP010GpuTest, HdrScRgbRebuildsThePlanarPqP010Exactly) {
    ExpectExactRoundTrip(
        {"FP16 HDR PQ limited", DXGI_FORMAT_R16G16B16A16_FLOAT, RgbColorTransform::kScRgbToHdr10, true, false, false,
         322, 182});
    ExpectExactRoundTrip(
        {"FP16 HDR PQ full", DXGI_FORMAT_R16G16B16A16_FLOAT, RgbColorTransform::kScRgbToHdr10, true, false, true, 322,
         182});
}

TEST(PackedP010GpuTest, A4kDesktopFrameRebuildsExactly) {
    // The dxgi_dup 4K 10-bit full-range recording this layout exists for.
    ExpectExactRoundTrip(
        {"4K R10 BT.709 full", DXGI_FORMAT_R10G10B10A2_UNORM, RgbColorTransform::kNone, false, false, true, 3840, 2160});
}
