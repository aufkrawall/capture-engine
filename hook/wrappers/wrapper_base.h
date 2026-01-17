/**
 * COM Wrapper Base Infrastructure
 * 
 * Provides base classes and utilities for creating COM interface wrappers
 * that replace MinHook-based function hooking.
 */

#pragma once

#include <Windows.h>
#include <dxgi1_6.h>
#include <d3d12.h>
#include <d3d11_4.h>
#include <atomic>
#include <mutex>

// Forward declarations
class CWrapDXGIFactory;
class CWrapDXGISwapChain;
class CWrapD3D12Device;
class CWrapD3D12CommandQueue;

// ============================================================================
// Wrapper GUIDs - Used to identify our wrappers and prevent unwrapping
// ============================================================================

// {A1B2C3D4-E5F6-7890-ABCD-EF1234567890}
static const GUID IID_CWrapDXGISwapChain = 
{ 0xa1b2c3d4, 0xe5f6, 0x7890, { 0xab, 0xcd, 0xef, 0x12, 0x34, 0x56, 0x78, 0x90 } };

// {B2C3D4E5-F678-90AB-CDEF-123456789012}
static const GUID IID_CWrapDXGIFactory = 
{ 0xb2c3d4e5, 0xf678, 0x90ab, { 0xcd, 0xef, 0x12, 0x34, 0x56, 0x78, 0x90, 0x12 } };

// {C3D4E5F6-7890-ABCD-EF12-345678901234}
static const GUID IID_CWrapD3D12Device = 
{ 0xc3d4e5f6, 0x7890, 0xabcd, { 0xef, 0x12, 0x34, 0x56, 0x78, 0x90, 0x12, 0x34 } };

// {D4E5F678-90AB-CDEF-1234-567890123456}
static const GUID IID_CWrapD3D12CommandQueue = 
{ 0xd4e5f678, 0x90ab, 0xcdef, { 0x12, 0x34, 0x56, 0x78, 0x90, 0x12, 0x34, 0x56 } };

// {A0B1C2D3-E4F5-6789-0123-456789ABCDEF}
static const GUID IID_ICWrapDXGIAdapter = 
{ 0xa0b1c2d3, 0xe4f5, 0x6789, { 0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef } };

// Interface for unwrapping
struct ICWrapDXGIAdapter : public IUnknown {
    virtual IDXGIAdapter* GetReal() = 0;
};

// ============================================================================
// Known FG Runtime Unwrap GUIDs - Block these to prevent unwrapping
// ============================================================================

// FSR3 FrameInterpolationSwapChain
static const GUID IID_IFfxFrameInterpolationSwapChain =
{ 0x36C0A582, 0x4D87, 0x4E98, { 0x82, 0x1F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01 } };

// FSR4 FrameInterpolationSwapChainDX12
static const GUID IID_IFrameInterpolationSwapChainDX12 =
{ 0x36C0A582, 0x4D87, 0x4E98, { 0x82, 0x1F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02 } };

// Streamline unwrap attempt marker
static const GUID IID_StreamlineUnwrap =
{ 0x5868f018, 0xafda, 0x4f5f, { 0xad, 0xa9, 0xbf, 0x66, 0xcc, 0xc6, 0x2b, 0x6e } };

// ============================================================================
// Helper Macros for COM Implementation
// ============================================================================

#define WRAP_REFCOUNT_IMPL() \
    ULONG STDMETHODCALLTYPE AddRef() override { \
        return InterlockedIncrement(&m_RefCount); \
    } \
    ULONG STDMETHODCALLTYPE Release() override { \
        ULONG count = InterlockedDecrement(&m_RefCount); \
        if (count == 0) { \
            delete this; \
        } \
        return count; \
    }

// Logging helper
void WrapperLog(const char* fmt, ...);

// ============================================================================
// Check if a GUID is a known unwrap attempt
// ============================================================================
inline bool IsUnwrapAttemptGUID(REFIID riid) {
    return riid == IID_IFfxFrameInterpolationSwapChain ||
           riid == IID_IFrameInterpolationSwapChainDX12 ||
           riid == IID_StreamlineUnwrap;
}

// ============================================================================
// Wrapper State Manager - Global tracking of wrapped objects
// ============================================================================
class WrapperStateManager {
public:
    static WrapperStateManager& Get() {
        static WrapperStateManager instance;
        return instance;
    }
    
    // Track wrapped swapchains
    void RegisterSwapchain(CWrapDXGISwapChain* pWrapper, IDXGISwapChain* pReal);
    void UnregisterSwapchain(CWrapDXGISwapChain* pWrapper);
    CWrapDXGISwapChain* FindWrapper(IDXGISwapChain* pReal);
    
    // Overlay state
    std::atomic<bool> overlayEnabled{true};
    std::atomic<bool> swapchainInvalid{false};
    
private:
    WrapperStateManager() = default;
    std::mutex m_Lock;
    // Using simple arrays to avoid STL in hot paths
    static constexpr int MAX_SWAPCHAINS = 16;
    CWrapDXGISwapChain* m_Wrappers[MAX_SWAPCHAINS] = {};
    IDXGISwapChain* m_RealSwapchains[MAX_SWAPCHAINS] = {};
};
