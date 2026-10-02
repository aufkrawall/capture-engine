#include "dx9_hook_internal.h"


bool DX9Capture::CreateD3D11Device() {


        // Load system D3D11/DXGI by full path to avoid using DXVK's versions.
        // In DXVK processes, d3d11.dll/dxgi.dll are DXVK's implementations whose
        // GetSharedHandle returns Vulkan-internal IDs that the real system D3D11
        // encoder cannot open. Using System32 paths ensures real KMT handles.
        char systemDir[MAX_PATH] = {};
        if (GetSystemDirectoryA(systemDir, MAX_PATH) == 0) {
            EarlyLog("DX9: GetSystemDirectory failed");
            return false;
        }
        std::string dxgiPath = std::string(systemDir) + "\\dxgi.dll";
        std::string d3d11Path = std::string(systemDir) + "\\d3d11.dll";

        // Find the adapter matching the D3D9 device
        HMODULE hDXGI = LoadLibraryA(dxgiPath.c_str());
        if (!hDXGI) {
            EarlyLog("DX9: System DXGI DLL not found");
            return false;
        }

        typedef HRESULT(WINAPI * PFN_CREATE_DXGI_FACTORY1)(REFIID, void**);
        PFN_CREATE_DXGI_FACTORY1 pCreateDXGIFactory1 =
            (PFN_CREATE_DXGI_FACTORY1)GetProcAddress(hDXGI, "CreateDXGIFactory1");
        if (!pCreateDXGIFactory1) {
            EarlyLog("DX9: CreateDXGIFactory1 not found");
            return false;
        }

        IDXGIFactory1* factory = nullptr;
        HRESULT hr = pCreateDXGIFactory1(IID_PPV_ARGS(&factory));
        if (FAILED(hr)) {
            EarlyLog("DX9: Failed to create DXGI factory");
            return false;
        }

        // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization) - zero-initialized placeholder; enum fields are assigned before use
        D3DDEVICE_CREATION_PARAMETERS creationParams = {};
        const bool hasCreationParams = SUCCEEDED(d3d9Device->GetCreationParameters(&creationParams));
        const UINT targetAdapterOrdinal = hasCreationParams ? creationParams.AdapterOrdinal : D3DADAPTER_DEFAULT;

        // Resolve the exact adapter without changing the game device. A private
        // Ex factory is safe to retain as the later shared-resource owner; it is
        // never returned to the application.
        IDirect3D9* d3d9 = nullptr;
        d3d9Device->GetDirect3D(&d3d9);
        LUID targetLuid = {};
        bool hasTargetLuid = false;
        IDirect3D9Ex* gameFactoryEx = nullptr;
        if (d3d9 && SUCCEEDED(d3d9->QueryInterface(__uuidof(IDirect3D9Ex), (void**)&gameFactoryEx)) && gameFactoryEx) {
            hasTargetLuid =
                hasCreationParams && SUCCEEDED(gameFactoryEx->GetAdapterLUID(targetAdapterOrdinal, &targetLuid));
            gameFactoryEx->Release();
        }
        if (!hasTargetLuid && !IsDXVKD3D9WrapperLoaded()) {
            HMODULE d3d9Module = GetModuleHandleA("d3d9.dll");
            Direct3DCreate9Ex_t create9Ex =
                d3d9Module ? reinterpret_cast<Direct3DCreate9Ex_t>(GetProcAddress(d3d9Module, "Direct3DCreate9Ex"))
                           : nullptr;
            if (!directSharedFactoryEx && create9Ex) {
                const HRESULT factoryHr = create9Ex(D3D_SDK_VERSION, &directSharedFactoryEx);
                if (FAILED(factoryHr)) {
                    directSharedFactoryEx = nullptr;
                    HookLogImportant(
                        "DX9: Private D3D9Ex helper factory unavailable for adapter mapping "
                        "(hr=0x%08x)",
                        (unsigned)factoryHr);
                }
            }
            if (directSharedFactoryEx && hasCreationParams) {
                hasTargetLuid = SUCCEEDED(directSharedFactoryEx->GetAdapterLUID(targetAdapterOrdinal, &targetLuid));
            }
        }
        if (d3d9)
            d3d9->Release();
        if (hasTargetLuid) {
            HookLogImportant("DX9: Resolved native device adapter LUID %08x:%08x (ordinal=%u)", targetLuid.HighPart,
                             targetLuid.LowPart, targetAdapterOrdinal);
        } else {
            HookLogImportant("DX9: Exact adapter LUID unavailable; using D3D9 adapter ordinal %u",
                             targetAdapterOrdinal);
        }

        // Find matching DXGI adapter
        IDXGIAdapter1* adapter = nullptr;
        for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; i++) {
            DXGI_ADAPTER_DESC1 desc;
            adapter->GetDesc1(&desc);
            bool matched = false;
            if (hasTargetLuid) {
                matched = (desc.AdapterLuid.LowPart == targetLuid.LowPart &&
                           desc.AdapterLuid.HighPart == targetLuid.HighPart);
            } else {
                matched = i == targetAdapterOrdinal;
            }

            if (matched) {
                // Store LUID
                // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
                luidLow = desc.AdapterLuid.LowPart;
                luidHigh = desc.AdapterLuid.HighPart;

                // Initialize SystemMetricsCollector with adapter LUID for GPU stats
                SystemMetricsCollector::Get().Initialize(luidLow, luidHigh);
                break;
            }

            adapter->Release();
            adapter = nullptr;
        }
        factory->Release();

        if (!adapter) {
            EarlyLog("DX9: No DXGI adapter found");
            return false;
        }

        // Create D3D11 device using system D3D11 (not DXVK's)
        HMODULE hD3D11 = LoadLibraryA(d3d11Path.c_str());
        if (!hD3D11) {
            EarlyLog("DX9: System D3D11 DLL not found");
            adapter->Release();
            return false;
        }

        // Redirect system d3d11.dll's dxgi.dll imports to system dxgi.dll so that
        // internal DXGI calls from system d3d11.dll resolve to the real driver rather
        // than DXVK's dxgi (which may be loaded first under the bare name "dxgi.dll").
        RedirectModuleImports(hD3D11, "dxgi.dll", hDXGI);

        typedef HRESULT(WINAPI * PFN_D3D11_CREATE_DEVICE)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT,
                                                          const D3D_FEATURE_LEVEL*, UINT, UINT, ID3D11Device**,
                                                          D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
        PFN_D3D11_CREATE_DEVICE pD3D11CreateDevice =
            (PFN_D3D11_CREATE_DEVICE)GetProcAddress(hD3D11, "D3D11CreateDevice");
        if (!pD3D11CreateDevice) {
            EarlyLog("DX9: D3D11CreateDevice not found");
            adapter->Release();
            return false;
        }

        D3D_FEATURE_LEVEL featureLevels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
        D3D_FEATURE_LEVEL featureLevel;

        hr = pD3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, NULL, 0, featureLevels, 3, D3D11_SDK_VERSION,
                                &d3d11Device, &featureLevel, &d3d11Context);
        adapter->Release();

        if (FAILED(hr)) {
            EarlyLog("DX9: Failed to create D3D11 device (hr=0x%08x)", hr);
            return false;
        }

        EarlyLog("DX9: Created D3D11 device (feature level %d)", featureLevel);
        return true;

}


