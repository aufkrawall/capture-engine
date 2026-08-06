#include "layer_capture_internal.h"


bool CreateD3D11InteropDevice(IDXGIAdapter* adapter,  ID3D11Device** ppDevice,  ID3D11DeviceContext** ppContext) {


    // Load the NATIVE d3d11.dll from System32 to bypass DXVK's replacement.
    // When DXVK is active, the process-level d3d11.dll is DXVK's Vulkan-backed
    // implementation whose shared handles are incompatible with Vulkan import.
    char systemDir[MAX_PATH];
    GetSystemDirectoryA(systemDir, MAX_PATH);
    char dxgiPath[MAX_PATH];
    snprintf(dxgiPath, MAX_PATH, "%s\\dxgi.dll", systemDir);
    char d3d11Path[MAX_PATH];
    snprintf(d3d11Path, MAX_PATH, "%s\\d3d11.dll", systemDir);

    HMODULE hDXGI = LoadLibraryA(dxgiPath);
    if (!hDXGI) {
        hDXGI = ce::security::LoadSystemLibrary(L"dxgi.dll");
    }

    HMODULE hD3D11 = LoadLibraryA(d3d11Path);
    if (!hD3D11) {
        // Fallback to default search (non-DXVK scenarios)
        hD3D11 = ce::security::LoadSystemLibrary(L"d3d11.dll");
    }
    if (!hD3D11)
        return false;

    if (hDXGI) {
        // Ensure imports inside the loaded d3d11 module resolve against the
        // system dxgi module rather than DXVK's already-loaded dxgi.dll.
        RedirectModuleImports(hD3D11, "dxgi.dll", hDXGI);
    } else {
        LayerLog("Vulkan Layer: [Warn] Failed to load system dxgi.dll for D3D11 interop import redirection");
    }

    char loadedD3D11Path[MAX_PATH] = {};
    char loadedDXGIPath[MAX_PATH] = {};
    GetModuleFileNameA(hD3D11, loadedD3D11Path, MAX_PATH);
    if (hDXGI) {
        GetModuleFileNameA(hDXGI, loadedDXGIPath, MAX_PATH);
    }
    LayerLog("Vulkan Layer: D3D11 interop modules: d3d11=%s dxgi=%s", loadedD3D11Path,
             hDXGI ? loadedDXGIPath : "<unavailable>");

    PFN_D3D11_CREATE_DEVICE createFn = (PFN_D3D11_CREATE_DEVICE)GetProcAddress(hD3D11, "D3D11CreateDevice");
    if (!createFn)
        return false;

    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0};
    D3D_FEATURE_LEVEL featureLevel;

    HRESULT hr = createFn(adapter, adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels,
                          1, D3D11_SDK_VERSION, ppDevice, &featureLevel, ppContext);

    return SUCCEEDED(hr);

}
uint64_t MakeLuidKey(const LUID& luid) {


    return (static_cast<uint64_t>(luid.HighPart) << 32) | static_cast<uint32_t>(luid.LowPart);

}
bool IsSpecificDxvkWrapperLoaded(const char* dllName) {


    if (!IsDllFromProject(dllName, "dxvk")) {
        return false;
    }
    return GetModuleHandleA(dllName) != nullptr;

}
VulkanCaptureInteropMode DetectVulkanInteropMode() {


    const bool dxvkD3D11 = IsSpecificDxvkWrapperLoaded("d3d11.dll");
    const bool dxvkD3D9 = IsSpecificDxvkWrapperLoaded("d3d9.dll");

    if (dxvkD3D11) {
        return VulkanCaptureInteropMode::kDxvkD3D11;
    }
    if (dxvkD3D9) {
        return VulkanCaptureInteropMode::kDxvkD3D9;
    }
    return VulkanCaptureInteropMode::kNative;

}
const char* VulkanInteropModeToString(VulkanCaptureInteropMode mode) {


    switch (mode) {
        case VulkanCaptureInteropMode::kNative:
            return "native";
        case VulkanCaptureInteropMode::kDxvkD3D11:
            return "dxvk-d3d11";
        case VulkanCaptureInteropMode::kDxvkD3D9:
            return "dxvk-d3d9";
    }
    return "unknown";

}
D3D11InteropDevice* GetOrCreateD3D11Device(const LUID& luid) {


    uint64_t key = MakeLuidKey(luid);

    for (auto it = layer_capture_g_D3D11Devices.begin(); it != layer_capture_g_D3D11Devices.end();) {
        if (it->luidKey == key) {
            if (it->valid)
                return &(*it);
            it = layer_capture_g_D3D11Devices.erase(it);
            continue;
        }
        ++it;
    }

    // Load the NATIVE dxgi.dll from System32 to bypass DXVK's replacement.
    // DXVK replaces both d3d11.dll and dxgi.dll; we need the real DXGI factory
    // to enumerate physical adapters and create real D3D11 shared textures.
    typedef HRESULT(WINAPI * PFN_CreateDXGIFactory1)(REFIID riid, void** ppFactory);
    PFN_CreateDXGIFactory1 nativeCreateFactory = nullptr;

    char systemDir[MAX_PATH];
    GetSystemDirectoryA(systemDir, MAX_PATH);
    char dxgiPath[MAX_PATH];
    snprintf(dxgiPath, MAX_PATH, "%s\\dxgi.dll", systemDir);

    HMODULE hDxgi = LoadLibraryA(dxgiPath);
    if (hDxgi) {
        nativeCreateFactory = (PFN_CreateDXGIFactory1)GetProcAddress(hDxgi, "CreateDXGIFactory1");
    }

    ComPtr<IDXGIFactory1> factory;
    HRESULT factoryHr = E_FAIL;
    if (nativeCreateFactory) {
        factoryHr = nativeCreateFactory(IID_PPV_ARGS(&factory));
    }
    if (FAILED(factoryHr)) {
        // Fallback to default (non-DXVK scenarios)
        factoryHr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    }
    if (FAILED(factoryHr))
        return nullptr;

    ComPtr<IDXGIAdapter> adapter;
    for (UINT i = 0; factory->EnumAdapters(i, &adapter) != DXGI_ERROR_NOT_FOUND; i++) {
        DXGI_ADAPTER_DESC desc;
        adapter->GetDesc(&desc);
        if (desc.AdapterLuid.LowPart == luid.LowPart && desc.AdapterLuid.HighPart == luid.HighPart) {
            LayerLog("Vulkan Layer: Interop adapter: '%ls' VendorId=%x DeviceId=%x LUID=%08x:%08x", desc.Description,
                     desc.VendorId, desc.DeviceId, desc.AdapterLuid.HighPart, desc.AdapterLuid.LowPart);
            ID3D11Device* device = nullptr;
            ID3D11DeviceContext* context = nullptr;
            if (CreateD3D11InteropDevice(adapter.Get(), &device, &context)) {
                D3D11InteropDevice newDev = {};
                newDev.luidKey = key;
                newDev.device = device;
                newDev.context = context;
                newDev.valid = true;
                layer_capture_g_D3D11Devices.push_back(newDev);
                return &layer_capture_g_D3D11Devices.back();
            }
            return nullptr;
        }
    }

    return nullptr;

}
uint32_t VkFormatToDXGI(VkFormat vkFormat) {


    switch (vkFormat) {
        case VK_FORMAT_B8G8R8A8_UNORM:
        case VK_FORMAT_B8G8R8A8_SRGB:
            // Map SRGB to UNORM: same byte layout, avoids SRGB shared-texture compatibility issues
            return DXGI_FORMAT_B8G8R8A8_UNORM;
        case VK_FORMAT_R8G8B8A8_UNORM:
        case VK_FORMAT_R8G8B8A8_SRGB:
            // Map SRGB to UNORM: same byte layout, avoids SRGB shared-texture compatibility issues
            return DXGI_FORMAT_R8G8B8A8_UNORM;
        case VK_FORMAT_R16G16B16A16_SFLOAT:
            return DXGI_FORMAT_R16G16B16A16_FLOAT;
        case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
            return DXGI_FORMAT_R10G10B10A2_UNORM;
        default:
            return DXGI_FORMAT_UNKNOWN;
    }

}
VkFormat NormalizeVkFormat(VkFormat fmt) {


    switch (fmt) {
        case VK_FORMAT_B8G8R8A8_SRGB:
            return VK_FORMAT_B8G8R8A8_UNORM;
        case VK_FORMAT_R8G8B8A8_SRGB:
            return VK_FORMAT_R8G8B8A8_UNORM;
        default:
            return fmt;
    }

}
