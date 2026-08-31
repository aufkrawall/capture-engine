#pragma once

#include <d3d11.h>
#include <windows.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include "../common/face_camera_config.h"
#include "face_camera_capture.h"

enum class FaceCameraColorMode : uint32_t {
    Sdr = 0,
    ScRgb = 1,
    Hdr10Pq = 2,
};

// Owns asynchronous camera ingest and the one-draw D3D11 composition path.
// Camera advancement is latest-frame-only; the encoder thread never waits.
class FaceCameraRenderer {
public:
    explicit FaceCameraRenderer(ce::face_camera::Config config);
    ~FaceCameraRenderer();

    bool StartCapture(ID3D11Device* device);
    bool InitRenderer(ID3D11Device* device, ID3D11DeviceContext* context);
    void Stop();

    bool GetOverlayFrameRect(uint32_t targetWidth, uint32_t targetHeight, uint32_t displayedWidth,
                             uint32_t displayedHeight, RECT* result);
    bool NeedsDynamicRepeatSource() const;
    bool CompositePreparedFrame(ID3D11Texture2D* targetTexture, uint32_t targetWidth, uint32_t targetHeight,
                                FaceCameraColorMode colorMode, float paperWhiteNits);

private:
    bool CreateRenderingResources();
    bool UpdateCameraTexture();
    bool EnsureCameraTexture(uint32_t width, uint32_t height, DXGI_FORMAT format);
    bool CopyGpuFrame(const FaceCameraFrame& frame);
    ID3D11RenderTargetView* GetTargetRenderView(ID3D11Texture2D* targetTexture,
                                                const D3D11_TEXTURE2D_DESC& targetDesc);
    void ClearTargetRenderViewCache();
    void CleanupRendererResources();

    ce::face_camera::Config config_;
    std::unique_ptr<FaceCameraCapture> capture_;
    ID3D11Device* device_ = nullptr;
    ID3D11DeviceContext* context_ = nullptr;
    ID3D11Texture2D* cameraTexture_ = nullptr;
    ID3D11ShaderResourceView* cameraSrv_ = nullptr;
    ID3D11VertexShader* vertexShader_ = nullptr;
    ID3D11PixelShader* pixelShader_ = nullptr;
    ID3D11Buffer* constantBuffer_ = nullptr;
    ID3D11SamplerState* sampler_ = nullptr;
    ID3D11BlendState* blendState_ = nullptr;
    ID3D11RasterizerState* rasterizerState_ = nullptr;
    uint32_t cameraWidth_ = 0;
    uint32_t cameraHeight_ = 0;
    DXGI_FORMAT cameraFormat_ = DXGI_FORMAT_UNKNOWN;
    uint64_t uploadedSequence_ = 0;
    uint64_t uploadedTickMs_ = 0;
    uint64_t captureStartTickMs_ = 0;
    uint64_t compositeCount_ = 0;
    uint64_t cameraUploadCount_ = 0;
    bool resourcesCreated_ = false;
    bool captureStarted_ = false;
    bool transportFailureLogged_ = false;
    bool staleLogged_ = false;
    bool colorMappingLogged_ = false;
    FaceCameraColorMode lastColorMode_ = FaceCameraColorMode::Sdr;
    DXGI_FORMAT lastTargetFormat_ = DXGI_FORMAT_UNKNOWN;
    float lastPaperWhiteNits_ = 0.0f;
    ce::face_camera::Layout preparedLayout_;

    static constexpr std::size_t kTargetRtvCacheSize = 64;
    struct TargetRtvCacheEntry {
        ID3D11Texture2D* texture = nullptr;
        ID3D11RenderTargetView* view = nullptr;
        uint64_t lastUsed = 0;
    };
    std::array<TargetRtvCacheEntry, kTargetRtvCacheSize> targetRtvCache_{};
    uint64_t targetRtvUseCounter_ = 0;
    uint32_t targetRtvWidth_ = 0;
    uint32_t targetRtvHeight_ = 0;
    uint32_t targetRtvArraySize_ = 0;
    DXGI_FORMAT targetRtvFormat_ = DXGI_FORMAT_UNKNOWN;
    DXGI_SAMPLE_DESC targetRtvSampleDesc_ = {};

    struct Constants {
        float destinationRect[4];
        float sourceRect[4];
        float shape[4];
        float color[4];
        float borderColor[4];
        float displayedSize[4];
    };
};
