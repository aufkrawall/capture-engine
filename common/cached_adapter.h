#pragma once

/**
 * Cached DXGI Adapter Information
 * 
 * Caches DXGI adapter properties to avoid redundant queries.
 * Thread-safe singleton for use across multiple hooks.
 */

#include <windows.h>
#include <dxgi.h>
#include <dxgi1_6.h>
#include <atomic>
#include <string>
#include <mutex>

namespace ce {

struct AdapterInfo {
    LUID luid;
    DXGI_ADAPTER_DESC desc;
    std::wstring description;
    UINT vendorId;
    UINT deviceId;
    UINT subSysId;
    UINT revision;
    SIZE_T dedicatedVideoMemory;
    SIZE_T dedicatedSystemMemory;
    SIZE_T sharedSystemMemory;
    bool valid = false;
    
    // Cached derived info
    bool isNvidia = false;
    bool isAMD = false;
    bool isIntel = false;
    
    void UpdateDerivedInfo();
};

// Singleton class for cached adapter information
class CachedAdapterManager {
public:
    static CachedAdapterManager& Get();
    
    // Query and cache adapter for a device
    // Returns true if successful, false if failed
    bool CacheAdapterFromDevice(IUnknown* device);
    
    // Query and cache adapter from LUID
    bool CacheAdapterFromLUID(const LUID& luid);
    
    // Query and cache primary adapter (adapter 0)
    bool CachePrimaryAdapter();
    
    // Get cached adapter info (returns nullptr if not cached)
    const AdapterInfo* GetCachedInfo() const;
    
    // Check if we have cached info
    bool HasCachedInfo() const { return m_cached.load(std::memory_order_acquire); }
    
    // Clear cached info (e.g., on device change)
    void ClearCache();
    
    // Get adapter description string (cached or "Unknown")
    std::wstring GetAdapterDescription() const;
    
    // Get VRAM size in MB
    UINT64 GetVRAM_MB() const;
    
    // Check if cached adapter is NVIDIA/AMD/Intel
    bool IsNvidia() const;
    bool IsAMD() const;
    bool IsIntel() const;
    
    // Get LUID (returns zero LUID if not cached)
    LUID GetLUID() const;

private:
    CachedAdapterManager() = default;
    ~CachedAdapterManager() = default;
    
    CachedAdapterManager(const CachedAdapterManager&) = delete;
    CachedAdapterManager& operator=(const CachedAdapterManager&) = delete;
    
    mutable std::mutex m_mutex;
    AdapterInfo m_info;
    std::atomic<bool> m_cached{false};
};

// Helper macros for easy access
#define ADAPTER_MGR ce::CachedAdapterManager::Get()

} // namespace ce
