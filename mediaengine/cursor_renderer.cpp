#include "cursor_renderer.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <new>
#include "../common/raii_helpers.h"
#include "../common/secure_dll_loading.h"
#include "cursor_bitmap_utils.h"
#include "cursor_geometry.h"
#include "mediaengine.h"
#include "video_format_policy.h"

// Simple vertex/pixel shader for alpha-blended cursor overlay
// Compiled inline using D3DCompile at runtime
static const char* CURSOR_SHADER_SOURCE = R"(
// Constant buffer
cbuffer CursorCB : register(b0) {
    float4 cursorRect; // x, y, width, height (normalized 0-1)
    float4 colorParams; // mode, paper-white nits, unused, unused
};

// Textures
Texture2D cursorTex : register(t0);
SamplerState cursorSampler : register(s0);

// Vertex shader - fullscreen quad with cursor positioning
struct VS_OUTPUT {
    float4 pos : SV_POSITION;
    float2 uv : TEXCOORD0;
};

VS_OUTPUT VS_Main(uint vertexId : SV_VertexID) {
    VS_OUTPUT output;

    // Generate quad corners based on vertex ID
    float2 corners[4] = {
        float2(0, 0), // Top-left
        float2(1, 0), // Top-right
        float2(0, 1), // Bottom-left
        float2(1, 1)  // Bottom-right
    };

    float2 corner = corners[vertexId];

    // Convert normalized cursor rect to clip space (-1 to 1)
    float2 pos = cursorRect.xy + corner * cursorRect.zw;
    pos = pos * 2.0 - 1.0; // 0-1 to -1 to 1
    pos.y = -pos.y;        // Flip Y for D3D

    output.pos = float4(pos, 0, 1);
    output.uv = corner;

    return output;
}

float sRGBToLinear(float s) {
    return (s <= 0.04045) ? (s / 12.92) : pow((s + 0.055) / 1.055, 2.4);
}

float linearToPQ(float nits) {
    float lp = pow(max(nits, 0.0) / 10000.0, 0.1593017578125);
    return pow((0.8359375 + 18.8515625 * lp) / (1.0 + 18.6875 * lp), 78.84375);
}

float3 rec709ToRec2020(float3 color) {
    return float3(0.6274040 * color.r + 0.3292820 * color.g + 0.0433136 * color.b,
                  0.0690970 * color.r + 0.9195400 * color.g + 0.0113612 * color.b,
                  0.0163916 * color.r + 0.0880132 * color.g + 0.8955950 * color.b);
}

// Windows cursor resources are SDR sRGB. Convert their RGB values into the
// destination transfer function before the fixed-function alpha blend.
float4 PS_Main(VS_OUTPUT input) : SV_TARGET {
    float4 cursor = cursorTex.Sample(cursorSampler, input.uv);
    if (colorParams.x > 0.5) {
        float3 linearRgb = float3(sRGBToLinear(cursor.r), sRGBToLinear(cursor.g), sRGBToLinear(cursor.b));
        if (colorParams.x < 1.5) {
            cursor.rgb = linearRgb * (colorParams.y / 80.0);
        } else {
            float3 rec2020 = rec709ToRec2020(linearRgb);
            cursor.rgb = float3(linearToPQ(rec2020.r * colorParams.y), linearToPQ(rec2020.g * colorParams.y),
                                linearToPQ(rec2020.b * colorParams.y));
        }
    }
    return cursor;
}
)";

