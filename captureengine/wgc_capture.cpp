// Windows Graphics Capture (WGC) implementation
// Supports capturing DirectFlip content that Desktop Duplication cannot handle
// Requires Windows 10 1803+

#include "wgc_capture.h"
#include "../common/logging.h"
#include "mediaengine_loader.h"
#include <chrono>
#include <d3d11.h>
#include <dxgi.h>

// WinRT/C++WinRT headers for WGC
#include <winrt/base.h>

// Initialize apartment for WinRT
#include <roapi.h>

// Check if actual WGC headers are available
#if __has_include(<winrt/Windows.Graphics.Capture.h>)
#define HAS_WGC 1
#include <windows.graphics.capture.h>
#include <windows.graphics.capture.interop.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <winrt/Windows.Graphics.DirectX.h>

// Manual definition of IDirect3DDxgiInterfaceAccess since SDK header may be
// missing This interface allows extracting the DXGI interface from a WinRT
// Direct3D device/surface {A9B3D012-3DF2-4EE3-B8D1-8695F457D3C1}
struct IDirect3DDxgiInterfaceAccess : public IUnknown {
  virtual HRESULT STDMETHODCALLTYPE GetInterface(REFIID riid,
                                                 void **ppvObject) = 0;
};

// Explicit IID for IDirect3DDxgiInterfaceAccess
static const GUID IID_IDirect3DDxgiInterfaceAccess = {
    0xA9B3D012,
    0x3DF2,
    0x4EE3,
    {0xB8, 0xD1, 0x86, 0x95, 0xF4, 0x57, 0xD3, 0xC1}};

// CreateDirect3D11DeviceFromDXGIDevice declaration
extern "C" {
HRESULT WINAPI CreateDirect3D11DeviceFromDXGIDevice(
    IDXGIDevice *dxgiDevice, IInspectable **graphicsDevice);
}

#else
#define HAS_WGC 0
#endif

#if HAS_WGC
namespace winrt {
using namespace Windows::Foundation;
using namespace Windows::Graphics::Capture;
using namespace Windows::Graphics::DirectX;
using namespace Windows::Graphics::DirectX::Direct3D11;
} // namespace winrt
#endif

class WGCCapture::Impl {
public:
#if HAS_WGC
  winrt::GraphicsCaptureItem item_{nullptr};
  winrt::Direct3D11CaptureFramePool framePool_{nullptr};
  winrt::GraphicsCaptureSession session_{nullptr};
  winrt::IDirect3DDevice winrtDevice_{nullptr};

  ID3D11Texture2D *latestFrame_ = nullptr;
  ID3D11Texture2D *cachedTexture_ =
      nullptr; // Reuse texture to avoid allocation per frame
  int32_t cachedWidth_ = 0;
  int32_t cachedHeight_ = 0;
  std::mutex frameMutex_;
  int64_t lastFrameTime_ = 0;
  bool frameReady_ = false;
  uint32_t frameWidth_ = 0;
  uint32_t frameHeight_ = 0;
  int64_t frameTimestamp_ = 0;

  ID3D11Device *d3dDevice_ = nullptr;
  ID3D11DeviceContext *d3dContext_ = nullptr;

  winrt::event_token frameArrivedToken_;

  // Event signaled on frame arrival for efficient waiting
  HANDLE frameArrivedEvent_ = NULL;

  // QPC frequency cached for timestamp conversion
  int64_t qpcFreq_ = 0;

  // Callback function for direct frame processing (OBS-style)
  std::function<void(ID3D11Texture2D *, uint32_t, uint32_t, int64_t)>
      frameCallback_;
  std::atomic<uint32_t> callbackFrameCount_{0};

