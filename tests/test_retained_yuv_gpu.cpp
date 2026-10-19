#include <gtest/gtest.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11_4.h>
#include <mmsystem.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "common/platform/raii_helpers.h"
#include "mediaengine/video/retained_yuv_converter.h"
#include "mediaengine/video/yuv_overlay_patch.h"

// Retained-YUV reservoir on the real adapter (llm-wiki/wgc-reservoir-vram-design.md).
// Reference = today's encoder path: overlays blended into the RGB surface, then
// the encoder's RGB -> NV12/P010 conversion. Candidate = the RGB surface
// converted at ingress, copied, and the overlays patched in YUV.

namespace {

using ce::ComGuard;
using ce::video_format::RgbColorTransform;

struct Gpu {
    ComGuard<ID3D11Device> device;
    ComGuard<ID3D11DeviceContext> context;
    bool Create() {
        return SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                           nullptr, 0, D3D11_SDK_VERSION, device.addressof(), nullptr,
                                           context.addressof()));
    }
};

uint16_t FloatToHalf(float value) {
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    const uint32_t sign = (bits >> 16) & 0x8000u;
    const int32_t exponent = static_cast<int32_t>((bits >> 23) & 0xffu) - 127 + 15;
    uint32_t mantissa = bits & 0x7fffffu;
    if (exponent <= 0) {
        if (exponent < -10)
            return static_cast<uint16_t>(sign);
        mantissa |= 0x800000u;
        const uint32_t shift = static_cast<uint32_t>(14 - exponent);
        uint32_t half = mantissa >> shift;
        const uint32_t rest = mantissa & ((1u << shift) - 1u);
        const uint32_t halfway = 1u << (shift - 1u);
        if (rest > halfway || (rest == halfway && (half & 1u)))
            ++half;
        return static_cast<uint16_t>(sign | half);
    }
    if (exponent >= 31)
        return static_cast<uint16_t>(sign | 0x7c00u);
    uint32_t half = (static_cast<uint32_t>(exponent) << 10) | (mantissa >> 13);
    const uint32_t rest = mantissa & 0x1fffu;
    if (rest > 0x1000u || (rest == 0x1000u && (half & 1u)))
        ++half;
    return static_cast<uint16_t>(sign | half);
}

float HalfToFloat(uint16_t half) {
    const uint32_t sign = (half & 0x8000u) << 16;
    const uint32_t exponent = (half >> 10) & 0x1fu;
    const uint32_t mantissa = half & 0x3ffu;
    if (exponent == 0) {
        const float value = std::ldexp(static_cast<float>(mantissa), -24);
        return sign ? -value : value;
    }
    const uint32_t bits = sign | ((exponent + 112u) << 23) | (mantissa << 13);
    float out = 0.0f;
    std::memcpy(&out, &bits, sizeof(out));
    return out;
}

struct Rgba {
    float r, g, b, a;
};

// Overlay blend of the RGB surface: UNORM `levels` for SDR, FP16 for scRGB.
Rgba Blend(const Rgba& layer, const Rgba& below, float levels, bool hdr) {
    auto q = [&](float value) {
        if (hdr)
            return HalfToFloat(FloatToHalf(value));
        return std::floor(std::clamp(value, 0.0f, 1.0f) * levels + 0.5f) / levels;
    };
    return {q(layer.r + (1.0f - layer.a) * below.r), q(layer.g + (1.0f - layer.a) * below.g),
            q(layer.b + (1.0f - layer.a) * below.b), 1.0f};
}

struct Layer {
    RECT rect = {};
    std::vector<Rgba> texels;  // premultiplied, rect-sized
    Rgba At(int x, int y) const {
        if (x < rect.left || y < rect.top || x >= rect.right || y >= rect.bottom)
            return {0, 0, 0, 0};
        return texels[(y - rect.top) * (rect.right - rect.left) + (x - rect.left)];
    }
};

