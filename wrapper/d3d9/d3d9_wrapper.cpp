#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <string>
#include <fstream>
#include <mutex>
#include <d3d9.h>
#include <initguid.h> 

#pragma comment(lib, "d3d9.lib")
#pragma comment(lib, "dxguid.lib")
#pragma comment(lib, "uuid.lib")

// --- Logging Helper ---
std::ofstream logFile;
void Log(const char* fmt, ...) {
    static std::mutex logMutex;
    std::lock_guard<std::mutex> lock(logMutex);
    
    if (!logFile.is_open()) {
        logFile.open("d3d9_wrapper.log", std::ios::app);
    }
    if (logFile.is_open()) {
        char buffer[1024];
        va_list args;
        va_start(args, fmt);
        vsnprintf(buffer, sizeof(buffer), fmt, args);
        va_end(args);
        logFile << buffer << std::endl;
        logFile.flush();
    }
}

// --- Global for Identity Hook ---
IDirect3D9* g_pProxyD3D9 = nullptr;

typedef HRESULT (STDMETHODCALLTYPE *GetDirect3D_t)(IDirect3DDevice9*, IDirect3D9**);
typedef HRESULT (STDMETHODCALLTYPE *QueryInterface_t)(IDirect3DDevice9*, REFIID, void**);

GetDirect3D_t Original_GetDirect3D = nullptr;
QueryInterface_t Original_QueryInterface = nullptr;

HRESULT STDMETHODCALLTYPE Hooked_GetDirect3D(IDirect3DDevice9* pDevice, IDirect3D9** ppD3D9) {
    Log("Hooked_GetDirect3D called on device %p", pDevice);
    if (!ppD3D9) return D3DERR_INVALIDCALL;
    
    if (g_pProxyD3D9) {
        *ppD3D9 = g_pProxyD3D9;
        g_pProxyD3D9->AddRef();
        Log("Hooked_GetDirect3D returning Proxy %p", g_pProxyD3D9);
        return S_OK;
    }
    
    if (Original_GetDirect3D) {
        Log("Hooked_GetDirect3D fallback to original");
        return Original_GetDirect3D(pDevice, ppD3D9);
    }
    return D3DERR_INVALIDCALL;
}

void PatchVTable(void* pObject, int methodIndex, void* pReplacement, void** ppOriginal);

HRESULT STDMETHODCALLTYPE Hooked_QueryInterface(IDirect3DDevice9* pDevice, REFIID riid, void** ppvObj) {
    HRESULT hr = Original_QueryInterface(pDevice, riid, ppvObj);
    
    if (SUCCEEDED(hr) && ppvObj && *ppvObj) {
        if (riid == IID_IDirect3DDevice9 || riid == IID_IUnknown) {
            if (*ppvObj != pDevice) {
                Log("Hooked_QueryInterface: IID_IDirect3DDevice9 returned different pointer! Old=%p New=%p. Patching new...", pDevice, *ppvObj);
                PatchVTable(*ppvObj, 6, (void*)Hooked_GetDirect3D, nullptr);
                PatchVTable(*ppvObj, 0, (void*)Hooked_QueryInterface, nullptr);
            }
        } else if (riid == __uuidof(IDirect3DDevice9Ex)) {
             Log("Hooked_QueryInterface: Queried IDirect3DDevice9Ex. Success.");
             if (*ppvObj != pDevice) {
                 Log("Hooked_QueryInterface: IDirect3DDevice9Ex returned different pointer! Patching...");
                 PatchVTable(*ppvObj, 6, (void*)Hooked_GetDirect3D, nullptr);
                 PatchVTable(*ppvObj, 0, (void*)Hooked_QueryInterface, nullptr);
             }
        }
    }
    return hr;
}

