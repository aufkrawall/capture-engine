/**
 * Custom Overlay - DX10 Backend Implementation
 *
 * Optimized version with:
 * - Shader caching to avoid redundant state changes
 * - Dynamic buffer resizing
 * - RAII with ComPtr for safety
 */

#include "custom_overlay_dx10.h"
#include <cstring>
#include "hook_common.h"
#include "overlay_shader_bytecode.h"

namespace CustomOverlay {

DX10Backend::DX10Backend(ID3D10Device* dev) : device(dev) {}

DX10Backend::~DX10Backend() {
    Shutdown();
}

bool DX10Backend::Initialize(int fontTextureWidth, int fontTextureHeight, const uint8_t* fontTextureData) {
    if (initialized || !device)
        return false;

    D3D10_TEXTURE2D_DESC texDesc = {};
    texDesc.Width = fontTextureWidth;
    texDesc.Height = fontTextureHeight;
    texDesc.MipLevels = 1;
    texDesc.ArraySize = 1;
    texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    texDesc.SampleDesc.Count = 1;
    texDesc.Usage = D3D10_USAGE_DEFAULT;
    texDesc.BindFlags = D3D10_BIND_SHADER_RESOURCE;

    D3D10_SUBRESOURCE_DATA initData = {};
    initData.pSysMem = fontTextureData;
    initData.SysMemPitch = fontTextureWidth * 4;

    HRESULT hr = device->CreateTexture2D(&texDesc, &initData, &fontTexture);
    if (FAILED(hr))
        return false;

    hr = device->CreateShaderResourceView(fontTexture.Get(), nullptr, &fontTextureView);
    if (FAILED(hr))
        return false;

    if (!CreateShaders())
        return false;
    if (!CreateBuffers())
        return false;
    if (!CreateStates())
        return false;

    initialized = true;
    return true;
}

void DX10Backend::Shutdown() {
    if (!initialized)
        return;

    fontTexture.Reset();
    fontTextureView.Reset();
    vertexBuffer.Reset();
    indexBuffer.Reset();
    constantBuffer.Reset();
    inputLayout.Reset();
    vertexShader.Reset();
    pixelShader.Reset();
    pixelShaderSolid.Reset();
    blendState.Reset();
    samplerState.Reset();
    rasterizerState.Reset();
    depthStencilState.Reset();

    device = nullptr;
    initialized = false;
}

bool DX10Backend::CreateShaders() {
    HRESULT hr = device->CreateVertexShader(g_VS_4_0, sizeof(g_VS_4_0), &vertexShader);
    if (FAILED(hr))
        return false;

    hr = device->CreatePixelShader(g_PS_Textured_4_0, sizeof(g_PS_Textured_4_0), &pixelShader);
    if (FAILED(hr))
        return false;

    hr = device->CreatePixelShader(g_PS_Solid_4_0, sizeof(g_PS_Solid_4_0), &pixelShaderSolid);
    if (FAILED(hr))
        return false;

    D3D10_INPUT_ELEMENT_DESC layout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D10_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D10_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 16, D3D10_INPUT_PER_VERTEX_DATA, 0},
    };

    hr = device->CreateInputLayout(layout, 3, g_VS_4_0, sizeof(g_VS_4_0), &inputLayout);
    if (FAILED(hr))
        return false;

    return true;
}

bool DX10Backend::CreateBuffers() {
    D3D10_BUFFER_DESC cbDesc = {};
    cbDesc.ByteWidth = 16;
    cbDesc.Usage = D3D10_USAGE_DYNAMIC;
    cbDesc.BindFlags = D3D10_BIND_CONSTANT_BUFFER;
    cbDesc.CPUAccessFlags = D3D10_CPU_ACCESS_WRITE;

    HRESULT hr = device->CreateBuffer(&cbDesc, nullptr, &constantBuffer);
    if (FAILED(hr))
        return false;

    vertexBufferSize = DX10_VERTEX_BUFFER_SIZE;
    D3D10_BUFFER_DESC vbDesc = {};
    vbDesc.ByteWidth = (UINT)vertexBufferSize;
    vbDesc.Usage = D3D10_USAGE_DYNAMIC;
    vbDesc.BindFlags = D3D10_BIND_VERTEX_BUFFER;
    vbDesc.CPUAccessFlags = D3D10_CPU_ACCESS_WRITE;

    hr = device->CreateBuffer(&vbDesc, nullptr, &vertexBuffer);
    if (FAILED(hr))
        return false;

    indexBufferSize = DX10_INDEX_BUFFER_SIZE;
    D3D10_BUFFER_DESC ibDesc = {};
    ibDesc.ByteWidth = (UINT)indexBufferSize;
    ibDesc.Usage = D3D10_USAGE_DYNAMIC;
    ibDesc.BindFlags = D3D10_BIND_INDEX_BUFFER;
    ibDesc.CPUAccessFlags = D3D10_CPU_ACCESS_WRITE;

    hr = device->CreateBuffer(&ibDesc, nullptr, &indexBuffer);
    if (FAILED(hr))
        return false;

    return true;
}

