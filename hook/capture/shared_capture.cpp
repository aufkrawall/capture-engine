/**
 * Shared Capture Implementation — capture manager
 *
 * Registry of shared capture targets. The D3D12 zero-copy capture lives in
 * shared_capture_d3d12.cpp.
 */

#include "shared_capture.h"
#include <string>
#include <unordered_map>

// ============================================================================
// CaptureManager Implementation
// ============================================================================

CaptureManager& CaptureManager::Get() {
    // Shared capture targets are themselves process-lifetime statics in several
    // hook modules.  A destructed function-static manager can therefore be
    // reached by a later target destructor during CRT/DLL teardown.  Keep the
    // registry alive until process termination; Windows reclaims it together
    // with the injected process.
    static CaptureManager* const instance = new CaptureManager();
    return *instance;
}

void CaptureManager::RegisterCaptureTarget(const char* name, ISharedCaptureTarget* target) {
    if (!name || !*name || !target)
        return;
    std::lock_guard<std::mutex> lock(m_Lock);
    m_Targets[name] = target;
}

void CaptureManager::UnregisterCaptureTarget(const char* name, ISharedCaptureTarget* target) {
    if (!name || !*name)
        return;
    std::lock_guard<std::mutex> lock(m_Lock);
    auto it = m_Targets.find(name);
    if (it != m_Targets.end() && (!target || it->second == target)) {
        m_Targets.erase(it);
    }
}

ISharedCaptureTarget* CaptureManager::GetCaptureTarget(const char* name) {
    if (!name || !*name)
        return nullptr;
    std::lock_guard<std::mutex> lock(m_Lock);
    auto it = m_Targets.find(name);
    return (it != m_Targets.end()) ? it->second : nullptr;
}

void CaptureManager::SetCaptureEnabled(bool enabled) {
    m_CaptureEnabled = enabled;
}