std::unique_ptr<uint8_t[]> ScaleBitmapNearestNeighbor(const uint8_t* src, uint32_t srcW, uint32_t srcH, uint32_t dstW,
                                                      uint32_t dstH) {
    if (!src || srcW == 0 || srcH == 0 || dstW == 0 || dstH == 0) {
        return nullptr;
    }
    if (srcW == dstW && srcH == dstH) {
        // No scaling needed - return a copy
        uint32_t size = dstW * dstH * 4;
        auto result = std::make_unique<uint8_t[]>(size);
        memcpy(result.get(), src, size);
        return result;
    }

    auto result = std::make_unique<uint8_t[]>(dstW * dstH * 4);
    uint8_t* dst = result.get();

    for (uint32_t dy = 0; dy < dstH; dy++) {
        uint32_t sy = dy * srcH / dstH;
        const uint8_t* srcRow = src + sy * srcW * 4;
        uint8_t* dstRow = dst + dy * dstW * 4;
        for (uint32_t dx = 0; dx < dstW; dx++) {
            uint32_t sx = dx * srcW / dstW;
            const uint8_t* srcPixel = srcRow + sx * 4;
            uint8_t* dstPixel = dstRow + dx * 4;
            dstPixel[0] = srcPixel[0];  // B
            dstPixel[1] = srcPixel[1];  // G
            dstPixel[2] = srcPixel[2];  // R
            dstPixel[3] = srcPixel[3];  // A
        }
    }
    return result;
}

CursorRenderer::CursorRenderer() = default;

CursorRenderer::~CursorRenderer() {
    Cleanup();
}

bool CursorRenderer::Init(ID3D11Device* dev, ID3D11DeviceContext* ctx) {
    // Already initialized - return success
    if (resourcesCreated) {
        return true;
    }

    if (!dev || !ctx) {
        return false;
    }

    device = dev;
    context = ctx;

    return CreateRenderingResources();
}

void CursorRenderer::Cleanup() {
    if (cursorTexture)
        cursorTexture->Release();
    if (cursorSRV)
        cursorSRV->Release();
    ClearTargetRenderViewCache();
    if (vertexShader)
        vertexShader->Release();
    if (pixelShader)
        pixelShader->Release();
    if (vertexBuffer)
        vertexBuffer->Release();
    if (constantBuffer)
        constantBuffer->Release();
    if (sampler)
        sampler->Release();
    if (blendState)
        blendState->Release();
    if (rasterizerState)
        rasterizerState->Release();

    cursorTexture = nullptr;
    cursorSRV = nullptr;
    vertexShader = nullptr;
    pixelShader = nullptr;
    vertexBuffer = nullptr;
    constantBuffer = nullptr;
    sampler = nullptr;
    blendState = nullptr;
    rasterizerState = nullptr;

    lastCursor = nullptr;
    lastRequestedWidth = 0;
    lastRequestedHeight = 0;
    resourcesCreated = false;
}