void PatchVTable(void* pObject, int methodIndex, void* pReplacement, void** ppOriginal) {
    if (!pObject) return;
    
    void** vtable = *(void***)pObject;
    
    if (vtable[methodIndex] == pReplacement) {
        return;
    }

    if (ppOriginal) *ppOriginal = vtable[methodIndex];
    
    DWORD oldProtect;
    VirtualProtect(&vtable[methodIndex], sizeof(void*), PAGE_READWRITE, &oldProtect);
    vtable[methodIndex] = pReplacement;
    VirtualProtect(&vtable[methodIndex], sizeof(void*), oldProtect, &oldProtect);
    
    Log("VTable Patch applied at index %d. Ptr=%p", methodIndex, &vtable[methodIndex]);
}


// --- Loading Logic ---
HMODULE hRealD3D9 = nullptr;
bool isLoading = false;

typedef IDirect3D9* (WINAPI *Direct3DCreate9_t)(UINT);
typedef HRESULT     (WINAPI *Direct3DCreate9Ex_t)(UINT, IDirect3D9Ex**);

Direct3DCreate9_t               RealDirect3DCreate9 = nullptr;
Direct3DCreate9Ex_t             RealDirect3DCreate9Ex = nullptr;

// Passthrough exports
extern "C" {
    int WINAPI D3DPERF_BeginEvent(D3DCOLOR col, LPCWSTR wszName);
    int WINAPI D3DPERF_EndEvent(void);
    void WINAPI D3DPERF_SetMarker(D3DCOLOR col, LPCWSTR wszName);
    void WINAPI D3DPERF_SetRegion(D3DCOLOR col, LPCWSTR wszName);
    BOOL WINAPI D3DPERF_QueryRepeatFrame(void);
    void WINAPI D3DPERF_SetOptions(DWORD dwOptions);
    DWORD WINAPI D3DPERF_GetStatus(void);
    void WINAPI DebugSetMute(void);
    int WINAPI Direct3DShaderValidatorCreate9(void);
    void WINAPI PSGP_Bernstein(void);
    void WINAPI PSGP_Beta(void);
}

typedef int (WINAPI *D3DPERF_BeginEvent_t)(D3DCOLOR, LPCWSTR);
typedef int (WINAPI *D3DPERF_EndEvent_t)(void);
typedef void (WINAPI *D3DPERF_SetMarker_t)(D3DCOLOR, LPCWSTR);
typedef void (WINAPI *D3DPERF_SetRegion_t)(D3DCOLOR, LPCWSTR);
typedef BOOL (WINAPI *D3DPERF_QueryRepeatFrame_t)(void);
typedef void (WINAPI *D3DPERF_SetOptions_t)(DWORD);
typedef DWORD (WINAPI *D3DPERF_GetStatus_t)(void);
typedef void (WINAPI *DebugSetMute_t)(void);
typedef int (WINAPI *Direct3DShaderValidatorCreate9_t)(void);
typedef void (WINAPI *PSGP_Bernstein_t)(void);
typedef void (WINAPI *PSGP_Beta_t)(void);