  void OnFrameArrived(winrt::Direct3D11CaptureFramePool const &sender,
                      winrt::IInspectable const &) {
    // OBS-STYLE: Process frame directly in callback for minimum latency
    // Get exactly ONE frame per callback (don't drain)
    auto winrtFrame = sender.TryGetNextFrame();
    if (!winrtFrame) {
      return;
    }

    auto surface = winrtFrame.Surface();
    IDirect3DDxgiInterfaceAccess *access = nullptr;

    if (SUCCEEDED(surface.as<IUnknown>()->QueryInterface(
            IID_IDirect3DDxgiInterfaceAccess, (void **)&access)) &&
        access) {

      ID3D11Texture2D *texture = nullptr;
      if (SUCCEEDED(access->GetInterface(__uuidof(ID3D11Texture2D),
                                         (void **)&texture)) &&
          texture) {

        D3D11_TEXTURE2D_DESC desc;
        texture->GetDesc(&desc);

        // Ensure cached texture exists and matches size
        if (!cachedTexture_ || cachedWidth_ != (int32_t)desc.Width ||
            cachedHeight_ != (int32_t)desc.Height) {
          if (cachedTexture_)
            cachedTexture_->Release();
          D3D11_TEXTURE2D_DESC copyDesc = desc;
          copyDesc.Usage = D3D11_USAGE_DEFAULT;
          copyDesc.BindFlags =
              D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
          copyDesc.CPUAccessFlags = 0;
          copyDesc.MiscFlags = 0;

          if (SUCCEEDED(d3dDevice_->CreateTexture2D(&copyDesc, nullptr,
                                                    &cachedTexture_))) {
            cachedWidth_ = desc.Width;
            cachedHeight_ = desc.Height;
          }
        }

        if (cachedTexture_) {
          // Copy texture (OBS does this in callback too)
          MediaEngine_LockD3D11();
          d3dContext_->CopyResource(cachedTexture_, texture);
          MediaEngine_UnlockD3D11();

          // Pass Raw QPC to MediaEngine
          LARGE_INTEGER t;
          QueryPerformanceCounter(&t);
          int64_t timestampQPC = t.QuadPart;

          // Call the frame callback to push to queue
          if (frameCallback_) {
            cachedTexture_->AddRef();
            frameCallback_(cachedTexture_, cachedWidth_, cachedHeight_,
                           timestampQPC);
          }

          callbackFrameCount_.fetch_add(1, std::memory_order_relaxed);
        }
        texture->Release();
      }
      access->Release();
    }

    winrtFrame.Close();

    // Also signal event for legacy waiters (if any)
    if (frameArrivedEvent_) {
      SetEvent(frameArrivedEvent_);
    }
  }

  bool CreateWinRTDevice() {
    // Get DXGI device from D3D11 device
    IDXGIDevice *dxgiDevice = nullptr;
    HRESULT hr =
        d3dDevice_->QueryInterface(__uuidof(IDXGIDevice), (void **)&dxgiDevice);
    if (FAILED(hr))
      return false;

    // Create WinRT device interop
    winrt::com_ptr<IInspectable> inspectable;
    hr = CreateDirect3D11DeviceFromDXGIDevice(dxgiDevice, inspectable.put());
    dxgiDevice->Release();

    if (FAILED(hr))
      return false;

    winrtDevice_ = inspectable.as<winrt::IDirect3DDevice>();
    return winrtDevice_ != nullptr;
  }

  bool CreateForMonitor(HMONITOR hmon) {
    auto interopFactory =
        winrt::get_activation_factory<winrt::GraphicsCaptureItem,
                                      IGraphicsCaptureItemInterop>();

    winrt::GraphicsCaptureItem item{nullptr};
    HRESULT hr = interopFactory->CreateForMonitor(
        hmon, winrt::guid_of<winrt::GraphicsCaptureItem>(),
        winrt::put_abi(item));

    if (FAILED(hr) || !item) {
      LogError("[WGC] CreateForMonitor failed: 0x%x", hr);
      return false;
    }

    item_ = item;
    return true;
  }

