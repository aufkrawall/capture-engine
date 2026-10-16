#pragma once

#include <d3d11.h>
#include <windows.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>
#include "common/config/face_camera_config.h"
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
    // Content time of the output slot being composed (scheduled - content
    // delay); the camera frame taken at or before it is shown. 0 = newest.
    void SetContentTargetQpc(int64_t qpc) { contentTargetQpc_ = qpc; }
    // viewportOrigin: frame pixel at the target's texel (0,0), for drawing the
    // camera alone into a small layer texture (retained-YUV overlay patch).
    bool CompositePreparedFrame(ID3D11Texture2D* targetTexture, uint32_t targetWidth, uint32_t targetHeight,
                                FaceCameraColorMode colorMode, float paperWhiteNits, int viewportOriginX = 0,
                                int viewportOriginY = 0);

private:
    bool CreateRenderingResources();
    struct CameraSlot {
        ID3D11Texture2D* texture = nullptr;
        ID3D11ShaderResourceView* srv = nullptr;
        uint32_t width = 0;
        uint32_t height = 0;
        DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
        uint32_t mipLevels = 0;
        uint64_t sequence = 0;
        int64_t captureQpc = 0;
        void Release();
    };
    bool UpdateCameraTexture();
    void UploadLatestFrame();
    void ReleaseHistory();
    bool EnsureSlotTexture(CameraSlot& slot, uint32_t width, uint32_t height, DXGI_FORMAT format);
    bool CopyGpuFrame(const FaceCameraFrame& frame, CameraSlot& slot);
    ID3D11RenderTargetView* GetTargetRenderView(ID3D11Texture2D* targetTexture,
                                                const D3D11_TEXTURE2D_DESC& targetDesc);
    void ClearTargetRenderViewCache();
    void CleanupRendererResources();

    ce::face_camera::Config config_;
    std::unique_ptr<FaceCameraCapture> capture_;
    ID3D11Device* device_ = nullptr;
    ID3D11DeviceContext* context_ = nullptr;
    // Camera frames by capture time (face_camera_history.h); cameraTexture_ and
    // cameraSrv_ point at the slot selected for the current output slot.
    std::vector<CameraSlot> history_;
    size_t historyNext_ = 0;
    size_t loggedHistoryFrames_ = 0;
    bool historyAllocationLogged_ = false;
    int64_t contentTargetQpc_ = 0;
    int64_t lastCaptureQpc_ = 0;
    int64_t cameraIntervalQpc_ = 0;
    int64_t qpcFrequency_ = 0;
    ce::face_camera::LagSummary lagSummary_;
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
