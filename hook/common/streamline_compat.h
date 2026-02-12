#pragma once

#include <Unknwn.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <windows.h>

// Streamline Native Interface GUID
// {ADEC44E2-61F0-45C3-AD9F-1B37379284FF}
// Used to query native (non-proxy) interfaces from Streamline-wrapped objects
// When DLSS-FG or FSR-FG is active, Streamline wraps device, queue, and
// swapchain Third-party overlays MUST use native interfaces to avoid crashes
static const GUID IID_StreamlineNativeInterface = {
    0xADEC44E2,
    0x61F0,
    0x45C3,
    {0xAD, 0x9F, 0x1B, 0x37, 0x37, 0x92, 0x84, 0xFF}};

// Function pointer type for slGetNativeInterface
typedef int (*slGetNativeInterface_pfn)(void *proxyInterface,
                                        void **baseInterface);

// Debug logging function (defined elsewhere)
void EarlyLog(const char *fmt, ...);

// Query the native (non-proxy) interface from a Streamline-wrapped object
// Returns true if pProxy was a Streamline proxy and ppNative now contains the
// native interface Returns false if pProxy is not a Streamline proxy (ppNative
// will be nullptr)
inline bool GetStreamlineNativeInterface(IUnknown *pProxy, void **ppNative,
                                         const char *objName = "object") {
  if (!pProxy || !ppNative)
    return false;
  *ppNative = nullptr;

  static bool firstCall = true;

  // Method 1: Try slGetNativeInterface from sl.interposer.dll (per SpecialK)
  static slGetNativeInterface_pfn slGetNativeInterface = nullptr;
  static bool slFuncChecked = false;
  static HMODULE hInterposer = nullptr;

  if (!slFuncChecked) {
    slFuncChecked = true;
    hInterposer = GetModuleHandleA("sl.interposer.dll");
    if (hInterposer) {
      slGetNativeInterface = (slGetNativeInterface_pfn)GetProcAddress(
          hInterposer, "slGetNativeInterface");
      if (firstCall) {
        EarlyLog(
            "DX12 FG: sl.interposer.dll found at %p, slGetNativeInterface=%p",
            hInterposer, slGetNativeInterface);
      }
    } else if (firstCall) {
      EarlyLog("DX12 FG: sl.interposer.dll NOT found");
    }
  }

  if (slGetNativeInterface) {
    void *native = nullptr;
    int result = slGetNativeInterface(pProxy, &native);
    if (firstCall) {
      EarlyLog("DX12 FG: slGetNativeInterface(%s %p) returned %d, native=%p",
               objName, pProxy, result, native);
    }
    if (result == 0 && native != nullptr && native != pProxy) {
      *ppNative = native;
      firstCall = false;
      return true;
    }
  }

  // Method 2: Try QueryInterface with Streamline GUID
  HRESULT hr = pProxy->QueryInterface(IID_StreamlineNativeInterface, ppNative);
  if (firstCall) {
    EarlyLog(
        "DX12 FG: QueryInterface(%s %p, StreamlineGUID) hr=0x%08X, native=%p",
        objName, pProxy, hr, *ppNative);
  }
  if (SUCCEEDED(hr) && *ppNative != nullptr) {
    // QueryInterface adds a ref, release it since caller will manage lifetime
    ((IUnknown *)*ppNative)->Release();
    if (*ppNative != pProxy) {
      firstCall = false;
      return true;
    }
  }

  firstCall = false;
  return false;
}

// Check if an object is a Streamline proxy
inline bool IsStreamlineProxy(IUnknown *pObject) {
  if (!pObject)
    return false;
  void *pNative = nullptr;
  HRESULT hr = pObject->QueryInterface(IID_StreamlineNativeInterface, &pNative);
  if (SUCCEEDED(hr) && pNative != nullptr) {
    ((IUnknown *)pNative)->Release();
    return (pNative != pObject);
  }
  return false;
}

// Get native D3D12 device from potentially proxied device
inline ID3D12Device *GetNativeDevice(ID3D12Device *pDevice) {
  if (!pDevice)
    return nullptr;

  ID3D12Device *pNative = nullptr;
  if (GetStreamlineNativeInterface(pDevice, (void **)&pNative, "Device")) {
    // Add ref since we're returning a new pointer
    pNative->AddRef();
    return pNative;
  }
  // Not a proxy, return original with added ref
  pDevice->AddRef();
  return pDevice;
}

// Get native command queue from potentially proxied queue
inline ID3D12CommandQueue *GetNativeCommandQueue(ID3D12CommandQueue *pQueue) {
  if (!pQueue)
    return nullptr;

  ID3D12CommandQueue *pNative = nullptr;
  if (GetStreamlineNativeInterface(pQueue, (void **)&pNative, "Queue")) {
    pNative->AddRef();
    return pNative;
  }
  pQueue->AddRef();
  return pQueue;
}

// Get native swapchain from potentially proxied swapchain
inline IDXGISwapChain *GetNativeSwapChain(IDXGISwapChain *pSwapChain) {
  if (!pSwapChain)
    return nullptr;

  IDXGISwapChain *pNative = nullptr;
  if (GetStreamlineNativeInterface(pSwapChain, (void **)&pNative,
                                   "SwapChain")) {
    pNative->AddRef();
    return pNative;
  }
  pSwapChain->AddRef();
  return pSwapChain;
}

// Get native swapchain3 from potentially proxied swapchain
inline IDXGISwapChain3 *GetNativeSwapChain3(IDXGISwapChain3 *pSwapChain) {
  if (!pSwapChain)
    return nullptr;

  IDXGISwapChain3 *pNative = nullptr;
  if (GetStreamlineNativeInterface(pSwapChain, (void **)&pNative,
                                   "SwapChain3")) {
    pNative->AddRef();
    return pNative;
  }
  pSwapChain->AddRef();
  return pSwapChain;
}