// A face-camera-like layer: opaque interior, antialiased ramp, transparent
// corners; `opacity` scales coverage. Colors are straight in the blend domain.
Layer MakeCameraLayer(RECT rect, float opacity, float scale) {
    Layer layer;
    layer.rect = rect;
    const int w = rect.right - rect.left;
    const int h = rect.bottom - rect.top;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const int edge = std::min({x, y, w - 1 - x, h - 1 - y});
            float coverage = edge >= 3 ? 1.0f : 0.25f * static_cast<float>(edge + 1);
            if ((x < 2 || x >= w - 2) && (y < 2 || y >= h - 2))
                coverage = 0.0f;
            const float a = coverage * opacity;
            const float u = static_cast<float>(x) / static_cast<float>(w);
            const float v = static_cast<float>(y) / static_cast<float>(h);
            const Rgba c = {scale * (0.15f + 0.7f * u), scale * (0.8f - 0.5f * v), scale * 0.35f, a};
            layer.texels.push_back({c.r * a, c.g * a, c.b * a, a});
        }
    }
    return layer;
}

// A cursor-like layer: white opaque arrow core, black outline, soft shadow,
// transparent rest.
Layer MakeCursorLayer(RECT rect, float scale) {
    Layer layer;
    layer.rect = rect;
    const int w = rect.right - rect.left;
    const int h = rect.bottom - rect.top;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            Rgba c = {0, 0, 0, 0};
            if (x <= y / 2 && y < h - 3) {
                c = (x == 0 || x == y / 2) ? Rgba{0, 0, 0, 1} : Rgba{scale, scale, scale, 1};
            } else if (x == y / 2 + 1 && y < h - 2) {
                c = {0, 0, 0, 0.5f};  // antialiased edge
            } else if (x <= y / 2 + 3 && y < h) {
                c = {0, 0, 0, 0.3f};  // shadow
            }
            layer.texels.push_back({c.r * c.a, c.g * c.a, c.b * c.a, c.a});
        }
    }
    return layer;
}

ComGuard<ID3D11Texture2D> MakeTexture(ID3D11Device* device, UINT w, UINT h, DXGI_FORMAT format, UINT bind,
                                      const void* data, UINT pitch) {
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

struct Planes {
    std::vector<int> luma;
    std::vector<int> chroma;  // Cb, Cr interleaved
};

Planes ReadYuv(Gpu& gpu, ID3D11Texture2D* texture) {
    D3D11_TEXTURE2D_DESC desc = {};
    texture->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComGuard<ID3D11Texture2D> staging;
    Planes out;
    if (FAILED(gpu.device->CreateTexture2D(&desc, nullptr, staging.addressof())))
        return out;
    gpu.context->CopyResource(staging.get(), texture);
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (FAILED(gpu.context->Map(staging.get(), 0, D3D11_MAP_READ, 0, &mapped)))
        return out;
    const bool eightBit = desc.Format == DXGI_FORMAT_NV12;
    const auto* bytes = static_cast<const uint8_t*>(mapped.pData);
    auto code = [&](size_t offset) {
        if (eightBit)
            return static_cast<int>(bytes[offset]);
        uint16_t value = 0;
        std::memcpy(&value, bytes + offset, sizeof(value));
        return static_cast<int>(value >> 6);
    };
    const size_t sample = eightBit ? 1 : 2;
    for (UINT y = 0; y < desc.Height; ++y)
        for (UINT x = 0; x < desc.Width; ++x)
            out.luma.push_back(code(y * mapped.RowPitch + x * sample));
    const size_t chromaBase = static_cast<size_t>(mapped.RowPitch) * desc.Height;
    for (UINT y = 0; y < desc.Height / 2; ++y)
        for (UINT x = 0; x < desc.Width; ++x)
            out.chroma.push_back(code(chromaBase + y * mapped.RowPitch + x * sample));
    gpu.context->Unmap(staging.get(), 0);
    return out;
}

struct PlaneRtvs {
    ComGuard<ID3D11RenderTargetView1> luma;
    ComGuard<ID3D11RenderTargetView1> chroma;
};

bool MakePlaneRtvs(ID3D11Device* device, ID3D11Texture2D* texture, PlaneRtvs* out) {
    ComGuard<ID3D11Device3> device3;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(device3.addressof()))))
        return false;
    D3D11_TEXTURE2D_DESC desc = {};
    texture->GetDesc(&desc);
    const bool eightBit = desc.Format == DXGI_FORMAT_NV12;
    auto view = [&](DXGI_FORMAT format, UINT plane, ComGuard<ID3D11RenderTargetView1>& rtv) {
        D3D11_RENDER_TARGET_VIEW_DESC1 rtvDesc = {};
        rtvDesc.Format = format;
        rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        rtvDesc.Texture2D.PlaneSlice = plane;
        return SUCCEEDED(device3->CreateRenderTargetView1(texture, &rtvDesc, rtv.addressof()));
    };
    return view(eightBit ? DXGI_FORMAT_R8_UNORM : DXGI_FORMAT_R16_UNORM, 0, out->luma) &&
           view(eightBit ? DXGI_FORMAT_R8G8_UNORM : DXGI_FORMAT_R16G16_UNORM, 1, out->chroma);
}