bool CursorRenderer::CreateRenderingResources() {
    HRESULT hr;

    DLL_Log("[CursorRenderer] CreateRenderingResources starting");

    // Load D3DCompile dynamically
    DLL_Log("[CursorRenderer] Loading d3dcompiler_47.dll");
    ce::ModuleGuard d3dCompiler(ce::security::LoadSystemLibrary(L"d3dcompiler_47.dll"));
    if (!d3dCompiler) {
        DLL_Log("[CursorRenderer] Failed to load d3dcompiler_47.dll");
        return false;
    }
    DLL_Log("[CursorRenderer] d3dcompiler_47.dll loaded OK");

    // D3DCompile returns blobs whose Release implementation lives in the compiler
    // module. Declare them after the module guard so they are destroyed first.
    ce::ComGuard<ID3DBlob> vsBlob;
    ce::ComGuard<ID3DBlob> psBlob;
    ce::ComGuard<ID3DBlob> errorBlob;

    typedef HRESULT(WINAPI * pD3DCompile)(LPCVOID pSrcData, SIZE_T SrcDataSize, LPCSTR pSourceName,
                                          const D3D_SHADER_MACRO* pDefines, ID3DInclude* pInclude, LPCSTR pEntrypoint,
                                          LPCSTR pTarget, UINT Flags1, UINT Flags2, ID3DBlob** ppCode,
                                          ID3DBlob** ppErrorMsgs);

    pD3DCompile d3dCompile = (pD3DCompile)GetProcAddress(d3dCompiler.get(), "D3DCompile");
    if (!d3dCompile) {
        DLL_Log("[CursorRenderer] Failed to get D3DCompile function");
        return false;
    }
    DLL_Log("[CursorRenderer] D3DCompile found");

    // Compile vertex shader
    DLL_Log("[CursorRenderer] Compiling vertex shader");
    hr = d3dCompile(CURSOR_SHADER_SOURCE, strlen(CURSOR_SHADER_SOURCE), nullptr, nullptr, nullptr, "VS_Main", "vs_4_0",
                    0, 0, vsBlob.put(), errorBlob.put());
    if (FAILED(hr)) {
        if (errorBlob) {
            DLL_Log("[CursorRenderer] VS compile error: %s", (char*)errorBlob->GetBufferPointer());
        }
        return false;
    }
    errorBlob.reset();
    DLL_Log("[CursorRenderer] VS compiled OK");

    // Compile pixel shader
    DLL_Log("[CursorRenderer] Compiling pixel shader");
    hr = d3dCompile(CURSOR_SHADER_SOURCE, strlen(CURSOR_SHADER_SOURCE), nullptr, nullptr, nullptr, "PS_Main", "ps_4_0",
                    0, 0, psBlob.put(), errorBlob.put());
    if (FAILED(hr)) {
        if (errorBlob) {
            DLL_Log("[CursorRenderer] PS compile error: %s", (char*)errorBlob->GetBufferPointer());
        }
        return false;
    }
    errorBlob.reset();
    DLL_Log("[CursorRenderer] PS compiled OK");

    // Create shaders
    DLL_Log("[CursorRenderer] Creating vertex shader, device=%p vsBlob=%p", (void*)device, (void*)vsBlob.get());
    if (!device || !vsBlob) {
        DLL_Log("[CursorRenderer] ERROR: device or vsBlob is null!");
        return false;
    }
    DLL_Log("[CursorRenderer] vsBlob size=%zu", vsBlob->GetBufferSize());
    hr = device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &vertexShader);
    if (FAILED(hr)) {
        DLL_Log("[CursorRenderer] Failed to create vertex shader");
        Cleanup();
        return false;
    }

    DLL_Log("[CursorRenderer] Creating pixel shader");
    hr = device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &pixelShader);
    if (FAILED(hr)) {
        DLL_Log("[CursorRenderer] Failed to create pixel shader");
        Cleanup();
        return false;
    }

    // Create constant buffer
    DLL_Log("[CursorRenderer] Creating constant buffer");
    D3D11_BUFFER_DESC cbDesc = {};
    cbDesc.ByteWidth = sizeof(CursorConstants);
    cbDesc.Usage = D3D11_USAGE_DYNAMIC;
    cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    hr = device->CreateBuffer(&cbDesc, nullptr, &constantBuffer);
    if (FAILED(hr)) {
        DLL_Log("[CursorRenderer] Failed to create constant buffer");
        Cleanup();
        return false;
    }

    // Create sampler
    DLL_Log("[CursorRenderer] Creating sampler");
    D3D11_SAMPLER_DESC sampDesc = {};
    sampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sampDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sampDesc.MaxLOD = D3D11_FLOAT32_MAX;

    hr = device->CreateSamplerState(&sampDesc, &sampler);
    if (FAILED(hr)) {
        DLL_Log("[CursorRenderer] Failed to create sampler");
        Cleanup();
        return false;
    }

    // Create blend state for alpha blending
    DLL_Log("[CursorRenderer] Creating blend state");
    D3D11_BLEND_DESC blendDesc = {};
    blendDesc.RenderTarget[0].BlendEnable = TRUE;
    blendDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    blendDesc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    blendDesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    blendDesc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    blendDesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    blendDesc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;

    hr = device->CreateBlendState(&blendDesc, &blendState);
    if (FAILED(hr)) {
        DLL_Log("[CursorRenderer] Failed to create blend state");
        Cleanup();
        return false;
    }

    // Create rasterizer state
    DLL_Log("[CursorRenderer] Creating rasterizer state");
    D3D11_RASTERIZER_DESC rastDesc = {};
    rastDesc.FillMode = D3D11_FILL_SOLID;
    rastDesc.CullMode = D3D11_CULL_NONE;

    hr = device->CreateRasterizerState(&rastDesc, &rasterizerState);
    if (FAILED(hr)) {
        DLL_Log("[CursorRenderer] Failed to create rasterizer state");
        Cleanup();
        return false;
    }

    resourcesCreated = true;
    DLL_Log("[CursorRenderer] D3D11 resources created successfully");
    return true;
}