void DX9Capture::Init(IDirect3DDevice9* device) {


        std::lock_guard<std::recursive_mutex> captureLock(captureMutex);
        HookLogImportant("DX9: DX9Capture::Init() entering. initialized=%d, failed=%d", initialized,
                         initializationFailed);
        if (generationResetPending) {
            SharedMemoryLayout* sharedMem = g_IPC ? g_IPC->GetSharedMem() : nullptr;
            if (HasOutstandingCaptureFrameLeases(sharedMem)) {
                static std::atomic<int> s_generationLeaseLogCount{0};
                if (s_generationLeaseLogCount.fetch_add(1, std::memory_order_relaxed) < 12) {
                    HookLog("DX9: Waiting for old frame leases before rebuilding capture after Reset");
                }
                return;
            }
            CleanupDX9(false, true);
        }
        if (initialized || initializationFailed)
            return;

        HookLogImportant("DX9: Init Step 1: AddRef device");
        device->AddRef();
        d3d9Device = device;

        EarlyLog("DX9: Init Step 2: GetRenderTarget");
        IDirect3DSurface9* backBuffer = nullptr;
        if (FAILED(device->GetRenderTarget(0, &backBuffer))) {
            EarlyLog("DX9: Failed to get render target");
            CleanupDX9(true);
            return;
        }

        EarlyLog("DX9: Init Step 3: GetDesc");
        D3DSURFACE_DESC desc;
        backBuffer->GetDesc(&desc);
        backBuffer->Release();

        width = desc.Width;
        height = desc.Height;
        d3d9Format = desc.Format;
        const D3D9SharedFormatSelection sharedFormat = SelectD3D9SharedCaptureFormat(desc.Format);
        d3d9SharedFormat = sharedFormat.resourceFormat;
        format = sharedFormat.transportFormat;
        EarlyLog("DX9: Init Step 4: Format check. w=%d, h=%d, fmt=%d", width, height, d3d9Format);
        if (sharedFormat && sharedFormat.requiresConversion) {
            HookLogImportant("DX9: Native GPU format conversion selected: backbuffer=%s/%d shared=%s/%d",
                             D3D9FormatName(d3d9Format), (int)d3d9Format, D3D9FormatName(d3d9SharedFormat),
                             (int)d3d9SharedFormat);
        }

        if (!sharedFormat) {
            HookLogImportant("DX9: Unsupported D3D9 capture backbuffer format %s/%d", D3D9FormatName(desc.Format),
                             (int)desc.Format);
            CleanupDX9(true);
            return;
        }

        EarlyLog("DX9: Init Step 5: CreateD3D11Device");
        if (!CreateD3D11Device()) {
            EarlyLog("DX9: CreateD3D11Device failed");
            CleanupDX9(true);
            return;
        }

        HookLogImportant("DX9: Init Step 6: Check D3D9Ex support");
        bool isD3D9Ex = false;
        if (SUCCEEDED(device->QueryInterface(__uuidof(IDirect3DDevice9Ex), (void**)&d3d9DeviceEx))) {
            HookLogImportant("DX9: Device supports D3D9Ex (zero-copy capture available)");
            isD3D9Ex = true;
        } else {
            HookLogImportant("DX9: Device is classic D3D9 (shared capture is probe-only; no Ex promotion)");
            d3d9DeviceEx = nullptr;
        }

        // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization) - zero-initialized placeholder; enum fields are assigned before use
        D3DDEVICE_CREATION_PARAMETERS creationParams = {};
        const HRESULT creationParamsHr = device->GetCreationParameters(&creationParams);
        allowAsyncD3D9WorkerCapture =
            SUCCEEDED(creationParamsHr) && ((creationParams.BehaviorFlags & D3DCREATE_MULTITHREADED) != 0);
        if (SUCCEEDED(creationParamsHr)) {
            HookLogImportant("DX9: Device creation flags=0x%08x (multithreaded=%d)",
                             (unsigned)creationParams.BehaviorFlags, allowAsyncD3D9WorkerCapture ? 1 : 0);
        } else {
            HookLogImportant(
                "DX9: GetCreationParameters failed during init (hr=0x%08x); async D3D9 worker capture disabled",
                (unsigned)creationParamsHr);
        }

        HookLogImportant("DX9: Init Step 7: Create DX9 Shared Resource");
        if (SetupDirectD3D9SharedRing(device, isD3D9Ex)) {
            goto create_ring_buffer;
        }
        HookLogImportant("DX9: Direct D3D9 shared ring unavailable, trying legacy shared-surface path");
        sharedHandle9 = NULL;

        // DXVK's D3D9Ex::CreateTexture returns Vulkan-internal IDs as shared
        // handles (not valid Win32 handles). Zero-copy via OpenSharedResource
        // would fail with these IDs. Skip zero-copy for DXVK and fall directly
        // to the D3D11 staging path (CPU readback + UpdateSubresource), which
        // uses only system D3D11 textures whose handles are always valid.
        // For native D3D9Ex (non-DXVK), try zero-copy as before.
        if (!IsDXVKD3D9WrapperLoaded()) {
            hr = device->CreateTexture(width, height, 1, D3DUSAGE_RENDERTARGET, d3d9SharedFormat, D3DPOOL_DEFAULT,
                                       &sharedTexture9, &sharedHandle9);
        } else {
            HookLogImportant("DX9: DXVK d3d9 detected - forcing D3D11 staging path (zero-copy skipped)");
            hr = E_FAIL;
        }

        if (FAILED(hr) || !sharedTexture9 || !sharedHandle9) {
            HookLogImportant(
                "DX9: Shared texture failed (hr=0x%08x, tex=%p, handle=%p), "
                "trying native D3D9 fallback paths...",
                hr, sharedTexture9, sharedHandle9);

            // Cleanup failed D3D9 shared texture
            if (sharedTexture9) {
                sharedTexture9->Release();
                sharedTexture9 = nullptr;
            }
            sharedHandle9 = NULL;

            // Try GDI interop for zero-copy capture on native D3D9.
            // Uses GetDC/BitBlt for GPU-accelerated D3D9->D3D11 transfer.
            if (SetupGDIInterop(device)) {
                useGDIInterop = true;
                HookLogImportant("DX9: GDI interop zero-copy path active");
                goto create_ring_buffer;
            }
            HookLogImportant("DX9: GDI interop unavailable, using D3D11 staging fallback");

            // D3D11 Staging Path: For non-Ex devices, we use GetRenderTargetData
            // to read the backbuffer into CPU memory, then UpdateSubresource to
            // upload directly into D3D11 shared textures. This avoids the slow
            // shmem IPC path entirely and gives the encoder real GPU textures.

            // Create a ring of staging surfaces + event queries so readback can be
            // pipelined and consumed without stalling the Present thread.
            bool stagingOk = true;
            for (int i = 0; i < CAPTURE_TEXTURE_COUNT && stagingOk; i++) {
                hr = device->CreateOffscreenPlainSurface(width, height, d3d9Format, D3DPOOL_SYSTEMMEM,
                                                         &shmemSurfaces[i], nullptr);
                if (FAILED(hr)) {
                    EarlyLog("DX9: Failed to create staging surface %d (hr=0x%08x)", i, hr);
                    stagingOk = false;
                    break;
                }

                if (i == 0) {
                    D3DLOCKED_RECT rect;
                    if (SUCCEEDED(shmemSurfaces[i]->LockRect(&rect, nullptr, D3DLOCK_READONLY))) {
                        shmemPitch = rect.Pitch;
                        shmemSurfaces[i]->UnlockRect();
                    }
                }

                hr = device->CreateQuery(D3DQUERYTYPE_EVENT, &shmemQueries[i]);
                if (FAILED(hr)) {
                    EarlyLog("DX9: Failed to create staging query %d (hr=0x%08x)", i, hr);
                    stagingOk = false;
                    break;
                }
            }

            if (!stagingOk) {
                CleanupDX9(true);
                return;
            }

            // Try to stage GPU work first into DEFAULT-pool render targets, then
            // read back older staged frames. This can reduce hard stalls compared to
            // directly reading the current backbuffer each submission.
            bool intermediateOk = true;
            for (int i = 0; i < CAPTURE_TEXTURE_COUNT && intermediateOk; ++i) {
                hr = device->CreateTexture(width, height, 1, D3DUSAGE_RENDERTARGET, d3d9Format, D3DPOOL_DEFAULT,
                                           &stagingTextures[i], nullptr);
                if (FAILED(hr) || !stagingTextures[i]) {
                    intermediateOk = false;
                    break;
                }
                hr = stagingTextures[i]->GetSurfaceLevel(0, &stagingRenderSurfaces[i]);
                if (FAILED(hr) || !stagingRenderSurfaces[i]) {
                    intermediateOk = false;
                    break;
                }
            }

            stagingWriteIdx = 0;
            stagingReadIdx = 0;
            stagingPending = 0;
            stagingUseGpuIntermediate = intermediateOk;
            stagingLastSubmitQpc = 0;
            for (int i = 0; i < CAPTURE_TEXTURE_COUNT; ++i) {
                shmemTextureReady[i] = false;
            }

            EarlyLog(
                "DX9: Staging ring created (%d surfaces, pitch=%d), proceeding "
                "to D3D11 ring buffer setup",
                CAPTURE_TEXTURE_COUNT, shmemPitch);
            if (!stagingUseGpuIntermediate) {
                for (int i = 0; i < CAPTURE_TEXTURE_COUNT; ++i) {
                    if (stagingRenderSurfaces[i]) {
                        stagingRenderSurfaces[i]->Release();
                        stagingRenderSurfaces[i] = nullptr;
                    }
                    if (stagingTextures[i]) {
                        stagingTextures[i]->Release();
                        stagingTextures[i] = nullptr;
                    }
                }
                EarlyLog(
                    "DX9: GPU intermediate staging unavailable, using direct "
                    "readback submissions");
            } else {
                EarlyLog("DX9: GPU intermediate staging enabled");
            }
            useD3D11Staging = true;
            char exePath[MAX_PATH] = {};
            GetModuleFileNameA(nullptr, exePath, MAX_PATH);
            const char* exeName = strrchr(exePath, '\\');
            exeName = exeName ? (exeName + 1) : exePath;
            // Trine3 DXVK SHMEM fallback removed - Vulkan layer handles zero-copy capture
            // Fall through to create D3D11 ring buffer shared textures (steps 10+)
        }

        // Steps 8-9: D3D9→D3D11 interop (only for zero-copy path with D3D9Ex)
        if (!useD3D11Staging && !useShmem) {
            EarlyLog("DX9: Init Step 8: GetSurfaceLevel");
            hr = sharedTexture9->GetSurfaceLevel(0, &copySurface);
            if (FAILED(hr)) {
                EarlyLog("DX9: GetSurfaceLevel failed");
                CleanupDX9(true);
                return;
            }

            EarlyLog("DX9: Init Step 9: OpenSharedResource in D3D11");
            if (d3d11Device) {
                hr = d3d11Device->OpenSharedResource(sharedHandle9, __uuidof(ID3D11Texture2D),
                                                     (void**)&d3d11SharedTexture);
                if (FAILED(hr)) {
                    EarlyLog("DX9: Failed to open shared resource in D3D11 (hr=0x%08x)", hr);
                    CleanupDX9(true);
                    return;
                }

                // D3D11 maps D3DFMT_X8R8G8B8 to DXGI_FORMAT_B8G8R8X8_UNORM, not B8G8R8A8_UNORM.
                // Sync 'format' to the actual D3D11 format so ring buffer textures use the same
                // format as d3d11SharedTexture — CopySubresourceRegion requires exact format match.
                D3D11_TEXTURE2D_DESC sharedDesc = {};
                d3d11SharedTexture->GetDesc(&sharedDesc);
                format = (uint32_t)sharedDesc.Format;
                EarlyLog("DX9: Shared resource format: %d (sharedD3D9Fmt=%d, backBufferFmt=%d)", sharedDesc.Format,
                         d3d9SharedFormat, d3d9Format);
            }

            // Create D3D9 event query for cross-API synchronization.
            // Ensures StretchRect to shared texture completes on the GPU
            // before D3D11 reads from the same resource.
            hr = device->CreateQuery(D3DQUERYTYPE_EVENT, &zeroCopyQuery);
            if (FAILED(hr)) {
                HookLogImportant("DX9: Warning: Failed to create zero-copy sync query (hr=0x%08x)", hr);
                zeroCopyQuery = nullptr;
            }
        }

    create_ring_buffer:
        EarlyLog("DX9: Init Step 10: Create Ring Buffer Shared Textures");
        bool success = true;
        if (!useShmem) {
            // Try to enable fences
            ID3D11Device5* device5 = nullptr;
            HRESULT qiHr = d3d11Device->QueryInterface(IID_PPV_ARGS(&device5));
            if (SUCCEEDED(qiHr)) {
                HRESULT fenceHr = device5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&fence));
                if (SUCCEEDED(fenceHr)) {
                    if (SUCCEEDED(d3d11Context->QueryInterface(IID_PPV_ARGS(&context4)))) {
                        HANDLE hTemp = NULL;
                        if (SUCCEEDED(fence->CreateSharedHandle(NULL, GENERIC_ALL, NULL, &hTemp))) {
                            sharedFenceHandle.store(hTemp, std::memory_order_release);
                            useFences = true;
                            EarlyLog("DX9: ID3D11Fence support enabled");
                        } else {
                            EarlyLog("DX9: Fence CreateSharedHandle failed");
                        }
                    } else {
                        EarlyLog("DX9: ID3D11DeviceContext4 QI failed");
                    }
                } else {
                    EarlyLog("DX9: CreateFence failed (hr=0x%08x)", fenceHr);
                }
                device5->Release();
            } else {
                EarlyLog(
                    "DX9: ID3D11Device5 QI failed (hr=0x%08x), "
                    "fence not available (feature level=0x%x)",
                    qiHr, d3d11Device->GetFeatureLevel());
            }

            if (!useFences) {
                EarlyLog("DX9: Fence not available, using synchronous copy");
            }

            if (useDirectD3D9SharedRing) {
                success = true;
                EarlyLog("DX9: Direct D3D9 shared ring active - skipping D3D11 shared texture ring creation");
            } else if (useGDIInterop) {
                success = CreateSharedTextureRing(true);
                if (success) {
                    if (gdiSurface) {
                        gdiSurface->Release();
                        gdiSurface = nullptr;
                    }
                    if (gdiTexture) {
                        gdiTexture->Release();
                        gdiTexture = nullptr;
                    }
                    HookLogImportant(
                        "DX9: GDI interop using shared ring textures (direct publish, no extra D3D11 copy)");
                } else {
                    HookLogImportant("DX9: Shared GDI ring unavailable, using intermediate GDI copy path");
                    success = CreateSharedTextureRing(false);
                }
            } else {
                success = CreateSharedTextureRing(false);
            }
        } else {
            EarlyLog("DX9: SHMEM transport active - skipping D3D11 shared handle ring setup");
        }

        if (success) {
            if (g_IPC) {
                PublishToSharedMemory(g_IPC);
            }
            CaptureBase::initialized = true;
            dx9_hook_g_DX9StagingCaptureActive.store(useD3D11Staging, std::memory_order_release);

            // Start background capture thread for D3D11 staging path
            if (useD3D11Staging && allowAsyncD3D9WorkerCapture) {
                StartCaptureThread([this]() { StagingCaptureThreadProc(); });
            } else if (useD3D11Staging) {
                HookLogImportant(
                    "DX9: D3D11 staging consume will stay on the render thread (device lacks D3DCREATE_MULTITHREADED)");
            }

            // Start background capture thread for GDI interop path
            if (useGDIInterop && allowAsyncD3D9WorkerCapture) {
                StartCaptureThread([this]() { GDICaptureThreadProc(); });
            } else if (useGDIInterop) {
                HookLogImportant(
                    "DX9: GDI interop consume will stay on the render thread (device lacks D3DCREATE_MULTITHREADED)");
            }

            const char* captureMode = useShmem                  ? "SHMEM"
                                      : useDirectD3D9SharedRing ? "D3D9-SHARED-DIRECT"
                                      : useD3D11Staging         ? "D3D11-STAGING"
                                      : useGDIInterop ? (gdiDirectSharedRing ? "GDI-INTEROP+DIRECT" : "GDI-INTEROP")
                                                      : "ZERO-COPY";
            const char* asyncSuffix = ((useD3D11Staging || useGDIInterop) && captureThreadRunning) ? "+ASYNC" : "";
            HookLogImportant("DX9 Capture Initialized (%s%s): %dx%d (LUID: %08x)", captureMode, asyncSuffix, width,
                             height, luidLow);
        } else {
            CleanupDX9();
        }

}

