#include "dx11_hook_internal.h"


bool DX11Capture::InitDX10(IDXGISwapChain* swapChain) {


        IDXGIDevice* dxgiDevice = nullptr;
        IDXGIAdapter* adapter = nullptr;
        ID3D10Device* device10 = nullptr;

        if (FAILED(swapChain->GetDevice(__uuidof(ID3D10Device), (void**)&device10))) {
            ID3D10Device1* device10_1 = nullptr;
            if (FAILED(swapChain->GetDevice(__uuidof(ID3D10Device1), (void**)&device10_1)) || !device10_1) {
                HookLog("DX10: Failed to get device from swapchain");
                return false;
            }
            device10 = device10_1;
        }

        if (!device10) {
            HookLog("DX10: Swapchain returned null device");
            return false;
        }

        cachedDevice10 = device10;  // Keep GetDevice() reference until Cleanup()

        if (SUCCEEDED(device10->QueryInterface(IID_PPV_ARGS(&dxgiDevice)))) {
            if (SUCCEEDED(dxgiDevice->GetAdapter(&adapter))) {
                DXGI_ADAPTER_DESC adapterDesc;
                if (SUCCEEDED(adapter->GetDesc(&adapterDesc))) {
                    // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
                    luidLow = adapterDesc.AdapterLuid.LowPart;
                    luidHigh = adapterDesc.AdapterLuid.HighPart;

                    SystemMetricsCollector::Get().Initialize(luidLow, luidHigh);
                    SystemMetricsCollector::Get().SetVRAMTotal(adapterDesc.DedicatedVideoMemory);
                }
                adapter->Release();
            }
            dxgiDevice->Release();
        }

        isDX10Mode = true;
        return true;

}
bool DX11Capture::IsCurrentD3D11DXVK() {


        HMODULE hD3D11 = GetModuleHandleA("d3d11.dll");
        if (!hD3D11)
            return false;
        char path[MAX_PATH] = {};
        if (!GetModuleFileNameA(hD3D11, path, MAX_PATH))
            return false;
        char sysDir[MAX_PATH] = {};
        if (!GetSystemDirectoryA(sysDir, MAX_PATH))
            return false;
        uint32_t sysLen = (uint32_t)strlen(sysDir);
        return !(_strnicmp(path, sysDir, sysLen) == 0 && path[sysLen] == '\\');

}
bool DX11Capture::CreateSystemD3D11DeviceForLUID(int32_t luidLowPart,  int32_t luidHighPart) {


        char systemDir[MAX_PATH] = {};
        if (GetSystemDirectoryA(systemDir, MAX_PATH) == 0)
            return false;
        std::string dxgiPath = std::string(systemDir) + "\\dxgi.dll";
        std::string d3d11Path = std::string(systemDir) + "\\d3d11.dll";

        HMODULE hDXGI = LoadLibraryA(dxgiPath.c_str());
        if (!hDXGI) {
            EarlyLog("DX11-DXVK: System DXGI not found");
            return false;
        }
        typedef HRESULT(WINAPI * PFN_CREATE_DXGI_FACTORY1)(REFIID, void**);
        PFN_CREATE_DXGI_FACTORY1 pCreateDXGIFactory1 =
            (PFN_CREATE_DXGI_FACTORY1)GetProcAddress(hDXGI, "CreateDXGIFactory1");
        if (!pCreateDXGIFactory1)
            return false;

        IDXGIFactory1* factory = nullptr;
        if (FAILED(pCreateDXGIFactory1(IID_PPV_ARGS(&factory))))
            return false;

        // Find the real adapter by LUID
        IDXGIAdapter1* adapter = nullptr;
        IDXGIAdapter1* matchedAdapter = nullptr;
        for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; i++) {
            DXGI_ADAPTER_DESC1 desc;
            adapter->GetDesc1(&desc);
            if (desc.AdapterLuid.LowPart == (DWORD)luidLowPart && desc.AdapterLuid.HighPart == luidHighPart) {
                matchedAdapter = adapter;
                break;
            }
            adapter->Release();
        }
        factory->Release();

        if (!matchedAdapter) {
            EarlyLog("DX11-DXVK: No system DXGI adapter found for LUID");
            return false;
        }

        HMODULE hD3D11 = LoadLibraryA(d3d11Path.c_str());
        if (!hD3D11) {
            matchedAdapter->Release();
            EarlyLog("DX11-DXVK: System D3D11 not found");
            return false;
        }
        // Redirect system d3d11.dll's dxgi.dll IAT to system dxgi.dll
        RedirectModuleImports(hD3D11, "dxgi.dll", hDXGI);

        typedef HRESULT(WINAPI * PFN_D3D11_CREATE_DEVICE)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT,
                                                          const D3D_FEATURE_LEVEL*, UINT, UINT, ID3D11Device**,
                                                          D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
        PFN_D3D11_CREATE_DEVICE pD3D11CreateDevice =
            (PFN_D3D11_CREATE_DEVICE)GetProcAddress(hD3D11, "D3D11CreateDevice");
        if (!pD3D11CreateDevice) {
            matchedAdapter->Release();
            return false;
        }

        D3D_FEATURE_LEVEL featureLevels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1};
        D3D_FEATURE_LEVEL featureLevel;
        HRESULT hr = pD3D11CreateDevice(matchedAdapter, D3D_DRIVER_TYPE_UNKNOWN, NULL, 0, featureLevels, 2,
                                        D3D11_SDK_VERSION, &ownedDevice, &featureLevel, &ownedContext);
        matchedAdapter->Release();
        if (FAILED(hr)) {
            EarlyLog("DX11-DXVK: Failed to create system D3D11 device (hr=0x%08x)", hr);
            return false;
        }
        EarlyLog("DX11-DXVK: System D3D11 device created for cross-device capture (fl=%d)", featureLevel);
        return true;

}
void DX11Capture::Init(ID3D11Device* device,  IDXGISwapChain* swapChain) {


        if (initialized)
            return;

        if (!cachedSwapChainIdentity) {
            const HRESULT identityHr =
                swapChain ? swapChain->QueryInterface(IID_PPV_ARGS(&cachedSwapChainIdentity)) : E_POINTER;
            if (FAILED(identityHr) || !cachedSwapChainIdentity) {
                EarlyLog("%s Capture Init: failed to retain swapchain identity hr=0x%08X", isDX10Mode ? "DX10" : "DX11",
                         identityHr);
                Cleanup();
                return;
            }
        }

        DXGI_SWAP_CHAIN_DESC desc = {};
        const HRESULT descHr = swapChain ? swapChain->GetDesc(&desc) : E_POINTER;
        if (SUCCEEDED(descHr) && (desc.BufferDesc.Width == 0 || desc.BufferDesc.Height == 0 ||
                                  desc.BufferDesc.Format == DXGI_FORMAT_UNKNOWN)) {
            // Width/height may have been HWND-derived at creation. Resolve the
            // concrete values from the actual buffer before rejecting capture.
            if (isDX10Mode) {
                ID3D10Texture2D* buffer = nullptr;
                if (SUCCEEDED(swapChain->GetBuffer(0, IID_PPV_ARGS(&buffer))) && buffer) {
                    D3D10_TEXTURE2D_DESC bufferDesc = {};
                    buffer->GetDesc(&bufferDesc);
                    desc.BufferDesc.Width = bufferDesc.Width;
                    desc.BufferDesc.Height = bufferDesc.Height;
                    desc.BufferDesc.Format = bufferDesc.Format;
                    buffer->Release();
                }
            } else {
                ID3D11Texture2D* buffer = nullptr;
                if (SUCCEEDED(swapChain->GetBuffer(0, IID_PPV_ARGS(&buffer))) && buffer) {
                    D3D11_TEXTURE2D_DESC bufferDesc = {};
                    buffer->GetDesc(&bufferDesc);
                    desc.BufferDesc.Width = bufferDesc.Width;
                    desc.BufferDesc.Height = bufferDesc.Height;
                    desc.BufferDesc.Format = bufferDesc.Format;
                    buffer->Release();
                }
            }
        }
        if (FAILED(descHr) || desc.BufferDesc.Width == 0 || desc.BufferDesc.Height == 0 ||
            desc.BufferDesc.Format == DXGI_FORMAT_UNKNOWN) {
            EarlyLog("%s Capture Init: invalid swapchain description hr=0x%08X size=%ux%u format=%d",
                     isDX10Mode ? "DX10" : "DX11", descHr, desc.BufferDesc.Width, desc.BufferDesc.Height,
                     static_cast<int>(desc.BufferDesc.Format));
            Cleanup();
            return;
        }

        width = desc.BufferDesc.Width;
        height = desc.BufferDesc.Height;
        format = desc.BufferDesc.Format;

        // Determine which device to use for creating textures
        ID3D11Device* captureDevice = device;

        if (isDX10Mode) {
            if (!cachedDevice10) {
                EarlyLog("DX10: Missing cached D3D10 device during capture init");
                return;
            }

            D3D10_TEXTURE2D_DESC texDesc10 = {};
            texDesc10.Width = width;
            texDesc10.Height = height;
            texDesc10.MipLevels = 1;
            texDesc10.ArraySize = 1;
            texDesc10.Format = (DXGI_FORMAT)format;
            texDesc10.SampleDesc.Count = 1;
            texDesc10.Usage = D3D10_USAGE_DEFAULT;
            texDesc10.BindFlags = D3D10_BIND_RENDER_TARGET | D3D10_BIND_SHADER_RESOURCE;
            texDesc10.CPUAccessFlags = 0;
            texDesc10.MiscFlags = D3D10_RESOURCE_MISC_SHARED;

            useFences = false;
            sharedFenceHandle.store(NULL, std::memory_order_release);

            bool success = true;
            for (int i = 0; i < CAPTURE_TEXTURE_COUNT; i++) {
                HRESULT hr = cachedDevice10->CreateTexture2D(&texDesc10, NULL, &sharedTextures10[i]);
                if (SUCCEEDED(hr) && sharedTextures10[i]) {
                    IDXGIResource* resource = nullptr;
                    if (SUCCEEDED(sharedTextures10[i]->QueryInterface(IID_PPV_ARGS(&resource))) && resource) {
                        HANDLE hTemp = NULL;
                        hr = resource->GetSharedHandle(&hTemp);
                        sharedTextureHandles[i].store(hTemp, std::memory_order_release);
                        sharedTextureHandlesAreNt[i] = false;
                        resource->Release();
                    } else {
                        EarlyLog("DX10: Error - Failed to query IDXGIResource for texture %d", i);
                        success = false;
                    }

                    if (sharedTextureHandles[i].load(std::memory_order_acquire) == NULL) {
                        EarlyLog("DX10: Critical - Shared handle is NULL for texture %d", i);
                        success = false;
                    }
                } else {
                    success = false;
                    HookLog("DX10: Failed to create shared texture %d (hr=0x%08x)", i, hr);
                }
            }

            if (success) {
                D3D10_QUERY_DESC queryDesc = {};
                queryDesc.Query = D3D10_QUERY_EVENT;
                queryDesc.MiscFlags = 0;

                for (int i = 0; i < CAPTURE_TEXTURE_COUNT; i++) {
                    HRESULT queryHr = cachedDevice10->CreateQuery(&queryDesc, &copyQueries10[i]);
                    if (FAILED(queryHr)) {
                        HookLog("DX10: Failed to create copy query %d (hr=0x%08x)", i, queryHr);
                        copyQueries10[i] = nullptr;
                    }
                }
            }

            if (success) {
                if (g_IPC) {
                    PublishToSharedMemory(g_IPC);
                }
                initialized = true;
                HookLogImportant("DX10 Capture Initialized: %dx%d (Fence: OFF, Queries: %s, DXVK: OFF)", width, height,
                                 (copyQueries10[0] != nullptr) ? "ON" : "OFF");
            } else {
                EarlyLog("DX10 Capture Init FAILED (success=false)");
                Cleanup();
            }
            return;
        } else {
            // Get LUID from DX11 device
            IDXGIDevice* dxgiDevice = nullptr;
            if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&dxgiDevice)))) {
                IDXGIAdapter* adapter = nullptr;
                if (SUCCEEDED(dxgiDevice->GetAdapter(&adapter))) {
                    DXGI_ADAPTER_DESC adapterDesc;
                    adapter->GetDesc(&adapterDesc);
                    // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
                    luidLow = adapterDesc.AdapterLuid.LowPart;
                    luidHigh = adapterDesc.AdapterLuid.HighPart;

                    // Initialize SystemMetricsCollector with adapter LUID for GPU stats
                    SystemMetricsCollector::Get().Initialize(luidLow, luidHigh);

                    adapter->Release();
                }
                dxgiDevice->Release();
            }
            cachedDevice = device;
            device->GetImmediateContext(&cachedContext);

            // Detect DXVK: if d3d11.dll is not from System32, this is DXVK.
            // DXVK's GetSharedHandle returns Vulkan-internal IDs, not valid Windows
            // NT handles. Fix: create a real system D3D11 device on the same GPU
            // for ring buffer textures, then import them into DXVK for CopyResource.
            if (IsCurrentD3D11DXVK() && !isDXVKMode) {
                EarlyLog("DX11: DXVK d3d11 detected - creating system D3D11 for ring buffer");
                if (CreateSystemD3D11DeviceForLUID(luidLow, luidHigh)) {
                    isDXVKMode = true;
                    captureDevice = ownedDevice;  // Ring buffer textures come from real D3D11
                    // cachedDevice/cachedContext remain as DXVK's for CopyResource
                } else {
                    EarlyLog("DX11: DXVK detected but system D3D11 creation failed, capture may produce bad handles");
                }
            }
        }

        D3D11_TEXTURE2D_DESC texDesc = {};
        texDesc.Width = width;
        texDesc.Height = height;
        texDesc.MipLevels = 1;
        texDesc.ArraySize = 1;
        texDesc.Format = (DXGI_FORMAT)format;
        texDesc.SampleDesc.Count = 1;
        texDesc.Usage = D3D11_USAGE_DEFAULT;
        texDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        texDesc.CPUAccessFlags = 0;

        // Use plain shared textures with NT Handles for cross-process sharing
        // Synchronization is handled via D3D11 Fence (DX11.3+) or no sync fallback
        texDesc.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
        useKeyedMutex = false;  // Disabled - using Fence instead

        // Try to create D3D11 Fence for async GPU synchronization (DX11.3+)
        // Skip for DXVK: fence lives in system D3D11 device but copy happens via
        // DXVK context - fence can't be signaled cross-device from DXVK.
        ID3D11Device5* device5 = nullptr;
        D3D11InternalIdentityProbeScope identityProbeScope;
        if (!isDXVKMode && SUCCEEDED(captureDevice->QueryInterface(IID_PPV_ARGS(&device5)))) {
            HRESULT hr = device5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&fence));
            if (SUCCEEDED(hr)) {
                // Get shared fence handle for cross-process
                HANDLE hTemp = NULL;
                hr = fence->CreateSharedHandle(NULL, GENERIC_ALL, NULL, &hTemp);
                sharedFenceHandle.store(hTemp, std::memory_order_release);
                if (SUCCEEDED(hr)) {
                    // Get Context4 for Signal()
                    ID3D11DeviceContext* immCtx = nullptr;
                    captureDevice->GetImmediateContext(&immCtx);
                    if (immCtx && SUCCEEDED(immCtx->QueryInterface(IID_PPV_ARGS(&context4)))) {
                        useFences = true;
                        EarlyLog("DX11: D3D11 Fence created (async GPU sync enabled)");
                    } else {
                        EarlyLog("DX11: Warning - ID3D11DeviceContext4 not available; using implicit shared sync");
                        HANDLE unusableFenceHandle = sharedFenceHandle.exchange(NULL, std::memory_order_acq_rel);
                        if (unusableFenceHandle)
                            CloseHandle(unusableFenceHandle);
                        fence->Release();
                        fence = nullptr;
                    }
                    if (immCtx)
                        immCtx->Release();
                } else {
                    EarlyLog("DX11: Warning - Fence shared handle creation failed");
                    fence->Release();
                    fence = nullptr;

                }
            } else {
                EarlyLog("DX11: Warning - Fence creation failed (hr=0x%08x)", hr);
            }
            device5->Release();
        } else {
            EarlyLog("DX11: ID3D11Device5 not available (DX11.3 required for Fences)");
        }

        bool success = true;
        for (int i = 0; i < CAPTURE_TEXTURE_COUNT; i++) {
            HRESULT hr = captureDevice->CreateTexture2D(&texDesc, NULL, &sharedTextures[i]);
            if (SUCCEEDED(hr)) {
                IDXGIResource1* pResource1 = NULL;
                // Use IDXGIResource1 for NT Handles
                if (SUCCEEDED(sharedTextures[i]->QueryInterface(IID_PPV_ARGS(&pResource1)))) {
                    HANDLE hTemp = NULL;
                    hr = pResource1->CreateSharedHandle(NULL, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
                                                        NULL, &hTemp);
                    sharedTextureHandles[i].store(hTemp, std::memory_order_release);
                    sharedTextureHandlesAreNt[i] = SUCCEEDED(hr) && hTemp != NULL;
                    pResource1->Release();
                } else {
                    // Fallback to legacy KMT if Resource1 not valid (should not happen
                    // with NTHANDLE flag) But if we requested NTHANDLE, GetSharedHandle
                    // (KMT) will fail on some drivers. We should log this specific
                    // failure path.
                    IDXGIResource* pResource = NULL;
                    if (SUCCEEDED(sharedTextures[i]->QueryInterface(IID_PPV_ARGS(&pResource))) &&
                        (texDesc.MiscFlags & D3D11_RESOURCE_MISC_SHARED)) {
                        HANDLE hTemp = NULL;
                        pResource->GetSharedHandle(&hTemp);
                        sharedTextureHandles[i].store(hTemp, std::memory_order_release);
                        sharedTextureHandlesAreNt[i] = false;
                        pResource->Release();
                        EarlyLog(
                            "DX11: Warning - Fallback to KMT handle for KeyedMutex "
                            "(NT Handle QI failed)");
                    } else {
                        EarlyLog(
                            "DX11: Error - Failed to get any shared handle interface "
                            "for texture %d",
                            i);
                    }
                }

                if (sharedTextureHandles[i].load() == NULL) {
                    EarlyLog("DX11: Critical - Shared Handle is NULL for texture %d", i);
                    success = false;
                } else {
                    EarlyLog("DX11: Created Texture %d Handle %p", i, sharedTextureHandles[i].load());
                }

            } else {
                // Fallback to legacy shared if NT Handle not supported
                if (hr == E_INVALIDARG && (texDesc.MiscFlags & D3D11_RESOURCE_MISC_SHARED_NTHANDLE)) {
                    EarlyLog(
                        "DX11: NT Handle not supported, falling back to legacy "
                        "shared textures");
                    texDesc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
                    i--;  // Retry this index
                    continue;
                }

                success = false;
                HookLog("%s: Failed to create texture %d (hr=0x%08x)", isDX10Mode ? "DX10" : "DX11", i, hr);
            }
        }

        if (success) {
            // For DXVK mode: open each system D3D11 texture in DXVK's device.
            // The copy at capture time will use DXVK's context to copy the game
            // backbuffer into the DXVK-imported texture. The encoder opens the
            // original system D3D11 NT handles normally.
            if (isDXVKMode) {
                ID3D11Device1* dxvkDevice1 = nullptr;
                D3D11InternalIdentityProbeScope identityProbeScope;
                if (SUCCEEDED(cachedDevice->QueryInterface(IID_PPV_ARGS(&dxvkDevice1)))) {
                    for (int i = 0; i < CAPTURE_TEXTURE_COUNT && success; i++) {
                        HANDLE ntHandle = sharedTextureHandles[i].load();
                        if (!ntHandle) {
                            EarlyLog("DX11-DXVK: NT handle %d is NULL, cannot import", i);
                            success = false;
                            break;
                        }
                        HRESULT hr = dxvkDevice1->OpenSharedResource1(ntHandle, IID_PPV_ARGS(&dxvkImportedTextures[i]));
                        if (FAILED(hr)) {
                            EarlyLog("DX11-DXVK: Failed to open texture %d in DXVK device (hr=0x%08x)", i, hr);
                            success = false;
                        }
                    }
                    dxvkDevice1->Release();
                } else {
                    EarlyLog("DX11-DXVK: DXVK device doesn't support ID3D11Device1 - disabling DXVK mode");
                    success = false;
                }
            }
        }

        if (success) {
            // Create GPU synchronization queries for each texture
            // These queries are used to ensure CopyResource completes before reusing
            // the texture. Skip for DXVK: queries live in system D3D11 but context
            // is DXVK's - they can't be used cross-device.
            if (!isDXVKMode) {
                D3D11_QUERY_DESC queryDesc = {};
                queryDesc.Query = D3D11_QUERY_EVENT;
                queryDesc.MiscFlags = 0;

                for (int i = 0; i < CAPTURE_TEXTURE_COUNT; i++) {
                    HRESULT queryHr = captureDevice->CreateQuery(&queryDesc, &copyQueries[i]);
                    if (FAILED(queryHr)) {
                        HookLog("%s: Failed to create copy query %d (hr=0x%08x)", isDX10Mode ? "DX10" : "DX11", i,
                                queryHr);
                        copyQueries[i] = nullptr;
                    }
                }
            }

            if (g_IPC) {
                PublishToSharedMemory(g_IPC);
            }
            initialized = true;
            HookLogImportant("%s Capture Initialized: %dx%d (Fence: %s, Queries: %s, DXVK: %s)",
                             isDX10Mode ? "DX10" : "DX11", width, height, useFences ? "ON" : "OFF",
                             (copyQueries[0] != nullptr) ? "ON" : "OFF", isDXVKMode ? "ON" : "OFF");
        } else {
            EarlyLog("%s Capture Init FAILED (success=false)", isDX10Mode ? "DX10" : "DX11");
            Cleanup();
        }

}