  bool CreateForWindow(HWND hwnd) {
    auto interopFactory =
        winrt::get_activation_factory<winrt::GraphicsCaptureItem,
                                      IGraphicsCaptureItemInterop>();

    winrt::GraphicsCaptureItem item{nullptr};
    HRESULT hr = interopFactory->CreateForWindow(
        hwnd, winrt::guid_of<winrt::GraphicsCaptureItem>(),
        winrt::put_abi(item));

    if (FAILED(hr) || !item) {
      LogError("[WGC] CreateForWindow failed: 0x%x", hr);
      return false;
    }

    item_ = item;
    return true;
  }

  bool StartCapture(uint32_t &width, uint32_t &height, bool captureCursor) {
    if (!item_ || !winrtDevice_)
      return false;

    auto size = item_.Size();
    width = size.Width;
    height = size.Height;

    // CRITICAL: Must use CreateFreeThreaded (not Create) because we have no
    // message pump! Create() requires a DispatcherQueue pumping messages for
    // callbacks to fire. CreateFreeThreaded() uses an internal worker thread
    // for callbacks. Using 4 buffers (instead of 2) to reduce frame loss when
    // draining is slow
    framePool_ = winrt::Direct3D11CaptureFramePool::CreateFreeThreaded(
        winrtDevice_, winrt::DirectXPixelFormat::B8G8R8A8UIntNormalized, 4,
        size);

    // Create event for frame arrival signaling (OBS-style immediate wake)
    // Auto-reset event ensures we wake once per signal
    if (!frameArrivedEvent_) {
      frameArrivedEvent_ = CreateEvent(NULL, FALSE, FALSE, NULL); // Auto-reset
    }

    // Subscribe to FrameArrived - signals event for immediate wake
    // Like OBS, we use callback to trigger processing, but actual work
    // happens on our capture thread to avoid WinRT thread pool issues
    frameArrivedToken_ = framePool_.FrameArrived(
        [this](auto &&sender, auto &&args) { OnFrameArrived(sender, args); });

    // Create and start capture session
    session_ = framePool_.CreateCaptureSession(item_);

    // Try to request borderless access and enable border removal (like OBS)
    try {
      if (session_) {
        // Request borderless access first (required on Windows 11+)
        winrt::Windows::Graphics::Capture::GraphicsCaptureAccess::
            RequestAccessAsync(winrt::Windows::Graphics::Capture::
                                   GraphicsCaptureAccessKind::Borderless)
                .get();
        session_.IsBorderRequired(false);
        LogInfo("[WGC] Borderless access granted, border removal enabled");
      }
    } catch (...) {
      // Not available on older Windows versions
      LogInfo("[WGC] Borderless access not available (Windows 10)");
    }

    // Configure cursor capture
    try {
      if (session_) {
        session_.IsCursorCaptureEnabled(captureCursor);
        LogInfo("[WGC] Native cursor capture allowed: %s",
                captureCursor ? "YES" : "NO");
      }
    } catch (...) {
      // Not available on older Windows versions
      LogInfo("[WGC] IsCursorCaptureEnabled not available");
    }

    session_.StartCapture();
    LogInfo("[WGC] Capture session started: %dx%d", width, height);
    return true;
  }

  void StopCapture() {
    if (session_) {
      session_.Close();
      session_ = nullptr;
    }
    if (framePool_) {
      // Unsubscribe from frame arrived event
      framePool_.FrameArrived(frameArrivedToken_);
      framePool_.Close();
      framePool_ = nullptr;
    }
    // NOTE: Do NOT null item_ - it's the capture target (monitor) and doesn't
    // change between recordings. StartCapture() needs item_ to exist.

    std::lock_guard<std::mutex> lock(frameMutex_);
    if (latestFrame_) {
      latestFrame_->Release();
      latestFrame_ = nullptr;
    }
    if (cachedTexture_) {
      cachedTexture_->Release();
      cachedTexture_ = nullptr;
    }
    frameReady_ = false;
  }