bool CursorRenderer::ExtractCursorBitmap(HICON icon, uint8_t** outBitmap, uint32_t* outWidth, uint32_t* outHeight,
                                         bool* outIsMonochrome) {
    if (!outBitmap || !outWidth || !outHeight || !outIsMonochrome) {
        return false;
    }

    *outBitmap = nullptr;
    *outWidth = 0;
    *outHeight = 0;
    *outIsMonochrome = false;

    ICONINFO ii = {};
    if (!GetIconInfo(icon, &ii)) {
        return false;
    }

    *outIsMonochrome = ii.hbmColor == nullptr;
    BITMAP nativeBitmap = {};
    const HBITMAP dimensionBitmap = ii.hbmColor ? ii.hbmColor : ii.hbmMask;
    const bool dimensionsValid =
        dimensionBitmap && GetObject(dimensionBitmap, sizeof(nativeBitmap), &nativeBitmap) != 0;
    DeleteObject(ii.hbmColor);
    DeleteObject(ii.hbmMask);
    if (!dimensionsValid || nativeBitmap.bmWidth <= 0 || nativeBitmap.bmHeight == 0) {
        return false;
    }

    const uint32_t width = static_cast<uint32_t>(nativeBitmap.bmWidth);
    const uint32_t bitmapHeight = static_cast<uint32_t>(std::abs(nativeBitmap.bmHeight));
    const uint32_t height = *outIsMonochrome ? bitmapHeight / 2 : bitmapHeight;
    if (width == 0 || height == 0) {
        return false;
    }

    BITMAPINFO dibInfo = {};
    dibInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    dibInfo.bmiHeader.biWidth = static_cast<LONG>(width);
    dibInfo.bmiHeader.biHeight = -static_cast<LONG>(height);  // Top-down, matching D3D texture rows.
    dibInfo.bmiHeader.biPlanes = 1;
    dibInfo.bmiHeader.biBitCount = 32;
    dibInfo.bmiHeader.biCompression = BI_RGB;

    HDC dc = CreateCompatibleDC(nullptr);
    void* dibPixels = nullptr;
    HBITMAP dib = dc ? CreateDIBSection(dc, &dibInfo, DIB_RGB_COLORS, &dibPixels, nullptr, 0) : nullptr;
    HGDIOBJ previousBitmap = dib ? SelectObject(dc, dib) : nullptr;
    if (!dc || !dib || !dibPixels || !previousBitmap || previousBitmap == HGDI_ERROR) {
        if (dib)
            DeleteObject(dib);
        if (dc)
            DeleteDC(dc);
        return false;
    }

    const size_t pixelCount = static_cast<size_t>(width) * height;
    auto overBlack = std::make_unique<uint8_t[]>(pixelCount * 4);
    auto result = std::make_unique<uint8_t[]>(pixelCount * 4);
    auto drawOver = [&](uint32_t background) {
        std::fill_n(static_cast<uint32_t*>(dibPixels), pixelCount, background);
        if (DrawIconEx(dc, 0, 0, icon, static_cast<int>(width), static_cast<int>(height), 0, nullptr, DI_NORMAL) ==
            FALSE) {
            return false;
        }
        // DrawIconEx may be held in GDI's per-thread command batch. Flush before
        // reading the DIB section through its directly mapped CPU pointer.
        return GdiFlush() != FALSE;
    };

    bool rendered = drawOver(0xFF000000u);
    if (rendered) {
        memcpy(overBlack.get(), dibPixels, pixelCount * 4);
        rendered = drawOver(0xFFFFFFFFu);
    }
    if (rendered) {
        const auto* overWhite = static_cast<const uint8_t*>(dibPixels);
        for (size_t i = 0; i < pixelCount; ++i) {
            const ce::cursor_bitmap::Bgra8 black = {
                overBlack[i * 4 + 0],
                overBlack[i * 4 + 1],
                overBlack[i * 4 + 2],
                overBlack[i * 4 + 3],
            };
            const ce::cursor_bitmap::Bgra8 white = {
                overWhite[i * 4 + 0],
                overWhite[i * 4 + 1],
                overWhite[i * 4 + 2],
                overWhite[i * 4 + 3],
            };
            const auto straight = ce::cursor_bitmap::ReconstructStraightAlpha(black, white);
            result[i * 4 + 0] = straight.b;
            result[i * 4 + 1] = straight.g;
            result[i * 4 + 2] = straight.r;
            result[i * 4 + 3] = straight.a;
        }
    }

    SelectObject(dc, previousBitmap);
    DeleteObject(dib);
    DeleteDC(dc);
    if (!rendered) {
        return false;
    }

    *outBitmap = result.release();
    *outWidth = width;
    *outHeight = height;
    return true;
}

