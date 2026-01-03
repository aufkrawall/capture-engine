#include "dxgi_capture.h"
#include "../common/logging.h"
#include "../mediaengine/mediaengine.h"
#include <vector>
#include <mutex>

#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3d11.lib")

class DxgiCapture::Impl {
public:
    IDXGIOutputDuplication* dup_ = nullptr;
    ID3D11Texture2D* mappedTexture_ = nullptr;
    ID3D11Texture2D* gdiImage_ = nullptr; // For cursor
    
    // Config
    int monitorIdx_ = 0;
    
    // Cache
    DXGI_OUTDUPL_FRAME_INFO frameInfo_ = {};
    
    void Release() {
        if (dup_) {
            dup_->Release();
            dup_ = nullptr;
        }
        if (mappedTexture_) {
            mappedTexture_->Release();
            mappedTexture_ = nullptr;
        }
        if (gdiImage_) {
            gdiImage_->Release();
            gdiImage_ = nullptr;
        }
    }
    
    bool InitializeDup(ID3D11Device* device) {
        Release();
        
        IDXGIDevice* dxgiDevice = nullptr;
        HRESULT hr = device->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDevice);
        if (FAILED(hr)) return false;
        
        IDXGIAdapter* adapter = nullptr;
        hr = dxgiDevice->GetAdapter(&adapter);
        dxgiDevice->Release();
        if (FAILED(hr)) return false;
        
        IDXGIOutput* output = nullptr;
        hr = adapter->EnumOutputs(monitorIdx_, &output);
        adapter->Release();
        if (FAILED(hr)) {
             LogError("[DXGI] Failed to get output %d (hr=0x%x)", monitorIdx_, hr);
             return false;
        }
        
        IDXGIOutput1* output1 = nullptr;
        hr = output->QueryInterface(__uuidof(IDXGIOutput1), (void**)&output1);
        output->Release();
        
        if (SUCCEEDED(hr)) {
            hr = output1->DuplicateOutput(device, &dup_);
            output1->Release();
        }
        
        if (FAILED(hr)) {
             if (hr == DXGI_ERROR_NOT_CURRENTLY_AVAILABLE) {
                 LogError("[DXGI] Desktop Duplication not available (limit reached or fullscreen exclusive mode)");
             } else {
                 LogError("[DXGI] DuplicateOutput failed (hr=0x%x)", hr);
             }
             return false;
        }
        
        return true;
    }
};

DxgiCapture::DxgiCapture() : impl_(std::make_unique<Impl>()) {}

DxgiCapture::~DxgiCapture() {
    StopCapture();
}

bool DxgiCapture::Init(ID3D11Device* device, int monitorIndex, std::mutex* /*d3dMutex*/) {
    device_ = device;
    device_->GetImmediateContext(&context_);
    impl_->monitorIdx_ = monitorIndex;
    // NOTE: d3dMutex parameter is deprecated. We use MediaEngine_LockD3D11() instead.
    
    // Test initialization
    if (!impl_->InitializeDup(device_)) {
        return false;
    }
    
    // Get desc
    DXGI_OUTDUPL_DESC desc;
    impl_->dup_->GetDesc(&desc);
    width_ = desc.ModeDesc.Width;
    height_ = desc.ModeDesc.Height;
    
    // Release immediate dup to save resources until StartCapture
    impl_->Release();
    
    initialized_ = true;
    LogInfo("[DXGI] Initialized for monitor %d (%dx%d)", monitorIndex, width_, height_);
    return true;
}

bool DxgiCapture::StartCapture() {
    if (!initialized_) return false;
    
    if (!impl_->dup_) {
        if (!impl_->InitializeDup(device_)) return false;
    }
    
    capturing_ = true;
    ResetStats();
    LogInfo("[DXGI] Capture started");
    return true;
}

void DxgiCapture::StopCapture() {
    capturing_ = false;
    impl_->Release();
    LogInfo("[DXGI] Capture stopped");
}

void DxgiCapture::ResetStats() {
    droppedFrames_ = 0;
}

bool DxgiCapture::GetNextFrame(WGCCapturedFrame& frame, bool skipCopy) {
    if (!capturing_ || !impl_->dup_) return false;
    
    IDXGIResource* desktopResource = nullptr;
    // Use 20ms timeout to avoid busy loop and give DDAPI time to lock surface
    HRESULT hr = impl_->dup_->AcquireNextFrame(20, &impl_->frameInfo_, &desktopResource); 
    
    if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
        // Log verbose only? No, duplicate output wait timeout is normal if screen is static.
        // But if game is running, it should update.
        return false; 
    }
    
    if (FAILED(hr)) {
        if (hr == DXGI_ERROR_ACCESS_LOST) {
             LogInfo("[DXGI] Access lost (mode change?), re-initializing...");
             impl_->InitializeDup(device_); 
        } else {
             LogError("[DXGI] AcquireNextFrame failed: 0x%x", hr);
        }
        return false;
    }
    
    bool success = false;
    
     // Process frame if we got a desktop image resource
     if (desktopResource) { 
          ID3D11Texture2D* texture = nullptr;
          hr = desktopResource->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&texture);
          
          if (SUCCEEDED(hr)) {
             D3D11_TEXTURE2D_DESC desc;
             texture->GetDesc(&desc);
             
             // Ensure mapped texture
             if (!impl_->mappedTexture_ || desc.Width != width_ || desc.Height != height_) {
                 if (impl_->mappedTexture_) impl_->mappedTexture_->Release();
                 
                 desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
                 desc.MiscFlags = 0; 
                 desc.CPUAccessFlags = 0;
                 desc.Usage = D3D11_USAGE_DEFAULT;
                 hr = device_->CreateTexture2D(&desc, nullptr, &impl_->mappedTexture_);
                 if (FAILED(hr)) {
                     LogError("[DXGI] Failed to create mapped texture: 0x%x", hr);
                 }
             }
             
              if (impl_->mappedTexture_) {
                  // Only copy if not skipping (performance optimization)
                  if (!skipCopy) {
                      MediaEngine_LockD3D11();
                      context_->CopyResource(impl_->mappedTexture_, texture);
                      MediaEngine_UnlockD3D11();
                      
                      frame.texture = impl_->mappedTexture_;
                      frame.texture->AddRef();
                      frame.width = desc.Width;
                      frame.height = desc.Height;
                      success = true; // Frame successfully copied
                  } else {
                      // Dropping frame to save BW, but we successfully acquired it
                      success = false; 
                  }
                 
                  // Timestamp: Pass Raw QPC (MediaEngine converts to MS)
                  LARGE_INTEGER qpc;
                  
                  // Use driver timestamp if available, otherwise current QPC
                  int64_t ticks = impl_->frameInfo_.LastPresentTime.QuadPart;
                  if (ticks == 0) {
                      QueryPerformanceCounter(&qpc);
                      ticks = qpc.QuadPart;
                      LogDebug("[DXGI] LastPresentTime was 0, using QPC for timestamp.");
                  }
                  
                  frame.timestamp = ticks;
 
                  success = true;
              }
             texture->Release();
         } else {
             LogError("[DXGI] QueryInterface(ID3D11Texture2D) failed: 0x%x", hr);
         }
    } else {
        // Just release
    }
    
    desktopResource->Release();
    impl_->dup_->ReleaseFrame();
    
    return success;
}