  bool GetNextFrame(WGCCapturedFrame &frame) {
    if (!framePool_)
      return false;

    // Simple drain-style polling: get the latest available frame
    winrt::Direct3D11CaptureFrame latestWinrtFrame = nullptr;
    winrt::Direct3D11CaptureFrame nextFrame = nullptr;

    // Drain pool to get latest frame (drop older frames)
    while ((nextFrame = framePool_.TryGetNextFrame()) != nullptr) {
      if (latestWinrtFrame) {
        latestWinrtFrame.Close(); // Release older frame
      }
      latestWinrtFrame = nextFrame;
    }

    if (!latestWinrtFrame) {
      // No new frame available
      return false;
    }

    // Process the latest frame
    bool success = false;
    auto surface = latestWinrtFrame.Surface();
    IDirect3DDxgiInterfaceAccess *access = nullptr;

    if (SUCCEEDED(surface.as<IUnknown>()->QueryInterface(
            IID_IDirect3DDxgiInterfaceAccess, (void **)&access)) &&
        access) {

      ID3D11Texture2D *texture = nullptr;
      if (SUCCEEDED(access->GetInterface(__uuidof(ID3D11Texture2D),
                                         (void **)&texture)) &&
          texture) {

        D3D11_TEXTURE2D_DESC desc;
        texture->GetDesc(&desc);

        // Recreate cache if needed
        if (!cachedTexture_ || cachedWidth_ != (int32_t)desc.Width ||
            cachedHeight_ != (int32_t)desc.Height) {
          if (cachedTexture_)
            cachedTexture_->Release();
          D3D11_TEXTURE2D_DESC copyDesc = desc;
          copyDesc.Usage = D3D11_USAGE_DEFAULT;
          copyDesc.BindFlags =
              D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
          copyDesc.CPUAccessFlags = 0;
          copyDesc.MiscFlags = 0;

          if (FAILED(d3dDevice_->CreateTexture2D(&copyDesc, nullptr,
                                                 &cachedTexture_))) {
            LogError("[WGC] Failed to create cache texture");
            // Continue, will ensure cleanup
          } else {
            cachedWidth_ = desc.Width;
            cachedHeight_ = desc.Height;
            LogInfo("[WGC] (Pull) New cache texture: %dx%d", cachedWidth_,
                    cachedHeight_);
          }
        }

        if (cachedTexture_) {
          LARGE_INTEGER t2, freq;

          MediaEngine_LockD3D11();
          d3dContext_->CopyResource(cachedTexture_, texture);
          MediaEngine_UnlockD3D11();

          // Pass Raw QPC
          QueryPerformanceCounter(&t2);
          frame.timestamp = t2.QuadPart;
          success = true;
        }
        texture->Release();
      }
      access->Release();
    }

    latestWinrtFrame.Close();
    return success;
  }
#else
  // Stub implementation when WGC headers not available
  ID3D11Device *d3dDevice_ = nullptr;
  ID3D11DeviceContext *d3dContext_ = nullptr;

  bool CreateWinRTDevice() { return false; }
  bool CreateForMonitor(void *) { return false; }
  bool CreateForWindow(void *) { return false; }
  bool StartCapture(uint32_t &, uint32_t &) { return false; }
  void StopCapture() {}
  bool GetNextFrame(WGCCapturedFrame &) { return false; }
#endif
};

WGCCapture::WGCCapture() : impl_(std::make_unique<Impl>()) {}

WGCCapture::~WGCCapture() { StopCapture(); }

bool WGCCapture::IsSupported() {
#if HAS_WGC
  // Check if GraphicsCaptureSession is supported
  return winrt::GraphicsCaptureSession::IsSupported();
#else
  return false;
#endif
}