bool CursorRenderer::LoadCursorBitmap(HCURSOR cursor, uint32_t requestedWidth, uint32_t requestedHeight,
                                      CursorBitmapData* result) {
    if (!cursor || !result) {
        return false;
    }

    *result = {};
    HICON icon = CopyIcon(cursor);
    if (!icon) {
        return false;
    }

    auto readHotspot = [](HICON source, int32_t* x, int32_t* y) {
        ICONINFO info = {};
        if (!GetIconInfo(source, &info)) {
            return false;
        }
        *x = static_cast<int32_t>(info.xHotspot);
        *y = static_cast<int32_t>(info.yHotspot);
        DeleteObject(info.hbmColor);
        DeleteObject(info.hbmMask);
        return true;
    };

    uint8_t* rawBitmap = nullptr;
    if (!readHotspot(icon, &result->hotspotX, &result->hotspotY) ||
        !ExtractCursorBitmap(icon, &rawBitmap, &result->width, &result->height, &result->isMonochrome)) {
        DestroyIcon(icon);
        return false;
    }
    result->pixels.reset(rawBitmap);

    const uint32_t targetWidth = std::max(result->width, requestedWidth);
    const uint32_t targetHeight = std::max(result->height, requestedHeight);
    if (targetWidth != result->width || targetHeight != result->height) {
        HICON resourceSized = reinterpret_cast<HICON>(CopyImage(cursor, IMAGE_CURSOR, static_cast<int>(targetWidth),
                                                                static_cast<int>(targetHeight), LR_COPYFROMRESOURCE));
        if (resourceSized) {
            CursorBitmapData resource;
            uint8_t* resourcePixels = nullptr;
            if (readHotspot(resourceSized, &resource.hotspotX, &resource.hotspotY) &&
                ExtractCursorBitmap(resourceSized, &resourcePixels, &resource.width, &resource.height,
                                    &resource.isMonochrome)) {
                resource.pixels.reset(resourcePixels);
                if (resource.width >= result->width && resource.height >= result->height) {
                    *result = std::move(resource);
                }
            }
            DestroyIcon(resourceSized);
        }
    }

    const uint32_t finalWidth = std::max(result->width, targetWidth);
    const uint32_t finalHeight = std::max(result->height, targetHeight);
    if (finalWidth != result->width || finalHeight != result->height) {
        const uint32_t sourceWidth = result->width;
        const uint32_t sourceHeight = result->height;
        auto scaled =
            ScaleBitmapNearestNeighbor(result->pixels.get(), sourceWidth, sourceHeight, finalWidth, finalHeight);
        if (scaled) {
            result->hotspotX = static_cast<int32_t>(
                (static_cast<int64_t>(result->hotspotX) * finalWidth + sourceWidth / 2) / sourceWidth);
            result->hotspotY = static_cast<int32_t>(
                (static_cast<int64_t>(result->hotspotY) * finalHeight + sourceHeight / 2) / sourceHeight);
            result->pixels = std::move(scaled);
            result->width = finalWidth;
            result->height = finalHeight;
        }
    }
    DestroyIcon(icon);
    return result->pixels != nullptr && result->width != 0 && result->height != 0;
}

