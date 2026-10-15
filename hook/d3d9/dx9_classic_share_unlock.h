#pragma once

#include <d3d9.h>

#include <windows.h>

// Opens the Windows D3D9 runtime's Ex-only shared-resource gate for one call on a
// classic device by pointing the device's internal factory field at CE's Ex
// reference factory; see d3d9_ex_flag_layout.h and d3d9_device_lock_layout.h for
// how the layouts are located and validated.

// Locates the layout once per process from CE's own Ex device/factory. Later calls
// return the cached result. The function must belong to the system d3d9.dll.
bool DX9_LocateClassicShareLayout(IDirect3DDevice9Ex* exDevice, IDirect3D9Ex* exFactory);

// Points `device`'s factory field at CE's Ex reference factory for the scope's
// lifetime and restores it on exit; the game's factory object is never written.
// A multithreaded device is held under the runtime's own device lock for the whole
// window, so its other threads never observe the swap; without a proven lock layout
// it keeps the fallback. A null device, an Ex device, an unknown layout, adapters
// the two factories disagree on, or any failed validation leaves everything
// untouched (engaged() == false).
// `ceOwnedFactory` names a CE-private factory that no application thread uses.
class DX9ClassicShareUnlockScope {
public:
    DX9ClassicShareUnlockScope(IDirect3DDevice9* device, const IDirect3D9* ceOwnedFactory, const char* label);
    ~DX9ClassicShareUnlockScope();
    DX9ClassicShareUnlockScope(const DX9ClassicShareUnlockScope&) = delete;
    DX9ClassicShareUnlockScope& operator=(const DX9ClassicShareUnlockScope&) = delete;

    bool engaged() const { return factoryField_ != nullptr; }

private:
    IDirect3D9* factory_ = nullptr;  // the device's own factory, restored on exit
    void* volatile* factoryField_ = nullptr;
    CRITICAL_SECTION* deviceLock_ = nullptr;
    const char* label_ = nullptr;
};