int WINAPI D3DPERF_BeginEvent(D3DCOLOR col, LPCWSTR wszName) { if(!hRealD3D9) return 0; auto p = (D3DPERF_BeginEvent_t)GetProcAddress(hRealD3D9, "D3DPERF_BeginEvent"); return p ? p(col, wszName) : 0; }
int WINAPI D3DPERF_EndEvent(void) { if(!hRealD3D9) return 0; auto p = (D3DPERF_EndEvent_t)GetProcAddress(hRealD3D9, "D3DPERF_EndEvent"); return p ? p() : 0; }
void WINAPI D3DPERF_SetMarker(D3DCOLOR col, LPCWSTR wszName) { if(!hRealD3D9) return; auto p = (D3DPERF_SetMarker_t)GetProcAddress(hRealD3D9, "D3DPERF_SetMarker"); if(p) p(col, wszName); }
void WINAPI D3DPERF_SetRegion(D3DCOLOR col, LPCWSTR wszName) { if(!hRealD3D9) return; auto p = (D3DPERF_SetRegion_t)GetProcAddress(hRealD3D9, "D3DPERF_SetRegion"); if(p) p(col, wszName); }
BOOL WINAPI D3DPERF_QueryRepeatFrame(void) { if(!hRealD3D9) return FALSE; auto p = (D3DPERF_QueryRepeatFrame_t)GetProcAddress(hRealD3D9, "D3DPERF_QueryRepeatFrame"); return p ? p() : FALSE; }
void WINAPI D3DPERF_SetOptions(DWORD dwOptions) { if(!hRealD3D9) return; auto p = (D3DPERF_SetOptions_t)GetProcAddress(hRealD3D9, "D3DPERF_SetOptions"); if(p) p(dwOptions); }
DWORD WINAPI D3DPERF_GetStatus(void) { if(!hRealD3D9) return 0; auto p = (D3DPERF_GetStatus_t)GetProcAddress(hRealD3D9, "D3DPERF_GetStatus"); return p ? p() : 0; }
void WINAPI DebugSetMute(void) { if(!hRealD3D9) return; auto p = (DebugSetMute_t)GetProcAddress(hRealD3D9, "DebugSetMute"); if(p) p(); }
int WINAPI Direct3DShaderValidatorCreate9(void) { if(!hRealD3D9) return 0; auto p = (Direct3DShaderValidatorCreate9_t)GetProcAddress(hRealD3D9, "Direct3DShaderValidatorCreate9"); return p ? p() : 0; }
void WINAPI PSGP_Bernstein(void) { if(!hRealD3D9) return; auto p = (PSGP_Bernstein_t)GetProcAddress(hRealD3D9, "PSGP_Bernstein"); if(p) p(); }
void WINAPI PSGP_Beta(void) { if(!hRealD3D9) return; auto p = (PSGP_Beta_t)GetProcAddress(hRealD3D9, "PSGP_Beta"); if(p) p(); }

void LoadRealD3D9() {
    if (hRealD3D9) return;
    if (isLoading) return;
    isLoading = true;

    Log("Loading real d3d9.dll (Forced Copy Strategy)...");
    
    char sysDir[MAX_PATH];
    GetSystemDirectoryA(sysDir, MAX_PATH);
    std::string sysD3D9Path = std::string(sysDir) + "\\d3d9.dll";
    
    HMODULE hOurModule = GetModuleHandleA("d3d9.dll");
    char wrapperPath[MAX_PATH];
    if (hOurModule) GetModuleFileNameA(hOurModule, wrapperPath, MAX_PATH);
    else GetCurrentDirectoryA(MAX_PATH, wrapperPath);
    
    std::string wrapperDir = std::string(wrapperPath);
    size_t lastSlash = wrapperDir.find_last_of("\\/");
    if (lastSlash != std::string::npos) wrapperDir = wrapperDir.substr(0, lastSlash + 1);
    
    std::string tempPath = wrapperDir + "d3d9_system.dll";
    
    CopyFileA(sysD3D9Path.c_str(), tempPath.c_str(), FALSE);
    hRealD3D9 = LoadLibraryA(tempPath.c_str());
    
    if (!hRealD3D9) hRealD3D9 = LoadLibraryA(sysD3D9Path.c_str());

    if (hRealD3D9) {
        RealDirect3DCreate9 = (Direct3DCreate9_t)GetProcAddress(hRealD3D9, "Direct3DCreate9");
        RealDirect3DCreate9Ex = (Direct3DCreate9Ex_t)GetProcAddress(hRealD3D9, "Direct3DCreate9Ex");
    }
    
    isLoading = false;
}

// --- ProxyDirect3D9 ---

class ProxyDirect3D9 : public IDirect3D9 {
private:
    IDirect3D9Ex* m_pReal;
    LONG m_RefCount;

public:
    ProxyDirect3D9(IDirect3D9Ex* pReal) : m_pReal(pReal), m_RefCount(1) {
        Log("ProxyDirect3D9 created wrapping %p", pReal);
        g_pProxyD3D9 = this;
    }