bool CursorRenderer::UpdateCursorTexture(const ce::cursor::CaptureState& state) {
    const HCURSOR cursor = reinterpret_cast<HCURSOR>(state.handle);
    if (!state.IsVisible() || !cursor) {
        return false;
    }

    if (cursor == lastCursor && state.requestedWidth == lastRequestedWidth &&
        state.requestedHeight == lastRequestedHeight && cursorTexture) {
        return true;
    }

    CursorBitmapData bitmap;
    if (!LoadCursorBitmap(cursor, state.requestedWidth, state.requestedHeight, &bitmap)) {
        return false;
    }
    cursorWidth = bitmap.width;
    cursorHeight = bitmap.height;
    hotspotX = bitmap.hotspotX;
    hotspotY = bitmap.hotspotY;
    isMonochrome = bitmap.isMonochrome;

    // Create or update cursor texture
    if (cursorTexture) {
        cursorTexture->Release();
        cursorTexture = nullptr;
    }
    if (cursorSRV) {
        cursorSRV->Release();
        cursorSRV = nullptr;
    }

    D3D11_TEXTURE2D_DESC texDesc = {};
    texDesc.Width = cursorWidth;
    texDesc.Height = cursorHeight;
    texDesc.MipLevels = 1;
    texDesc.ArraySize = 1;
    texDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    texDesc.SampleDesc.Count = 1;
    texDesc.Usage = D3D11_USAGE_DEFAULT;
    texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA initData = {};
    initData.pSysMem = bitmap.pixels.get();
    initData.SysMemPitch = cursorWidth * 4;

    HRESULT hr = device->CreateTexture2D(&texDesc, &initData, &cursorTexture);

    if (FAILED(hr)) {
        DLL_Log("[CursorRenderer] Failed to create cursor texture");
        return false;
    }

    hr = device->CreateShaderResourceView(cursorTexture, nullptr, &cursorSRV);
    if (FAILED(hr)) {
        DLL_Log("[CursorRenderer] Failed to create cursor SRV");
        cursorTexture->Release();
        cursorTexture = nullptr;
        return false;
    }

    lastCursor = cursor;
    lastRequestedWidth = state.requestedWidth;
    lastRequestedHeight = state.requestedHeight;
    return true;
}