struct Scenario {
    DXGI_FORMAT rgbFormat = DXGI_FORMAT_R10G10B10A2_UNORM;  // R10, BGRA8 or FP16 (HDR)
    DXGI_FORMAT yuvFormat = DXGI_FORMAT_P010;
    bool hdr = false;
    bool bt2020 = false;
    bool fullRange = false;
    float cameraOpacity = 1.0f;
    bool chromaDetail = true;  // saturated stripes 3 px apart under the overlay edges (worst case)
    UINT width = 64;
    UINT height = 48;
};

struct Comparison {
    int outsideLumaMismatches = 0;
    int outsideChromaMismatches = 0;
    int opaqueLumaMaxDiff = 0;
    int partialLumaMaxDiff = 0;
    int coveredChromaMaxDiff = 0;  // footprint fully covered by opaque overlay pixels
    int partialChromaMaxDiff = 0;
    int touchedLuma = 0;
};

// RGB surface texel values (blend domain) for pixel (x, y).
Rgba Background(const Scenario& s, UINT x, UINT y) {
    const float fx = static_cast<float>(x) / static_cast<float>(s.width);
    const float fy = static_cast<float>(y) / static_cast<float>(s.height);
    Rgba c = {0.1f + 0.8f * fx, 0.2f + 0.6f * fy, 0.9f - 0.7f * fx * fy, 1.0f};
    if (s.chromaDetail && ((x / 3) + (y / 5)) % 4 == 0)  // high-chroma detail inside the overlay area
        c = {0.95f, 0.05f, 0.4f, 1.0f};
    if (s.hdr) {
        c.r *= 3.0f;
        c.g *= 3.0f;
        c.b *= 3.0f;
    }
    return c;
}

std::vector<uint8_t> PackRgb(const Scenario& s, const std::vector<Rgba>& pixels, UINT* pitch) {
    const size_t bpp = s.rgbFormat == DXGI_FORMAT_R16G16B16A16_FLOAT ? 8 : 4;
    *pitch = static_cast<UINT>(s.width * bpp);
    std::vector<uint8_t> bytes(pixels.size() * bpp);
    for (size_t i = 0; i < pixels.size(); ++i) {
        const Rgba& p = pixels[i];
        if (s.rgbFormat == DXGI_FORMAT_R16G16B16A16_FLOAT) {
            const uint16_t half[4] = {FloatToHalf(p.r), FloatToHalf(p.g), FloatToHalf(p.b), FloatToHalf(1.0f)};
            std::memcpy(&bytes[i * 8], half, 8);
        } else if (s.rgbFormat == DXGI_FORMAT_R10G10B10A2_UNORM) {
            auto c = [](float v) { return static_cast<uint32_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 1023.0f)); };
            const uint32_t packed = c(p.r) | (c(p.g) << 10) | (c(p.b) << 20) | (3u << 30);
            std::memcpy(&bytes[i * 4], &packed, 4);
        } else {  // B8G8R8A8
            auto c = [](float v) { return static_cast<uint8_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); };
            const uint8_t bgra[4] = {c(p.b), c(p.g), c(p.r), 255};
            std::memcpy(&bytes[i * 4], bgra, 4);
        }
    }
    return bytes;
}

// The surface exactly as stored (UNORM or FP16), so CPU blends start from it.
Rgba Stored(const Scenario& s, const Rgba& c) {
    if (s.rgbFormat == DXGI_FORMAT_R16G16B16A16_FLOAT)
        return {HalfToFloat(FloatToHalf(c.r)), HalfToFloat(FloatToHalf(c.g)), HalfToFloat(FloatToHalf(c.b)), 1.0f};
    const float levels = s.rgbFormat == DXGI_FORMAT_R10G10B10A2_UNORM ? 1023.0f : 255.0f;
    auto q = [&](float v) { return std::round(std::clamp(v, 0.0f, 1.0f) * levels) / levels; };
    return {q(c.r), q(c.g), q(c.b), 1.0f};
}