    STDMETHOD(QueryInterface)(REFIID riid, void** ppvObj) {
        if (!ppvObj) return E_POINTER;
        if (riid == IID_IDirect3D9 || riid == IID_IUnknown) {
            *ppvObj = this;
            AddRef();
            return S_OK;
        }
        if (riid == __uuidof(IDirect3D9Ex)) {
            Log("Proxy: Blocked QueryInterface for IDirect3D9Ex");
            *ppvObj = nullptr;
            return E_NOINTERFACE;
        }
        return m_pReal->QueryInterface(riid, ppvObj);
    }
    STDMETHOD_(ULONG, AddRef)() { 
        LONG r = InterlockedIncrement(&m_RefCount);
        Log("ProxyD3D9 AddRef: %d", r);
        return r;
    }
    STDMETHOD_(ULONG, Release)() {
        ULONG count = InterlockedDecrement(&m_RefCount);
        Log("ProxyD3D9 Release: %d", count);
        if (count == 0) {
            Log("ProxyDirect3D9 deleted");
            m_pReal->Release();
            if (g_pProxyD3D9 == this) g_pProxyD3D9 = nullptr;
            delete this;
        }
        return count;
    }

    // IDirect3D9 - Forwarding
    STDMETHOD(RegisterSoftwareDevice)(void* pInitializeFunction) { return m_pReal->RegisterSoftwareDevice(pInitializeFunction); }
    STDMETHOD_(UINT, GetAdapterCount)() { return m_pReal->GetAdapterCount(); }
    STDMETHOD(GetAdapterIdentifier)(UINT Adapter, DWORD Flags, D3DADAPTER_IDENTIFIER9* pIdentifier) { return m_pReal->GetAdapterIdentifier(Adapter, Flags, pIdentifier); }
    STDMETHOD_(UINT, GetAdapterModeCount)(UINT Adapter, D3DFORMAT Format) { return m_pReal->GetAdapterModeCount(Adapter, Format); }
    STDMETHOD(EnumAdapterModes)(UINT Adapter, D3DFORMAT Format, UINT Mode, D3DDISPLAYMODE* pMode) { return m_pReal->EnumAdapterModes(Adapter, Format, Mode, pMode); }
    STDMETHOD(GetAdapterDisplayMode)(UINT Adapter, D3DDISPLAYMODE* pMode) { return m_pReal->GetAdapterDisplayMode(Adapter, pMode); }
    STDMETHOD(CheckDeviceType)(UINT Adapter, D3DDEVTYPE DevType, D3DFORMAT AdapterFormat, D3DFORMAT BackBufferFormat, BOOL bWindowed) { return m_pReal->CheckDeviceType(Adapter, DevType, AdapterFormat, BackBufferFormat, bWindowed); }
    STDMETHOD(CheckDeviceFormat)(UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT AdapterFormat, DWORD Usage, D3DRESOURCETYPE RType, D3DFORMAT CheckFormat) { return m_pReal->CheckDeviceFormat(Adapter, DeviceType, AdapterFormat, Usage, RType, CheckFormat); }
    STDMETHOD(CheckDeviceMultiSampleType)(UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT SurfaceFormat, BOOL Windowed, D3DMULTISAMPLE_TYPE MultiSampleType, DWORD* pQualityLevels) { return m_pReal->CheckDeviceMultiSampleType(Adapter, DeviceType, SurfaceFormat, Windowed, MultiSampleType, pQualityLevels); }
    STDMETHOD(CheckDepthStencilMatch)(UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT AdapterFormat, D3DFORMAT RenderTargetFormat, D3DFORMAT DepthStencilFormat) { return m_pReal->CheckDepthStencilMatch(Adapter, DeviceType, AdapterFormat, RenderTargetFormat, DepthStencilFormat); }
    STDMETHOD(CheckDeviceFormatConversion)(UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT SourceFormat, D3DFORMAT TargetFormat) { return m_pReal->CheckDeviceFormatConversion(Adapter, DeviceType, SourceFormat, TargetFormat); }
    STDMETHOD(GetDeviceCaps)(UINT Adapter, D3DDEVTYPE DeviceType, D3DCAPS9* pCaps) { return m_pReal->GetDeviceCaps(Adapter, DeviceType, pCaps); }
    STDMETHOD_(HMONITOR, GetAdapterMonitor)(UINT Adapter) { return m_pReal->GetAdapterMonitor(Adapter); }