bool CursorRenderer::GetCursorFrameRect(int frameWidth, int frameHeight, const ce::cursor::CaptureState& state,
                                        RECT* result) {
    if (!result || !resourcesCreated || !device || !context || frameWidth <= 0 || frameHeight <= 0 ||
        !state.IsVisible() || !UpdateCursorTexture(state) || !cursorTexture || !cursorSRV) {
        return false;
    }

    const int captureWidth = state.captureWidth != 0 ? static_cast<int>(state.captureWidth) : frameWidth;
    const int captureHeight = state.captureHeight != 0 ? static_cast<int>(state.captureHeight) : frameHeight;
    const int positionHotspotX =
        ce::cursor_geometry::ResolveHotspotForPosition(hotspotX, state.PositionIsShapeTopLeft());
    const int positionHotspotY =
        ce::cursor_geometry::ResolveHotspotForPosition(hotspotY, state.PositionIsShapeTopLeft());
    ce::cursor_geometry::Rect cursorRect;
    if (!ce::cursor_geometry::MapScreenCursorToFrame(state.screenX, state.screenY, positionHotspotX, positionHotspotY,
                                                     static_cast<int>(cursorWidth), static_cast<int>(cursorHeight),
                                                     state.captureLeft, state.captureTop, captureWidth, captureHeight,
                                                     frameWidth, frameHeight, &cursorRect)) {
        return false;
    }

    *result = {cursorRect.left, cursorRect.top, cursorRect.right, cursorRect.bottom};
    return true;
}

void CursorRenderer::ClearTargetRenderViewCache() {
    for (auto& entry : targetRtvCache) {
        if (entry.view) {
            entry.view->Release();
        }
        entry = {};
    }
    targetRtvUseCounter = 0;
    targetRtvWidth = 0;
    targetRtvHeight = 0;
    targetRtvArraySize = 0;
    targetRtvFormat = DXGI_FORMAT_UNKNOWN;
    targetRtvSampleDesc = {};
}

ID3D11RenderTargetView* CursorRenderer::GetTargetRenderView(ID3D11Texture2D* targetTexture,
                                                            const D3D11_TEXTURE2D_DESC& targetDesc) {
    const bool targetClassChanged = targetRtvWidth != targetDesc.Width || targetRtvHeight != targetDesc.Height ||
                                    targetRtvArraySize != targetDesc.ArraySize ||
                                    targetRtvFormat != targetDesc.Format ||
                                    targetRtvSampleDesc.Count != targetDesc.SampleDesc.Count ||
                                    targetRtvSampleDesc.Quality != targetDesc.SampleDesc.Quality;
    if (targetClassChanged) {
        ClearTargetRenderViewCache();
        targetRtvWidth = targetDesc.Width;
        targetRtvHeight = targetDesc.Height;
        targetRtvArraySize = targetDesc.ArraySize;
        targetRtvFormat = targetDesc.Format;
        targetRtvSampleDesc = targetDesc.SampleDesc;
    }

    ++targetRtvUseCounter;
    TargetRtvCacheEntry* replacement = nullptr;
    for (auto& entry : targetRtvCache) {
        if (entry.texture == targetTexture && entry.view) {
            entry.lastUsed = targetRtvUseCounter;
            return entry.view;
        }
        if (!replacement || !entry.view || entry.lastUsed < replacement->lastUsed) {
            replacement = &entry;
            if (!entry.view) {
                break;
            }
        }
    }

    D3D11_RENDER_TARGET_VIEW_DESC rtvDesc = {};
    rtvDesc.Format = ce::video_format::GetRgbShaderResourceViewFormat(targetDesc.Format);
    if (rtvDesc.Format == DXGI_FORMAT_UNKNOWN) {
        return nullptr;
    }
    if (targetDesc.ArraySize > 1) {
        if (targetDesc.SampleDesc.Count > 1) {
            rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DMSARRAY;
            rtvDesc.Texture2DMSArray.ArraySize = 1;
        } else {
            rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
            rtvDesc.Texture2DArray.MipSlice = 0;
            rtvDesc.Texture2DArray.ArraySize = 1;
        }
    } else if (targetDesc.SampleDesc.Count > 1) {
        rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DMS;
    } else {
        rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        rtvDesc.Texture2D.MipSlice = 0;
    }

    ID3D11RenderTargetView* view = nullptr;
    if (FAILED(device->CreateRenderTargetView(targetTexture, &rtvDesc, &view))) {
        return nullptr;
    }
    if (replacement->view) {
        replacement->view->Release();
    }
    replacement->texture = targetTexture;
    replacement->view = view;
    replacement->lastUsed = targetRtvUseCounter;
    return view;
}