bool ConvertRgb(Gpu& gpu, RetainedYuvConverter& converter, const Scenario& s, const std::vector<Rgba>& pixels,
                ID3D11Texture2D* yuv) {
    UINT pitch = 0;
    const std::vector<uint8_t> bytes = PackRgb(s, pixels, &pitch);
    ComGuard<ID3D11Texture2D> rgb =
        MakeTexture(gpu.device.get(), s.width, s.height, s.rgbFormat, D3D11_BIND_SHADER_RESOURCE, bytes.data(), pitch);
    ComGuard<ID3D11ShaderResourceView> view;
    if (!rgb || FAILED(gpu.device->CreateShaderResourceView(rgb.get(), nullptr, view.addressof())))
        return false;
    PlaneRtvs rtvs;
    if (!MakePlaneRtvs(gpu.device.get(), yuv, &rtvs))
        return false;
    RetainedYuvConverter::Request request;
    request.source = view.get();
    request.width = s.width;
    request.height = s.height;
    request.lumaTarget = rtvs.luma.get();
    request.chromaTarget = rtvs.chroma.get();
    request.eightBit = s.yuvFormat == DXGI_FORMAT_NV12;
    request.normalize = s.hdr ? RgbColorTransform::kScRgbToHdr10 : RgbColorTransform::kNone;
    request.hdrOutput = s.hdr;
    request.bt2020 = s.bt2020;
    request.fullRange = s.fullRange;
    return converter.Convert(gpu.context.get(), request);
}

ComGuard<ID3D11ShaderResourceView> LayerView(Gpu& gpu, const Layer& layer) {
    const UINT w = static_cast<UINT>(layer.rect.right - layer.rect.left);
    const UINT h = static_cast<UINT>(layer.rect.bottom - layer.rect.top);
    ComGuard<ID3D11Texture2D> texture = MakeTexture(gpu.device.get(), w, h, DXGI_FORMAT_R32G32B32A32_FLOAT,
                                                    D3D11_BIND_SHADER_RESOURCE, layer.texels.data(), w * 16);
    ComGuard<ID3D11ShaderResourceView> view;
    if (texture)
        gpu.device->CreateShaderResourceView(texture.get(), nullptr, view.addressof());
    return view;
}