bool WGCCapture::Init(ID3D11Device *device) {
#if HAS_WGC
  device_ = device;
  device_->GetImmediateContext(&context_);
  impl_->d3dDevice_ = device;
  impl_->d3dContext_ = context_;

  // Initialize COM for WinRT
  HRESULT hr = RoInitialize(RO_INIT_MULTITHREADED);
  if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
    LogError("[WGC] RoInitialize failed: 0x%x", hr);
    return false;
  }

  if (!impl_->CreateWinRTDevice()) {
    LogError("[WGC] Failed to create WinRT device");
    return false;
  }

  // Get primary monitor
  HMONITOR hmon =
      MonitorFromWindow(GetDesktopWindow(), MONITOR_DEFAULTTOPRIMARY);
  if (!impl_->CreateForMonitor(hmon)) {
    return false;
  }

  initialized_ = true;
  LogInfo("[WGC] Initialized for primary monitor");
  return true;
#else
  LogError("[WGC] Not available - WinRT headers not found");
  return false;
#endif
}

bool WGCCapture::InitForWindow(ID3D11Device *device, void *hwnd) {
#if HAS_WGC
  device_ = device;
  device_->GetImmediateContext(&context_);
  impl_->d3dDevice_ = device;
  impl_->d3dContext_ = context_;

  HRESULT hr = RoInitialize(RO_INIT_MULTITHREADED);
  if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
    LogError("[WGC] RoInitialize failed: 0x%x", hr);
    return false;
  }

  if (!impl_->CreateWinRTDevice()) {
    LogError("[WGC] Failed to create WinRT device");
    return false;
  }

  if (!impl_->CreateForWindow((HWND)hwnd)) {
    return false;
  }

  initialized_ = true;
  LogInfo("[WGC] Initialized for window 0x%p", hwnd);
  return true;
#else
  LogError("[WGC] Not available - WinRT headers not found");
  return false;
#endif
}

void WGCCapture::SetCaptureCursor(bool enabled) { captureCursor_ = enabled; }

bool WGCCapture::InitForMonitor(ID3D11Device *device, void *hmonitor) {
#if HAS_WGC
  device_ = device;
  device_->GetImmediateContext(&context_);
  impl_->d3dDevice_ = device;
  impl_->d3dContext_ = context_;

  HRESULT hr = RoInitialize(RO_INIT_MULTITHREADED);
  if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
    LogError("[WGC] RoInitialize failed: 0x%x", hr);
    return false;
  }

  if (!impl_->CreateWinRTDevice()) {
    LogError("[WGC] Failed to create WinRT device");
    return false;
  }

  if (!impl_->CreateForMonitor((HMONITOR)hmonitor)) {
    return false;
  }

  initialized_ = true;
  LogInfo("[WGC] Initialized for monitor 0x%p", hmonitor);
  return true;
#else
  LogError("[WGC] Not available - WinRT headers not found");
  return false;
#endif
}

bool WGCCapture::StartCapture() {
  if (!initialized_)
    return false;

  bool result = impl_->StartCapture(width_, height_, captureCursor_);
  if (result) {
    capturing_ = true;
    LogInfo("[WGC] Capture started");
  }
  return result;
}

void WGCCapture::StopCapture() {
  capturing_ = false;
  impl_->StopCapture();
  LogInfo("[WGC] Capture stopped");
}

bool WGCCapture::GetNextFrame(WGCCapturedFrame &frame) {
  if (!capturing_)
    return false;
  return impl_->GetNextFrame(frame);
}

HANDLE WGCCapture::GetFrameArrivedEvent() const {
#if HAS_WGC
  return impl_ ? impl_->frameArrivedEvent_ : NULL;
#else
  return NULL;
#endif
}

void WGCCapture::SetDirectFrameCallback(
    std::function<void(ID3D11Texture2D *, uint32_t, uint32_t, int64_t)>
        callback) {
#if HAS_WGC
  if (impl_) {
    impl_->frameCallback_ = callback;
  }
#endif
}

uint32_t WGCCapture::GetCallbackFrameCount() const {
#if HAS_WGC
  return impl_ ? impl_->callbackFrameCount_.load(std::memory_order_relaxed) : 0;
#else
  return 0;
#endif
}
