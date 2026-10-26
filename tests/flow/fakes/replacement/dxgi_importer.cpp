// An application module mapped after a custom DLL replaced the resident dxgi.dll (FlowModuleReplacement):
// it imports dxgi.dll statically, so the loader binds it to the first image of that name, and it asks
// for modules through its own loader imports, which CE patches in every module it sees load.

#include <dxgi1_3.h>
#include <windows.h>

// The import slot itself (the MinGW import library's symbol for it).
extern "C" void* __imp_CreateDXGIFactory2;

namespace {
void* g_factoryImportAtAttach = nullptr;
}

extern "C" BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH)
        g_factoryImportAtAttach = __imp_CreateDXGIFactory2;
    return TRUE;
}

// Where the dxgi.dll import pointed when this module's DllMain ran: CE rebinds it from the loader
// notification, which arrives once the loader has bound the imports and before DllMain.
extern "C" __declspec(dllexport) void* CEFake_FactoryImportAtAttach() {
    return g_factoryImportAtAttach;
}

extern "C" __declspec(dllexport) HRESULT CEFake_CreateFactoryThroughImport() {
    IDXGIFactory2* factory = nullptr;
    const HRESULT hr = CreateDXGIFactory2(0, __uuidof(IDXGIFactory2), reinterpret_cast<void**>(&factory));
    if (factory)
        factory->Release();
    return hr;
}

extern "C" __declspec(dllexport) HMODULE CEFake_GetModuleHandleW(const wchar_t* name) {
    return GetModuleHandleW(name);
}

extern "C" __declspec(dllexport) HMODULE CEFake_GetModuleHandleA(const char* name) {
    return GetModuleHandleA(name);
}

extern "C" __declspec(dllexport) HMODULE CEFake_LoadLibraryW(const wchar_t* name) {
    return LoadLibraryW(name);
}

extern "C" __declspec(dllexport) void* CEFake_GetProcAddress(HMODULE module, const char* name) {
    return reinterpret_cast<void*>(GetProcAddress(module, name));
}