    STDMETHOD(CreateDevice)(UINT Adapter, D3DDEVTYPE DeviceType, HWND hFocusWindow, DWORD BehaviorFlags, D3DPRESENT_PARAMETERS* pPresentationParameters, IDirect3DDevice9** ppReturnedDeviceInterface) {
        Log("Proxy: CreateDevice called (Window=%p, Flags=%x)", hFocusWindow, BehaviorFlags);
        
        D3DDISPLAYMODEEX modeEx = { sizeof(D3DDISPLAYMODEEX) };
        D3DDISPLAYMODE currentMode;
        HRESULT hrMode = m_pReal->GetAdapterDisplayMode(Adapter, &currentMode);
        if (SUCCEEDED(hrMode)) {
            modeEx.Width = currentMode.Width;
            modeEx.Height = currentMode.Height;
            modeEx.RefreshRate = currentMode.RefreshRate;
            modeEx.Format = currentMode.Format;
            modeEx.ScanLineOrdering = D3DSCANLINEORDERING_PROGRESSIVE;
        }
        
        if (pPresentationParameters->BackBufferWidth > 0) modeEx.Width = pPresentationParameters->BackBufferWidth;
        if (pPresentationParameters->BackBufferHeight > 0) modeEx.Height = pPresentationParameters->BackBufferHeight;
        if (pPresentationParameters->FullScreen_RefreshRateInHz > 0) modeEx.RefreshRate = pPresentationParameters->FullScreen_RefreshRateInHz;
        
        D3DDISPLAYMODEEX* pModeEx = nullptr;
        if (!pPresentationParameters->Windowed) pModeEx = &modeEx;

        IDirect3DDevice9Ex* pDeviceEx = nullptr;
        HRESULT hr = m_pReal->CreateDeviceEx(Adapter, DeviceType, hFocusWindow, BehaviorFlags, pPresentationParameters, pModeEx, &pDeviceEx);
        
        if (SUCCEEDED(hr) && pDeviceEx) {
            Log("Proxy: CreateDeviceEx promotion successful! Real Device=%p", pDeviceEx);
            
            PatchVTable(pDeviceEx, 6, (void*)Hooked_GetDirect3D, (void**)&Original_GetDirect3D);
            PatchVTable(pDeviceEx, 0, (void*)Hooked_QueryInterface, (void**)&Original_QueryInterface);

            this->AddRef();
            *ppReturnedDeviceInterface = pDeviceEx; 
            return S_OK;
        } else {
            Log("Proxy: CreateDeviceEx failed (hr=0x%08x). Fallback.", hr);
            return m_pReal->CreateDevice(Adapter, DeviceType, hFocusWindow, BehaviorFlags, pPresentationParameters, ppReturnedDeviceInterface);
        }
    }
};


// --- Exports ---

extern "C" {

IDirect3D9* WINAPI Direct3DCreate9(UINT SDKVersion) {
    Log("Direct3DCreate9 called (SDKVersion=%u)", SDKVersion);
    LoadRealD3D9();
    
    // Attempt Ex promotion logic
    if (RealDirect3DCreate9Ex) {
        IDirect3D9Ex* pRealEx = nullptr;
        HRESULT hr = RealDirect3DCreate9Ex(SDKVersion, &pRealEx);
        if (SUCCEEDED(hr) && pRealEx) {
             Log("Direct3DCreate9Ex created simple real object %p. Wrapping in Proxy...", pRealEx);
             return new ProxyDirect3D9(pRealEx);
        }
    }
    
    if (RealDirect3DCreate9) return RealDirect3DCreate9(SDKVersion);
    return nullptr;
}

HRESULT WINAPI Direct3DCreate9Ex(UINT SDKVersion, IDirect3D9Ex** ppD3D) {
    Log("Direct3DCreate9Ex called (Passthrough)");
    LoadRealD3D9();
    if (RealDirect3DCreate9Ex) return RealDirect3DCreate9Ex(SDKVersion, ppD3D);
    return E_FAIL;
}

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved) {
    if (fdwReason == DLL_PROCESS_DETACH) {
        if (hRealD3D9) FreeLibrary(hRealD3D9);
        if (logFile.is_open()) logFile.close();
    }
    return TRUE;
}

}
