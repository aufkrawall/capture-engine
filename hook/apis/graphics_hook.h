#pragma once

class GraphicsHook {
public:
    virtual ~GraphicsHook() = default;

    // Called when the hook DLL loads/initializes
    virtual void Init() = 0;

    // Called periodically or on events
    virtual void Update() {}

    // Called when host captureengine dies (before reconnection attempt)
    // Stops capture threads and resets state so we can reconnect cleanly
    virtual void OnHostDisconnect() {}

    // Called on unload
    virtual void Shutdown() = 0;
};
// Reentrancy guard to prevent double-waiting/double-capturing when multiple hooks are active (e.g. DX11 + DX12)
extern thread_local bool g_InPresentHook;