bool RunScenario(const Scenario& s, Comparison* out) {
    Gpu gpu;
    if (!gpu.Create())
        return false;
    RetainedYuvConverter converter;
    YuvOverlayPatcher patcher;
    if (!converter.EnsureKernels(gpu.device.get()) || !patcher.Init(gpu.device.get(), gpu.context.get()))
        return false;

    const float scale = s.hdr ? 2.5f : 1.0f;  // scRGB overlay at 200 nits
    const Layer camera = MakeCameraLayer({8, 6, 40, 30}, s.cameraOpacity, scale);
    const Layer cursor = MakeCursorLayer({30, 20, 46, 37}, scale);
    const float levels = s.rgbFormat == DXGI_FORMAT_B8G8R8A8_UNORM ? 255.0f : 1023.0f;

    std::vector<Rgba> base;
    std::vector<Rgba> composite;
    std::vector<int> coverage;  // 0 untouched, 1 partial, 2 opaque top layer
    for (UINT y = 0; y < s.height; ++y) {
        for (UINT x = 0; x < s.width; ++x) {
            const Rgba bg = Stored(s, Background(s, x, y));
            base.push_back(bg);
            const Rgba cam = camera.At(static_cast<int>(x), static_cast<int>(y));
            const Rgba cur = cursor.At(static_cast<int>(x), static_cast<int>(y));
            Rgba c = bg;
            if (cam.a > 0.0f)
                c = Blend(cam, c, levels, s.hdr);
            if (cur.a > 0.0f)
                c = Blend(cur, c, levels, s.hdr);
            composite.push_back(c);
            const bool opaque = cur.a >= 1.0f || (cur.a == 0.0f && cam.a >= 1.0f);
            coverage.push_back(cam.a == 0.0f && cur.a == 0.0f ? 0 : (opaque ? 2 : 1));
        }
    }

    const UINT yuvBind = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    ComGuard<ID3D11Texture2D> reference = MakeTexture(gpu.device.get(), s.width, s.height, s.yuvFormat, yuvBind,
                                                      nullptr, 0);
    ComGuard<ID3D11Texture2D> retained = MakeTexture(gpu.device.get(), s.width, s.height, s.yuvFormat, yuvBind,
                                                     nullptr, 0);
    ComGuard<ID3D11Texture2D> patched = MakeTexture(gpu.device.get(), s.width, s.height, s.yuvFormat, yuvBind,
                                                    nullptr, 0);
    if (!reference || !retained || !patched || !ConvertRgb(gpu, converter, s, composite, reference.get()) ||
        !ConvertRgb(gpu, converter, s, base, retained.get()))
        return false;
    gpu.context->CopyResource(patched.get(), retained.get());

    PlaneRtvs rtvs;
    if (!MakePlaneRtvs(gpu.device.get(), patched.get(), &rtvs))
        return false;
    ComGuard<ID3D11ShaderResourceView> cameraView = LayerView(gpu, camera);
    ComGuard<ID3D11ShaderResourceView> cursorView = LayerView(gpu, cursor);
    YuvOverlayPatcher::Params params;
    params.hdr = s.hdr;
    params.eightBit = s.yuvFormat == DXGI_FORMAT_NV12;
    params.bt2020 = s.bt2020;
    params.fullRange = s.fullRange;
    params.rgbLevels = levels;
    if (!patcher.Apply(retained.get(), rtvs.luma.get(), rtvs.chroma.get(), s.width, s.height,
                       {cameraView.get(), camera.rect}, {cursorView.get(), cursor.rect}, params))
        return false;

    const Planes ref = ReadYuv(gpu, reference.get());
    const Planes base0 = ReadYuv(gpu, retained.get());
    const Planes got = ReadYuv(gpu, patched.get());
    if (ref.luma.empty() || got.luma.empty())
        return false;
    for (UINT y = 0; y < s.height; ++y) {
        for (UINT x = 0; x < s.width; ++x) {
            const size_t i = y * s.width + x;
            const int diff = std::abs(got.luma[i] - ref.luma[i]);
            if (coverage[i] == 0) {
                out->outsideLumaMismatches += got.luma[i] != base0.luma[i] || got.luma[i] != ref.luma[i];
            } else {
                ++out->touchedLuma;
                int& slot = coverage[i] == 2 ? out->opaqueLumaMaxDiff : out->partialLumaMaxDiff;
                slot = std::max(slot, diff);
            }
        }
    }
    for (UINT cy = 0; cy < s.height / 2; ++cy) {
        for (UINT cx = 0; cx < s.width / 2; ++cx) {
            int touched = 0;
            int opaque = 0;
            int footprint = 0;
            for (int dy = s.hdr ? -1 : 0; dy <= 1; ++dy) {
                for (int dx = -1; dx <= 1; ++dx) {
                    const int px = std::clamp(static_cast<int>(cx * 2) + dx, 0, static_cast<int>(s.width) - 1);
                    const int py = std::clamp(static_cast<int>(cy * 2) + dy, 0, static_cast<int>(s.height) - 1);
                    const int cov = coverage[static_cast<size_t>(py) * s.width + px];
                    ++footprint;
                    touched += cov != 0;
                    opaque += cov == 2;
                }
            }
            for (int component = 0; component < 2; ++component) {
                const size_t i = (cy * s.width / 2 + cx) * 2 + component;
                const int diff = std::abs(got.chroma[i] - ref.chroma[i]);
                if (touched == 0) {
                    out->outsideChromaMismatches += got.chroma[i] != base0.chroma[i] || got.chroma[i] != ref.chroma[i];
                } else if (opaque == footprint) {
                    out->coveredChromaMaxDiff = std::max(out->coveredChromaMaxDiff, diff);
                } else {
                    out->partialChromaMaxDiff = std::max(out->partialChromaMaxDiff, diff);
                }
            }
        }
    }
    std::printf("[RetainedYuvGpu] touchedLuma=%d opaqueLuma<=%d partialLuma<=%d coveredChroma<=%d "
                "partialChroma<=%d outsideMismatch=%d/%d\n",
                out->touchedLuma, out->opaqueLumaMaxDiff, out->partialLumaMaxDiff, out->coveredChromaMaxDiff,
                out->partialChromaMaxDiff, out->outsideLumaMismatches, out->outsideChromaMismatches);
    return true;
}

