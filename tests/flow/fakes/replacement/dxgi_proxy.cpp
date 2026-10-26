// A [ThirdParty] custom_dll_paths stand-in built as replacement/dxgi.dll: a proxy over System32's
// dxgi.dll, the shape of ReShade or Special K installed as dxgi.dll. It reaches the image it wraps the
// way those proxies do, by loading System32's path, which CE must leave to it (FlowModuleReplacement):
// handing a configured custom DLL its own image for that path would make every export call itself.

#include <windows.h>

#include <atomic>

namespace {

std::atomic<long> g_calls{0};

HMODULE OwnImage() {
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&g_calls), &self);
    return self;
}

HMODULE WrappedImage() {
    static const HMODULE module = [] {
        wchar_t path[MAX_PATH] = {};
        const UINT length = GetSystemDirectoryW(path, MAX_PATH);
        if (length == 0 || length + 10 >= MAX_PATH)
            return HMODULE{};
        lstrcatW(path, L"\\dxgi.dll");
        return LoadLibraryW(path);
    }();
    return module;
}

template <typename Function>
Function WrappedExport(const char* name) {
    const HMODULE wrapped = WrappedImage();
    if (!wrapped || wrapped == OwnImage())
        return nullptr;
    return reinterpret_cast<Function>(GetProcAddress(wrapped, name));
}

}  // namespace

extern "C" __declspec(dllexport) HRESULT WINAPI CreateDXGIFactory(const GUID& riid, void** factory) {
    ++g_calls;
    const auto real = WrappedExport<HRESULT(WINAPI*)(const GUID&, void**)>("CreateDXGIFactory");
    return real ? real(riid, factory) : E_FAIL;
}

extern "C" __declspec(dllexport) HRESULT WINAPI CreateDXGIFactory1(const GUID& riid, void** factory) {
    ++g_calls;
    const auto real = WrappedExport<HRESULT(WINAPI*)(const GUID&, void**)>("CreateDXGIFactory1");
    return real ? real(riid, factory) : E_FAIL;
}

extern "C" __declspec(dllexport) HRESULT WINAPI CreateDXGIFactory2(UINT flags, const GUID& riid, void** factory) {
    ++g_calls;
    const auto real = WrappedExport<HRESULT(WINAPI*)(UINT, const GUID&, void**)>("CreateDXGIFactory2");
    return real ? real(flags, riid, factory) : E_FAIL;
}

extern "C" __declspec(dllexport) HRESULT WINAPI DXGIGetDebugInterface1(UINT flags, const GUID& riid, void** debug) {
    ++g_calls;
    const auto real = WrappedExport<HRESULT(WINAPI*)(UINT, const GUID&, void**)>("DXGIGetDebugInterface1");
    return real ? real(flags, riid, debug) : E_FAIL;
}

// Calls that reached this replacement, through any route.
extern "C" __declspec(dllexport) long CEFake_ReplacementCalls() {
    return g_calls.load();
}

// The image this proxy forwards to: System32's dxgi.dll, never itself.
extern "C" __declspec(dllexport) HMODULE CEFake_WrappedImage() {
    return WrappedImage();
}
