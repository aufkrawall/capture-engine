#pragma once
#include <guiddef.h>
#include "graphics_hook.h"

// DirectDraw/DX6/DX7 Hook - captures games using DirectDraw surfaces
// Handles hybrid DDraw+D3D7 games where both render to same surface
// Copies locked DirectDraw pixels into the shared D3D11 capture ring; the
// native D3D7 overlay sidecar avoids CPU surface access on regular 3D frames.
class DDrawHook : public GraphicsHook {
public:
    void Init() override;
    void Shutdown() override;
    void OnHostDisconnect() override;
};

bool HookDirectDrawObject(void* directDrawObject, REFIID iid);

// Whether the process has actually used DirectDraw: an application interface
// was accepted or a real presentation surface was activated. This is the proof
// that separates a DirectDraw title from a process that merely loaded
// ddraw.dll/d3d9.dll as a transitive dependency.
bool WasDirectDrawInterfaceObserved();