bool DX10Backend::CreateStates() {
    D3D10_SAMPLER_DESC sampDesc = {};
    sampDesc.Filter = D3D10_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    sampDesc.AddressU = D3D10_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressV = D3D10_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressW = D3D10_TEXTURE_ADDRESS_CLAMP;
    sampDesc.MaxLOD = D3D10_FLOAT32_MAX;

    HRESULT hr = device->CreateSamplerState(&sampDesc, &samplerState);
    if (FAILED(hr))
        return false;

    D3D10_BLEND_DESC blendDesc = {};
    blendDesc.BlendEnable[0] = TRUE;
    blendDesc.SrcBlend = D3D10_BLEND_SRC_ALPHA;
    blendDesc.DestBlend = D3D10_BLEND_INV_SRC_ALPHA;
    blendDesc.BlendOp = D3D10_BLEND_OP_ADD;
    blendDesc.SrcBlendAlpha = D3D10_BLEND_ONE;
    blendDesc.DestBlendAlpha = D3D10_BLEND_INV_SRC_ALPHA;
    blendDesc.BlendOpAlpha = D3D10_BLEND_OP_ADD;
    blendDesc.RenderTargetWriteMask[0] = D3D10_COLOR_WRITE_ENABLE_ALL;

    hr = device->CreateBlendState(&blendDesc, &blendState);
    if (FAILED(hr))
        return false;

    D3D10_RASTERIZER_DESC rasterDesc = {};
    rasterDesc.FillMode = D3D10_FILL_SOLID;
    rasterDesc.CullMode = D3D10_CULL_NONE;
    rasterDesc.ScissorEnable = FALSE;
    rasterDesc.DepthClipEnable = TRUE;

    hr = device->CreateRasterizerState(&rasterDesc, &rasterizerState);
    if (FAILED(hr))
        return false;

    D3D10_DEPTH_STENCIL_DESC depthDesc = {};
    depthDesc.DepthEnable = FALSE;
    depthDesc.DepthWriteMask = D3D10_DEPTH_WRITE_MASK_ALL;
    depthDesc.DepthFunc = D3D10_COMPARISON_ALWAYS;

    hr = device->CreateDepthStencilState(&depthDesc, &depthStencilState);
    if (FAILED(hr))
        return false;

    return true;
}

bool DX10Backend::ResizeVertexBuffer(size_t requiredBytes) {
    if (!device)
        return false;

    size_t newSize = vertexBufferSize * 2;
    while (newSize < requiredBytes)
        newSize *= 2;

    D3D10_BUFFER_DESC vbDesc = {};
    vbDesc.ByteWidth = (UINT)newSize;
    vbDesc.Usage = D3D10_USAGE_DYNAMIC;
    vbDesc.BindFlags = D3D10_BIND_VERTEX_BUFFER;
    vbDesc.CPUAccessFlags = D3D10_CPU_ACCESS_WRITE;

    ComPtr<ID3D10Buffer> newBuffer;
    HRESULT hr = device->CreateBuffer(&vbDesc, nullptr, &newBuffer);
    if (FAILED(hr))
        return false;

    vertexBuffer = newBuffer;
    vertexBufferSize = newSize;

    return true;
}

