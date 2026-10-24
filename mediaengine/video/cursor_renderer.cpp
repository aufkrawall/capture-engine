#include "cursor_renderer.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <new>
#include <mutex>
#include <vector>
#include "common/logging/log_meter.h"
#include "runtime_shader_compiler.h"
#include "cursor_geometry.h"
#include "mediaengine/engine/mediaengine.h"
#include "video_format_policy.h"

// Simple vertex/pixel shader for alpha-blended cursor overlay
// Bytecode prepared during renderer construction, before video frames.
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

namespace {

const std::array<std::vector<uint8_t>, 2>& CursorShaderBytecode() {
    static std::array<std::vector<uint8_t>, 2> bytecode;
    static std::once_flag compileOnce;
    std::call_once(compileOnce, [] {
        const ce::video_color::RuntimeShaderEntry entries[] = {{"VS_Main", "vs_4_0"}, {"PS_Main", "ps_4_0"}};
        const bool compiled = ce::video_color::CompileRuntimeShaders(
            CURSOR_SHADER_SOURCE, std::strlen(CURSOR_SHADER_SOURCE), entries, 2, "CursorRenderer",
            [](ID3DBlob* const* blobs) {
                for (size_t i = 0; i < bytecode.size(); ++i) {
                    const auto* bytes = static_cast<const uint8_t*>(blobs[i]->GetBufferPointer());
                    bytecode[i].assign(bytes, bytes + blobs[i]->GetBufferSize());
                }
                return true;
            });
        DLL_Log("[CursorRenderer] Shader preparation %s (configuration, before video frames)",
                compiled ? "ready" : "failed");
    });
    return bytecode;
}

}  // namespace

CursorRenderer::CursorRenderer() {
    (void)CursorShaderBytecode();
}

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
    lastLoggedColorMode = CursorColorMode::Sdr;
    lastLoggedPaperWhiteNits = -1.0f;
    lastLoggedTargetFormat = DXGI_FORMAT_UNKNOWN;
    resourcesCreated = false;
}

bool CursorRenderer::CreateRenderingResources() {
    HRESULT hr;

    DLL_Log("[CursorRenderer] CreateRenderingResources starting");

    const auto& bytecode = CursorShaderBytecode();
    if (bytecode[0].empty() || bytecode[1].empty())
        return false;
    hr = device->CreateVertexShader(bytecode[0].data(), bytecode[0].size(), nullptr, &vertexShader);
    if (FAILED(hr)) {
        DLL_Log("[CursorRenderer] Failed to create prepared vertex shader");
        Cleanup();
        return false;
    }
    hr = device->CreatePixelShader(bytecode[1].data(), bytecode[1].size(), nullptr, &pixelShader);
    if (FAILED(hr)) {
        DLL_Log("[CursorRenderer] Failed to create prepared pixel shader");
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
    // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization) - zero-initialized placeholder; enum fields are assigned before use
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
    // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization) - zero-initialized placeholder; enum fields are assigned before use
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
    // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization) - zero-initialized placeholder; enum fields are assigned before use
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

void CursorRenderer::LogTargetViewRefused(const D3D11_TEXTURE2D_DESC& targetDesc, HRESULT hr) {
    // Every frame of that target loses its cursor, so the line repeats with a count.
    const auto log = targetViewRefusedLog.Observe(
        ce::log_meter::FieldKey(static_cast<uint32_t>(targetDesc.Format), static_cast<uint32_t>(hr)), GetTickCount64());
    if (log) {
        DLL_Log("[CursorRenderer] No render target view for targetFmt=%d %ux%u HR=%x: cursor not drawn%s",
                static_cast<int>(targetDesc.Format), targetDesc.Width, targetDesc.Height, static_cast<unsigned>(hr),
                ce::log_meter::SuppressedNote(log.suppressed).c_str());
    }
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
    rtvDesc.Format = ce::video_format::GetOverlayRenderTargetViewFormat(targetDesc.Format);
    if (rtvDesc.Format == DXGI_FORMAT_UNKNOWN) {
        LogTargetViewRefused(targetDesc, S_OK);
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
    const HRESULT hr = device->CreateRenderTargetView(targetTexture, &rtvDesc, &view);
    if (FAILED(hr)) {
        LogTargetViewRefused(targetDesc, hr);
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
                                        float paperWhiteNits, int viewportOriginX, int viewportOriginY) {
    if (!targetTexture || !resourcesCreated || !device || !context) {
        return false;
    }

    RECT cursorRect = {};
    if (!GetCursorFrameRect(frameWidth, frameHeight, state, &cursorRect)) {
        return false;
    }

    D3D11_TEXTURE2D_DESC targetDesc = {};
    targetTexture->GetDesc(&targetDesc);
    const float resolvedPaperWhiteNits =
        std::clamp(std::isfinite(paperWhiteNits) ? paperWhiteNits : 203.0f, 80.0f, 1000.0f);
    if (lastLoggedColorMode != colorMode || lastLoggedTargetFormat != targetDesc.Format ||
        std::abs(lastLoggedPaperWhiteNits - resolvedPaperWhiteNits) >= 0.5f) {
        const char* mode = colorMode == CursorColorMode::ScRgb
                               ? "scRGB-linear-P709"
                               : (colorMode == CursorColorMode::Hdr10Pq ? "PQ-P2020" : "SDR-sRGB");
        DLL_Log(
            "[Cursor Color] SDR cursor mapping: targetFmt=%d target=%s paperWhite=%.1f-nit "
            "calibration=%s alpha=straight",
            targetDesc.Format, mode, resolvedPaperWhiteNits,
            colorMode == CursorColorMode::Sdr ? "not-applicable" : "windows-sdr-white");
        lastLoggedColorMode = colorMode;
        lastLoggedPaperWhiteNits = resolvedPaperWhiteNits;
        lastLoggedTargetFormat = targetDesc.Format;
    }
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
    cb->paperWhiteNits = resolvedPaperWhiteNits;
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
    vp.TopLeftX = -static_cast<float>(viewportOriginX);
    vp.TopLeftY = -static_cast<float>(viewportOriginY);
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
