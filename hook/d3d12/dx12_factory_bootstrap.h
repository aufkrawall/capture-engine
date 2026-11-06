#pragma once

namespace ce::dx12_factory_bootstrap {

// Check before taking the install mutex: a nested LoadLibrary return can still be inside
// another DLL's DllMain. Waiting there for an installer that needs the loader lock deadlocks,
// and creating a DXGI factory from DllMain is itself unsupported. The existing hook-thread
// load notification and periodic pass retry after the callout returns.
template <typename Environment>
bool CanInstall(Environment& environment) {
    if (environment.IsInLoaderCallout()) {
        environment.NoteLoaderDeferral();
        return false;
    }
    return true;
}

}  // namespace ce::dx12_factory_bootstrap
