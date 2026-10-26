#pragma once

#include <windows.h>

#include <string>
#include <vector>

// Custom DLLs that replace a module the process had already resolved from another file
// (rules and the reason in module_replacement_policy.h). The third-party preload publishes the
// replacements once; from then on application code that asks for such a module - by import,
// LoadLibrary, GetModuleHandle or GetProcAddress - gets the replacement, while Windows' own
// modules and CE keep the system image. Every query is lock-free, allocation-free on the
// common path and safe under the loader lock.
namespace ce::module_replacement {

struct CustomModule {
    HMODULE module = nullptr;
    std::wstring path;  // the configured file, as loaded
};

// Adopts every loaded custom DLL whose base name a by-name lookup still resolves to another
// image: publishes it, moves CE's own forwarding originals into it, rebinds the import slots
// application modules hold into the superseded image, and makes the superseded image's known
// runtime entry points forward application calls (addresses resolved before CE was injected).
// Once per process, after the whole custom list is loaded (a replacement may import another).
void AdoptCustomModules(const std::vector<CustomModule>& custom);

bool IsActive();

// Rebinds `module`'s imports of replaced names if it is application code. Called for every
// module mapped after adoption: from the loader notification, and again once its load has
// returned in case the notification came before the loader had bound the imports.
void RebindLoadedModule(HMODULE module);
void NoteLoadReturned(HMODULE module);
// Retries importers whose slots were still unbound; also driven by the hook thread's service
// pass for loads that never return through CE's loader hooks (delay loads).
void ServicePendingRebinds();

// The replacement a module query from `callerAddress` names, or nullptr. `nameOrPath` is a
// bare name ("d3d12", "dxgi.dll") or the superseded image's own path; see RequesterScope.
HMODULE ReplacementForModuleQuery(const char* nameOrPath, const void* callerAddress);
HMODULE ReplacementForModuleQuery(const wchar_t* nameOrPath, const void* callerAddress);

// The replacement's path for a LoadLibrary request from `callerAddress`, or nullptr.
const wchar_t* ReplacementPathForLoad(const wchar_t* request, const void* callerAddress);

// The module GetProcAddress should search: the replacement when an application caller asks
// the superseded image, else `requested`.
HMODULE ModuleForProcLookup(HMODULE requested, const void* callerAddress);

// The module a by-name lookup resolves to for application code: CE's own identity checks use
// it so a replaced runtime reads as what the game runs (the "DX12 (VKD3D-Proton)" label).
HMODULE ApplicationModuleByName(const char* baseName);

}  // namespace ce::module_replacement
