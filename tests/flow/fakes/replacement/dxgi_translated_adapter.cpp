// The adapter of the replacement/dxgi.dll stand-in (dxgi_proxy.cpp), the shape of DXVK's: its own COM object
// that answers only the public interfaces. System32's D3D11 asks every adapter for a private one and faults on
// the empty answer; GTA V Enhanced's Epic overlay passed DXVK's adapter to D3D11CreateDevice that way (session
// 20261026_175321, FlowModuleReplacement). A TU of its own: <dxgi.h> declares the exports the proxy defines.

#include <windows.h>

#include <dxgi.h>

#include <atomic>

namespace {

class TranslatedAdapter final : public IDXGIAdapter {
public:
    explicit TranslatedAdapter(IDXGIAdapter* inner) : inner_(inner) {}
    TranslatedAdapter(const TranslatedAdapter&) = delete;
    TranslatedAdapter& operator=(const TranslatedAdapter&) = delete;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override {
        if (!object)
            return E_POINTER;
        if (IsEqualIID(riid, __uuidof(IUnknown)) || IsEqualIID(riid, __uuidof(IDXGIObject)) ||
            IsEqualIID(riid, __uuidof(IDXGIAdapter))) {
            *object = static_cast<IDXGIAdapter*>(this);
            AddRef();
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG left = --references_;
        if (left == 0)
            delete this;
        return left;
    }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID name, UINT size, const void* data) override {
        return inner_->SetPrivateData(name, size, data);
    }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID name, const IUnknown* unknown) override {
        return inner_->SetPrivateDataInterface(name, unknown);
    }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID name, UINT* size, void* data) override {
        return inner_->GetPrivateData(name, size, data);
    }
    HRESULT STDMETHODCALLTYPE GetParent(REFIID riid, void** parent) override { return inner_->GetParent(riid, parent); }
    HRESULT STDMETHODCALLTYPE EnumOutputs(UINT index, IDXGIOutput** output) override {
        return inner_->EnumOutputs(index, output);
    }
    HRESULT STDMETHODCALLTYPE GetDesc(DXGI_ADAPTER_DESC* desc) override { return inner_->GetDesc(desc); }
    HRESULT STDMETHODCALLTYPE CheckInterfaceSupport(REFGUID name, LARGE_INTEGER* version) override {
        return inner_->CheckInterfaceSupport(name, version);
    }

private:
    ~TranslatedAdapter() { inner_->Release(); }

    IDXGIAdapter* inner_;
    std::atomic<ULONG> references_{1};
};

}  // namespace

// This replacement's first adapter: System32's adapter 0 behind the translation layer's own object.
extern "C" __declspec(dllexport) HRESULT CEFake_CreateTranslatedAdapter(IDXGIAdapter** adapter) {
    if (!adapter)
        return E_POINTER;
    *adapter = nullptr;
    wchar_t path[MAX_PATH] = {};
    const UINT length = GetSystemDirectoryW(path, MAX_PATH);
    if (length == 0 || length + 10 >= MAX_PATH)
        return E_FAIL;
    lstrcatW(path, L"\\dxgi.dll");
    // A configured custom DLL keeps the System32 image it names by path (FlowModuleReplacement).
    const HMODULE system = LoadLibraryW(path);
    const auto create =
        system ? reinterpret_cast<HRESULT(WINAPI*)(REFIID, void**)>(GetProcAddress(system, "CreateDXGIFactory1"))
               : nullptr;
    IDXGIFactory1* factory = nullptr;
    if (!create || FAILED(create(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory))) || !factory)
        return E_FAIL;
    IDXGIAdapter* inner = nullptr;
    const HRESULT hr = factory->EnumAdapters(0, &inner);
    factory->Release();
    if (FAILED(hr) || !inner)
        return FAILED(hr) ? hr : E_FAIL;
    *adapter = new TranslatedAdapter(inner);
    return S_OK;
}