bool DX10Backend::ResizeIndexBuffer(size_t requiredBytes) {
    if (!device)
        return false;

    size_t newSize = indexBufferSize * 2;
    while (newSize < requiredBytes)
        newSize *= 2;

    D3D10_BUFFER_DESC ibDesc = {};
    ibDesc.ByteWidth = (UINT)newSize;
    ibDesc.Usage = D3D10_USAGE_DYNAMIC;
    ibDesc.BindFlags = D3D10_BIND_INDEX_BUFFER;
    ibDesc.CPUAccessFlags = D3D10_CPU_ACCESS_WRITE;

    ComPtr<ID3D10Buffer> newBuffer;
    HRESULT hr = device->CreateBuffer(&ibDesc, nullptr, &newBuffer);
    if (FAILED(hr))
        return false;

    indexBuffer = newBuffer;
    indexBufferSize = newSize;

    return true;
}

void DX10Backend::Render(const std::vector<DrawVertex>& vertices, const std::vector<uint16_t>& indices,
                         const std::vector<DrawCommand>& commands, int viewportWidth, int viewportHeight) {
    if (!initialized || !device || vertices.empty() || commands.empty())
        return;

    // Resize buffers if needed
    size_t vbSize = vertices.size() * sizeof(DrawVertex);
    if (vbSize > vertexBufferSize) {
        if (!ResizeVertexBuffer(vbSize))
            return;
    }

    size_t ibSize = indices.size() * sizeof(uint16_t);
    if (ibSize > indexBufferSize) {
        if (!ResizeIndexBuffer(ibSize))
            return;
    }

    // Update vertex buffer
    void* mapped = nullptr;
    HRESULT hr = vertexBuffer->Map(D3D10_MAP_WRITE_DISCARD, 0, &mapped);
    if (SUCCEEDED(hr)) {
        memcpy(mapped, vertices.data(), vbSize);
        vertexBuffer->Unmap();
    }

    // Update index buffer
    mapped = nullptr;
    hr = indexBuffer->Map(D3D10_MAP_WRITE_DISCARD, 0, &mapped);
    if (SUCCEEDED(hr)) {
        memcpy(mapped, indices.data(), ibSize);
        indexBuffer->Unmap();
    }

    // Update constant buffer with HDR params
    mapped = nullptr;
    hr = constantBuffer->Map(D3D10_MAP_WRITE_DISCARD, 0, &mapped);
    if (SUCCEEDED(hr)) {
        float* cb = (float*)mapped;
        cb[0] = (float)viewportWidth;
        cb[1] = (float)viewportHeight;
        cb[2] = (float)hdrMode;
        cb[3] = paperWhiteNits;
        constantBuffer->Unmap();
    }

    // Save full pipeline state
    ID3D10RasterizerState* oldRasterState = nullptr;
    ID3D10BlendState* oldBlendState = nullptr;
    ID3D10DepthStencilState* oldDepthState = nullptr;
    FLOAT oldBlendFactor[4];
    UINT oldSampleMask, oldStencilRef;
    device->RSGetState(&oldRasterState);
    device->OMGetBlendState(&oldBlendState, oldBlendFactor, &oldSampleMask);
    device->OMGetDepthStencilState(&oldDepthState, &oldStencilRef);

    // Save shaders
    ID3D10VertexShader* oldVS = nullptr;
    ID3D10PixelShader* oldPS = nullptr;
    device->VSGetShader(&oldVS);
    device->PSGetShader(&oldPS);

    // Save IA state
    ID3D10InputLayout* oldInputLayout = nullptr;
    D3D10_PRIMITIVE_TOPOLOGY oldTopology;
    ID3D10Buffer* oldVB = nullptr;
    UINT oldVBStride = 0, oldVBOffset = 0;
    ID3D10Buffer* oldIB = nullptr;
    DXGI_FORMAT oldIBFormat = DXGI_FORMAT_UNKNOWN;
    UINT oldIBOffset = 0;
    device->IAGetInputLayout(&oldInputLayout);
    device->IAGetPrimitiveTopology(&oldTopology);
    device->IAGetVertexBuffers(0, 1, &oldVB, &oldVBStride, &oldVBOffset);
    device->IAGetIndexBuffer(&oldIB, &oldIBFormat, &oldIBOffset);

    // Save VS constant buffer and PS resources
    ID3D10Buffer* oldVSCB = nullptr;
    ID3D10Buffer* oldPSCB = nullptr;
    ID3D10ShaderResourceView* oldPSSRV = nullptr;
    ID3D10SamplerState* oldPSSampler = nullptr;
    device->VSGetConstantBuffers(0, 1, &oldVSCB);
    device->PSGetConstantBuffers(0, 1, &oldPSCB);
    device->PSGetShaderResources(0, 1, &oldPSSRV);
    device->PSGetSamplers(0, 1, &oldPSSampler);

    // Save viewport
    UINT oldNumViewports = 1;
    D3D10_VIEWPORT oldViewport = {};
    device->RSGetViewports(&oldNumViewports, &oldViewport);

    // Set state
    float blendFactor[4] = {0, 0, 0, 0};
    device->RSSetState(rasterizerState.Get());
    device->OMSetBlendState(blendState.Get(), blendFactor, 0xFFFFFFFF);
    device->OMSetDepthStencilState(depthStencilState.Get(), 0);
    device->VSSetShader(vertexShader.Get());
    device->VSSetConstantBuffers(0, 1, constantBuffer.GetAddressOf());
    device->PSSetConstantBuffers(0, 1, constantBuffer.GetAddressOf());
    device->PSSetSamplers(0, 1, samplerState.GetAddressOf());
    device->PSSetShaderResources(0, 1, fontTextureView.GetAddressOf());
    device->IASetInputLayout(inputLayout.Get());

    UINT stride = sizeof(DrawVertex);
    UINT offset = 0;
    device->IASetVertexBuffers(0, 1, vertexBuffer.GetAddressOf(), &stride, &offset);
    device->IASetIndexBuffer(indexBuffer.Get(), DXGI_FORMAT_R16_UINT, 0);
    device->IASetPrimitiveTopology(D3D10_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    // Set viewport
    D3D10_VIEWPORT vp = {};
    vp.Width = viewportWidth;
    vp.Height = viewportHeight;
    vp.MaxDepth = 1.0f;
    device->RSSetViewports(1, &vp);

    // Draw with shader caching
    lastPixelShader = nullptr;
    for (const auto& cmd : commands) {
        ID3D10PixelShader* targetPS = cmd.useTexture ? pixelShader.Get() : pixelShaderSolid.Get();

        if (targetPS != lastPixelShader) {
            device->PSSetShader(targetPS);
            lastPixelShader = targetPS;
        }

        device->DrawIndexed(cmd.indexCount, cmd.indexOffset, 0);
    }

    // Restore full pipeline state and release raw pointers
    device->RSSetState(oldRasterState);
    device->OMSetBlendState(oldBlendState, oldBlendFactor, oldSampleMask);
    device->OMSetDepthStencilState(oldDepthState, oldStencilRef);
    device->VSSetShader(oldVS);
    device->PSSetShader(oldPS);
    device->IASetInputLayout(oldInputLayout);
    device->IASetPrimitiveTopology(oldTopology);
    device->IASetVertexBuffers(0, 1, &oldVB, &oldVBStride, &oldVBOffset);
    device->IASetIndexBuffer(oldIB, oldIBFormat, oldIBOffset);
    device->VSSetConstantBuffers(0, 1, &oldVSCB);
    device->PSSetConstantBuffers(0, 1, &oldPSCB);
    device->PSSetShaderResources(0, 1, &oldPSSRV);
    device->PSSetSamplers(0, 1, &oldPSSampler);
    if (oldNumViewports > 0) {
        device->RSSetViewports(oldNumViewports, &oldViewport);
    }

    // Release saved state references
    if (oldRasterState)
        oldRasterState->Release();
    if (oldBlendState)
        oldBlendState->Release();
    if (oldDepthState)
        oldDepthState->Release();
    if (oldVS)
        oldVS->Release();
    if (oldPS)
        oldPS->Release();
    if (oldInputLayout)
        oldInputLayout->Release();
    if (oldVB)
        oldVB->Release();
    if (oldIB)
        oldIB->Release();
    if (oldVSCB)
        oldVSCB->Release();
    if (oldPSCB)
        oldPSCB->Release();
    if (oldPSSRV)
        oldPSSRV->Release();
    if (oldPSSampler)
        oldPSSampler->Release();
}

}  // namespace CustomOverlay
