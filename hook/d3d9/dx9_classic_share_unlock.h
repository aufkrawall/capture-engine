#pragma once

#include <d3d9.h>

#include <windows.h>

// Opens the Windows D3D9 runtime's Ex-only shared-resource gate for one call on a
// classic device by setting its factory's internal "is Ex" BOOL; see
// d3d9_ex_flag_layout.h for how the layout is located and validated.

// Locates the layout once per process from CE's own Ex device/factory. Later calls
// return the cached result. The function must belong to the system d3d9.dll.
bool DX9_LocateClassicShareLayout(IDirect3DDevice9Ex* exDevice, IDirect3D9Ex* exFactory);

// Sets the flag on `device`'s factory for the scope's lifetime and restores it on
// exit. A null device, an Ex device, an unknown layout, a failed validation or a
// multithreaded game device leaves everything untouched (engaged() == false).
// `ceOwnedFactory` names a CE-private factory that no application thread uses.
class DX9ClassicShareUnlockScope {
public:
    DX9ClassicShareUnlockScope(IDirect3DDevice9* device, const IDirect3D9* ceOwnedFactory, const char* label);
    ~DX9ClassicShareUnlockScope();
    DX9ClassicShareUnlockScope(const DX9ClassicShareUnlockScope&) = delete;
    DX9ClassicShareUnlockScope& operator=(const DX9ClassicShareUnlockScope&) = delete;

    bool engaged() const { return flag_ != nullptr; }

private:
    IDirect3D9* factory_ = nullptr;
    volatile LONG* flag_ = nullptr;
    const char* label_ = nullptr;
};
