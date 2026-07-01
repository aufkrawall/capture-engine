#include "cursor_renderer.h"
#include <cstring>
#include <memory>
#include <new>
#include "../common/raii_helpers.h"
#include "mediaengine.h"

// Simple vertex/pixel shader for alpha-blended cursor overlay
// Compiled inline using D3DCompile at runtime
static const char* CURSOR_SHADER_SOURCE = R"(
// Constant buffer
cbuffer CursorCB : register(b0) {
    float4 cursorRect; // x, y, width, height (normalized 0-1)
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

// Pixel shader - sample cursor texture with alpha
float4 PS_Main(VS_OUTPUT input) : SV_TARGET {
    return cursorTex.Sample(cursorSampler, input.uv);
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
    if (targetRTV)
        targetRTV->Release();
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
    targetRTV = nullptr;
    vertexShader = nullptr;
    pixelShader = nullptr;
    vertexBuffer = nullptr;
    constantBuffer = nullptr;
    sampler = nullptr;
    blendState = nullptr;
    rasterizerState = nullptr;

    lastCursor = nullptr;
    resourcesCreated = false;
}

bool CursorRenderer::CreateRenderingResources() {
    HRESULT hr;

    DLL_Log("[CursorRenderer] CreateRenderingResources starting");

    // Compile shaders
    ce::ComGuard<ID3DBlob> vsBlob;
    ce::ComGuard<ID3DBlob> psBlob;
    ce::ComGuard<ID3DBlob> errorBlob;

    // Load D3DCompile dynamically
    DLL_Log("[CursorRenderer] Loading d3dcompiler_47.dll");
    HMODULE d3dCompiler = LoadLibraryW(L"d3dcompiler_47.dll");
    if (!d3dCompiler) {
        DLL_Log("[CursorRenderer] Failed to load d3dcompiler_47.dll");
        return false;
    }
    DLL_Log("[CursorRenderer] d3dcompiler_47.dll loaded OK");

    typedef HRESULT(WINAPI * pD3DCompile)(LPCVOID pSrcData, SIZE_T SrcDataSize, LPCSTR pSourceName,
                                          const D3D_SHADER_MACRO* pDefines, ID3DInclude* pInclude, LPCSTR pEntrypoint,
                                          LPCSTR pTarget, UINT Flags1, UINT Flags2, ID3DBlob** ppCode,
                                          ID3DBlob** ppErrorMsgs);

    pD3DCompile d3dCompile = (pD3DCompile)GetProcAddress(d3dCompiler, "D3DCompile");
    if (!d3dCompile) {
        DLL_Log("[CursorRenderer] Failed to get D3DCompile function");
        FreeLibrary(d3dCompiler);
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
        FreeLibrary(d3dCompiler);
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
        FreeLibrary(d3dCompiler);
        return false;
    }
    errorBlob.reset();
    DLL_Log("[CursorRenderer] PS compiled OK");

    FreeLibrary(d3dCompiler);

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

    ICONINFO ii;
    if (!GetIconInfo(icon, &ii)) {
        return false;
    }

    BITMAP bmpColor = {};
    BITMAP bmpMask = {};
    std::unique_ptr<uint8_t[]> colorData;
    std::unique_ptr<uint8_t[]> maskData;

    *outIsMonochrome = (ii.hbmColor == nullptr);

    if (ii.hbmColor) {
        if (GetObject(ii.hbmColor, sizeof(bmpColor), &bmpColor) == 0) {
            DeleteObject(ii.hbmColor);
            DeleteObject(ii.hbmMask);
            return false;
        }

        if (bmpColor.bmBitsPixel < 32) {
            // Non-32bpp not supported
            DeleteObject(ii.hbmColor);
            DeleteObject(ii.hbmMask);
            return false;
        }

        uint32_t size = bmpColor.bmHeight * bmpColor.bmWidthBytes;
        colorData.reset(new (std::nothrow) uint8_t[size]);
        if (!colorData) {
            DeleteObject(ii.hbmColor);
            DeleteObject(ii.hbmMask);
            return false;
        }
        if (GetBitmapBits(ii.hbmColor, size, colorData.get()) != static_cast<LONG>(size)) {
            DeleteObject(ii.hbmColor);
            DeleteObject(ii.hbmMask);
            return false;
        }

        *outWidth = bmpColor.bmWidth;
        *outHeight = bmpColor.bmHeight;

        // Check if we need to apply mask for alpha
        if (ii.hbmMask && GetObject(ii.hbmMask, sizeof(bmpMask), &bmpMask) != 0) {
            uint32_t maskSize = bmpMask.bmHeight * bmpMask.bmWidthBytes;
            maskData.reset(new (std::nothrow) uint8_t[maskSize]);
            if (!maskData) {
                DeleteObject(ii.hbmColor);
                DeleteObject(ii.hbmMask);
                return false;
            }
            if (GetBitmapBits(ii.hbmMask, maskSize, maskData.get()) != static_cast<LONG>(maskSize)) {
                DeleteObject(ii.hbmColor);
                DeleteObject(ii.hbmMask);
                return false;
            }

            // Check if color bitmap has alpha
            bool hasAlpha = false;
            uint32_t pixels = bmpColor.bmWidth * bmpColor.bmHeight;
            for (uint32_t i = 0; i < pixels && !hasAlpha; i++) {
                if (colorData[i * 4 + 3] != 0) {
                    hasAlpha = true;
                }
            }

            // Apply mask if no alpha in color bitmap
            if (!hasAlpha) {
                for (uint32_t y = 0; y < (uint32_t)bmpMask.bmHeight; y++) {
                    for (uint32_t x = 0; x < (uint32_t)bmpMask.bmWidth; x++) {
                        uint32_t maskBitOffset = y * (bmpMask.bmWidthBytes * 8) + x;
                        uint8_t maskByte = maskData[maskBitOffset / 8];
                        bool maskBit = (maskByte >> (7 - (maskBitOffset % 8))) & 1;

                        uint32_t pixelIdx = (y * bmpColor.bmWidth + x) * 4;
                        colorData[pixelIdx + 3] = maskBit ? 0 : 255;
                    }
                }
            }
        }

        *outBitmap = colorData.release();
    } else {
        // Monochrome cursor - not fully supported, create placeholder
        if (!GetObject(ii.hbmMask, sizeof(bmpMask), &bmpMask)) {
            DeleteObject(ii.hbmMask);
            return false;
        }

        // Monochrome mask is split: top half = AND mask, bottom half = XOR mask
        *outWidth = bmpMask.bmWidth;
        *outHeight = bmpMask.bmHeight / 2;
        uint32_t pixels = (*outWidth) * (*outHeight);
        std::unique_ptr<uint8_t[]> bitmap(new (std::nothrow) uint8_t[pixels * 4]);
        if (!bitmap) {
            DeleteObject(ii.hbmColor);
            DeleteObject(ii.hbmMask);
            return false;
        }

        uint32_t maskSize = bmpMask.bmHeight * bmpMask.bmWidthBytes;
        maskData.reset(new (std::nothrow) uint8_t[maskSize]);
        if (!maskData) {
            DeleteObject(ii.hbmColor);
            DeleteObject(ii.hbmMask);
            return false;
        }
        if (GetBitmapBits(ii.hbmMask, maskSize, maskData.get()) != static_cast<LONG>(maskSize)) {
            DeleteObject(ii.hbmColor);
            DeleteObject(ii.hbmMask);
            return false;
        }

        uint32_t bottomOffset = bmpMask.bmWidthBytes * (*outHeight);

        for (uint32_t i = 0; i < pixels; i++) {
            uint32_t byteOffset = i / 8;
            uint32_t bitOffset = 7 - (i % 8);

            uint8_t andMask = (maskData[byteOffset] >> bitOffset) & 1;
            uint8_t xorMask = (maskData[bottomOffset + byteOffset] >> bitOffset) & 1;

            uint32_t color;
            if (!andMask) {
                color = xorMask ? 0xFFFFFFFF : 0xFF000000;  // White or black
            } else {
                color = xorMask ? 0xFFFFFFFF : 0x00000000;  // Inverted or transparent
            }

            memcpy(bitmap.get() + i * 4, &color, 4);
        }
        *outBitmap = bitmap.release();
    }

    DeleteObject(ii.hbmColor);
    DeleteObject(ii.hbmMask);

    return true;
}

bool CursorRenderer::UpdateCursorTexture(bool allowHandleVisibilityFallback) {
    CURSORINFO ci = {sizeof(CURSORINFO)};
    if (!GetCursorInfo(&ci)) {
        return false;
    }

    // Check if cursor is hidden
    const bool cursorVisible = (ci.flags & CURSOR_SHOWING) || (allowHandleVisibilityFallback && ci.hCursor);
    if (!cursorVisible) {
        return false;
    }

    // Check if cursor shape changed
    if (ci.hCursor == lastCursor && cursorTexture) {
        return true;  // Same cursor, texture already valid
    }

    // New cursor shape - extract bitmap
    HICON icon = CopyIcon(ci.hCursor);
    if (!icon) {
        return false;
    }

    ICONINFO ii;
    if (!GetIconInfo(icon, &ii)) {
        DestroyIcon(icon);
        return false;
    }

    hotspotX = ii.xHotspot;
    hotspotY = ii.yHotspot;
    DeleteObject(ii.hbmColor);
    DeleteObject(ii.hbmMask);

    uint8_t* rawBitmap = nullptr;
    if (!ExtractCursorBitmap(icon, &rawBitmap, &cursorWidth, &cursorHeight, &isMonochrome)) {
        DestroyIcon(icon);
        return false;
    }
    std::unique_ptr<uint8_t[]> bitmap(rawBitmap);

    DestroyIcon(icon);

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
    initData.pSysMem = bitmap.get();
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

    lastCursor = ci.hCursor;
    return true;
}

bool CursorRenderer::CompositeOntoFrame(ID3D11Texture2D* targetTexture, int frameWidth, int frameHeight,
                                        int captureOriginX, int captureOriginY,
                                        bool allowHandleVisibilityFallback) {
    if (!resourcesCreated || !device || !context) {
        return false;
    }

    // Fast path: Check if cursor is visible (single API call)
    CURSORINFO ci = {sizeof(CURSORINFO)};
    if (!GetCursorInfo(&ci)) {
        return false;
    }

    const bool cursorVisible = (ci.flags & CURSOR_SHOWING) || (allowHandleVisibilityFallback && ci.hCursor);
    if (!cursorVisible) {
        return false;  // Cursor hidden - zero overhead path
    }

    // Update cursor texture if needed
    if (!UpdateCursorTexture(allowHandleVisibilityFallback)) {
        return false;
    }

    if (!cursorTexture || !cursorSRV) {
        return false;
    }

    // Create render target view for target texture
    if (targetRTV) {
        targetRTV->Release();
        targetRTV = nullptr;
    }

    HRESULT hr = device->CreateRenderTargetView(targetTexture, nullptr, &targetRTV);
    if (FAILED(hr)) {
        return false;
    }

    // The process is Per-Monitor V2 DPI-aware (via embedded manifest), so
    // GetCursorInfo() already returns physical screen coordinates.
    // captureOrigin is also in physical coords.  No DPI conversion needed.
    POINT cursorPos = ci.ptScreenPos;

    // Calculate normalized cursor position and size
    float cursorX = (float)((cursorPos.x - captureOriginX) - hotspotX) / (float)frameWidth;
    float cursorY = (float)((cursorPos.y - captureOriginY) - hotspotY) / (float)frameHeight;
    float cursorW = (float)cursorWidth / (float)frameWidth;
    float cursorH = (float)cursorHeight / (float)frameHeight;

    // Update constant buffer
    D3D11_MAPPED_SUBRESOURCE mapped;
    hr = context->Map(constantBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (SUCCEEDED(hr)) {
        CursorConstants* cb = (CursorConstants*)mapped.pData;
        cb->cursorX = cursorX;
        cb->cursorY = cursorY;
        cb->cursorWidth = cursorW;
        cb->cursorHeight = cursorH;
        context->Unmap(constantBuffer, 0);
    }

    // Save current state
    ID3D11RenderTargetView* oldRTV = nullptr;
    ID3D11DepthStencilView* oldDSV = nullptr;
    D3D11_VIEWPORT oldVP;
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

    // Restore state
    context->OMSetRenderTargets(1, &oldRTV, oldDSV);
    context->RSSetViewports(1, &oldVP);

    if (oldRTV)
        oldRTV->Release();
    if (oldDSV)
        oldDSV->Release();

    return true;
}