bool CursorRenderer::CompositeOntoFrame(ID3D11Texture2D* targetTexture, int frameWidth, int frameHeight,
                                        const ce::cursor::CaptureState& state, CursorColorMode colorMode,
                                        float paperWhiteNits) {
    if (!targetTexture || !resourcesCreated || !device || !context) {
        return false;
    }

    RECT cursorRect = {};
    if (!GetCursorFrameRect(frameWidth, frameHeight, state, &cursorRect)) {
        return false;
    }

    D3D11_TEXTURE2D_DESC targetDesc = {};
    targetTexture->GetDesc(&targetDesc);
    ID3D11RenderTargetView* targetRTV = GetTargetRenderView(targetTexture, targetDesc);
    if (!targetRTV) {
        return false;
    }

    const float cursorX = static_cast<float>(cursorRect.left) / static_cast<float>(frameWidth);
    const float cursorY = static_cast<float>(cursorRect.top) / static_cast<float>(frameHeight);
    const float cursorW = static_cast<float>(cursorRect.right - cursorRect.left) / static_cast<float>(frameWidth);
    const float cursorH = static_cast<float>(cursorRect.bottom - cursorRect.top) / static_cast<float>(frameHeight);

    // Update constant buffer
    D3D11_MAPPED_SUBRESOURCE mapped;
    HRESULT hr = context->Map(constantBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (FAILED(hr)) {
        return false;
    }
    CursorConstants* cb = (CursorConstants*)mapped.pData;
    cb->cursorX = cursorX;
    cb->cursorY = cursorY;
    cb->cursorWidth = cursorW;
    cb->cursorHeight = cursorH;
    cb->colorMode = static_cast<float>(colorMode);
    cb->paperWhiteNits = paperWhiteNits;
    cb->padding0 = 0.0f;
    cb->padding1 = 0.0f;
    context->Unmap(constantBuffer, 0);

    // Save current state
    ID3D11RenderTargetView* oldRTV = nullptr;
    ID3D11DepthStencilView* oldDSV = nullptr;
    D3D11_VIEWPORT oldVP = {};
    UINT numVPs = 1;
    context->OMGetRenderTargets(1, &oldRTV, &oldDSV);
    context->RSGetViewports(&numVPs, &oldVP);

    // Set up viewport
    D3D11_VIEWPORT vp = {};
    vp.Width = (float)frameWidth;
    vp.Height = (float)frameHeight;
    vp.MaxDepth = 1.0f;

    // Set render state
    context->OMSetRenderTargets(1, &targetRTV, nullptr);
    context->RSSetViewports(1, &vp);
    context->RSSetState(rasterizerState);

    float blendFactor[4] = {0, 0, 0, 0};
    context->OMSetBlendState(blendState, blendFactor, 0xFFFFFFFF);

    // Set shaders and resources
    context->VSSetShader(vertexShader, nullptr, 0);
    context->PSSetShader(pixelShader, nullptr, 0);
    context->VSSetConstantBuffers(0, 1, &constantBuffer);
    context->PSSetShaderResources(0, 1, &cursorSRV);
    context->PSSetSamplers(0, 1, &sampler);

    // Draw cursor quad (4 vertices for triangle strip)
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    context->Draw(4, 0);

    ID3D11ShaderResourceView* nullSRV = nullptr;
    context->PSSetShaderResources(0, 1, &nullSRV);

    // Restore state
    context->OMSetRenderTargets(1, &oldRTV, oldDSV);
    context->RSSetViewports(numVPs ? 1 : 0, numVPs ? &oldVP : nullptr);

    if (oldRTV)
        oldRTV->Release();
    if (oldDSV)
        oldDSV->Release();

    return true;
}