// Untouched pixels and samples are bit-identical; fully covered ones differ
// only by float rounding of the UNORM surface (<= 1 code). Partially covered
// pixels depend on the background's lost 4:2:0 chroma detail, so their bound
// is measured: large only where a translucent edge crosses saturated chroma
// detail finer than the chroma grid.
void ExpectFaithful(const Comparison& c, int partialLumaBound, int partialChromaBound) {
    EXPECT_GT(c.touchedLuma, 100);
    EXPECT_EQ(c.outsideLumaMismatches, 0);
    EXPECT_EQ(c.outsideChromaMismatches, 0);
    EXPECT_LE(c.opaqueLumaMaxDiff, 1);
    EXPECT_LE(c.coveredChromaMaxDiff, 1);
    EXPECT_LE(c.partialLumaMaxDiff, partialLumaBound);
    EXPECT_LE(c.partialChromaMaxDiff, partialChromaBound);
}

}  // namespace

TEST(RetainedYuvGpuTest, SdrP010FromRgb10MatchesRgbCompositeThenConvert) {
    Comparison c;
    ASSERT_TRUE(RunScenario(Scenario{}, &c));
    ExpectFaithful(c, 2, 64);
}

TEST(RetainedYuvGpuTest, SdrP010OverSmoothBackgroundStaysWithinAFewCodes) {
    Scenario s;
    s.chromaDetail = false;
    Comparison c;
    ASSERT_TRUE(RunScenario(s, &c));
    ExpectFaithful(c, 1, 4);
}

TEST(RetainedYuvGpuTest, HdrP010OverSmoothBackgroundStaysWithinAFewCodes) {
    Scenario s;
    s.rgbFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
    s.hdr = true;
    s.chromaDetail = false;
    Comparison c;
    ASSERT_TRUE(RunScenario(s, &c));
    ExpectFaithful(c, 4, 4);
}

TEST(RetainedYuvGpuTest, SdrNv12FromBgra8FullRangeBt2020MatchesRgbCompositeThenConvert) {
    Scenario s;
    s.rgbFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
    s.yuvFormat = DXGI_FORMAT_NV12;
    s.bt2020 = true;
    s.fullRange = true;
    Comparison c;
    ASSERT_TRUE(RunScenario(s, &c));
    ExpectFaithful(c, 2, 20);
}

TEST(RetainedYuvGpuTest, TranslucentCameraKeepsUniformCoverageExact) {
    Scenario s;
    s.cameraOpacity = 0.6f;
    Comparison c;
    ASSERT_TRUE(RunScenario(s, &c));
    ExpectFaithful(c, 2, 64);
}

TEST(RetainedYuvGpuTest, HdrP010FromScRgbMatchesLinearBlendThenPq) {
    Scenario s;
    s.rgbFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
    s.yuvFormat = DXGI_FORMAT_P010;
    s.hdr = true;
    Comparison c;
    ASSERT_TRUE(RunScenario(s, &c));
    ExpectFaithful(c, 8, 16);
}

TEST(RetainedYuvGpuTest, ChromaFootprintCoversBothSitingsAndClamps) {
    const RECT footprint = YuvOverlayPatcher::ChromaFootprint({5, 4, 9, 10}, 64, 48);
    // Sample c reads columns 2c-1..2c+1 and rows 2c-1..2c+1 (HDR) / 2c..2c+1 (SDR).
    // The bound may include one sample too many (the shader discards it), never one too few.
    EXPECT_EQ(footprint.left, 2);    // sample 2 reads column 5
    EXPECT_EQ(footprint.top, 1);
    EXPECT_EQ(footprint.right, 5);   // sample 4 reads column 8, sample 5 starts at 9
    EXPECT_EQ(footprint.bottom, 6);  // HDR sample 5 reads row 9
    const RECT clamped = YuvOverlayPatcher::ChromaFootprint({-6, -4, 70, 60}, 64, 48);
    EXPECT_EQ(clamped.left, 0);
    EXPECT_EQ(clamped.top, 0);
    EXPECT_EQ(clamped.right, 32);
    EXPECT_EQ(clamped.bottom, 24);
}
