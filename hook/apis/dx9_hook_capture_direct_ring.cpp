#include "dx9_hook_internal.h"


bool DX9Capture::EnsureDirectD3D9HelperWindow() {


        if (directSharedHelperWindow)
            return true;

        static constexpr const char* kHelperWindowClass = "CE_DX9SharedRingHelper";

        WNDCLASSEXA wc = {sizeof(wc)};
        wc.style = CS_CLASSDC;
        wc.lpfnWndProc = DefWindowProcA;
        wc.hInstance = GetModuleHandle(nullptr);
        wc.lpszClassName = kHelperWindowClass;
        if (!RegisterClassExA(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            HookLogImportant("DX9: Direct D3D9 shared ring unavailable - helper window class registration failed");
            return false;
        }

        directSharedHelperWindow = CreateWindowA(kHelperWindowClass, "CE DX9 Shared Ring", WS_OVERLAPPED, 0, 0, 64, 64,
                                                 nullptr, nullptr, wc.hInstance, nullptr);
        if (!directSharedHelperWindow) {
            HookLogImportant("DX9: Direct D3D9 shared ring unavailable - helper window creation failed");
            return false;
        }
        ShowWindow(directSharedHelperWindow, SW_HIDE);
        return true;

}


DWORD DX9Capture::BuildDirectD3D9HelperBehaviorFlags(DWORD gameBehaviorFlags) {


        DWORD helperFlags = D3DCREATE_MULTITHREADED;
        if (gameBehaviorFlags & D3DCREATE_FPU_PRESERVE) {
            helperFlags |= D3DCREATE_FPU_PRESERVE;
        }

        if (gameBehaviorFlags & D3DCREATE_HARDWARE_VERTEXPROCESSING) {
            helperFlags |= D3DCREATE_HARDWARE_VERTEXPROCESSING;
        } else if (gameBehaviorFlags & D3DCREATE_MIXED_VERTEXPROCESSING) {
            helperFlags |= D3DCREATE_MIXED_VERTEXPROCESSING;
        } else {
            helperFlags |= D3DCREATE_SOFTWARE_VERTEXPROCESSING;
        }

        return helperFlags;

}


DWORD DX9Capture::BuildDirectD3D9HelperSoftwareVpFlags(DWORD helperFlags) {


        helperFlags &= ~(D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_MIXED_VERTEXPROCESSING);
        helperFlags |= D3DCREATE_SOFTWARE_VERTEXPROCESSING;
        return helperFlags;

}


D3DFORMAT DX9Capture::ResolveDirectD3D9HelperBackBufferFormat() const {


        if (d3d9SharedFormat != D3DFMT_UNKNOWN) {
            return d3d9SharedFormat;
        }
        if (d3d9Format != D3DFMT_UNKNOWN) {
            return d3d9Format;
        }
        return D3DFMT_A8R8G8B8;

}


void DX9Capture::BuildDirectD3D9HelperPresentParameters(D3DPRESENT_PARAMETERS& pp,  bool useExRuntime) const {


        // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization) - zero-initialized placeholder; enum fields are assigned before use
        pp = {};
        pp.Windowed = TRUE;
        pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
        pp.hDeviceWindow = directSharedHelperWindow;
        if (useExRuntime) {
            // The Ex helper producer never presents. A minimal hidden swapchain
            // avoids 0x0/UNKNOWN device-creation quirks and keeps helper VRAM
            // pressure low while still allowing shared-resource creation.
            pp.BackBufferWidth = 1;
            pp.BackBufferHeight = 1;
            pp.BackBufferFormat = ResolveDirectD3D9HelperBackBufferFormat();
        } else {
            // Plain D3D9 is stricter in windowed mode: let the runtime pick a
            // desktop-compatible backbuffer so the helper device can exist only
            // as a resource factory for native shared-texture zero-copy.
            pp.BackBufferWidth = 0;
            pp.BackBufferHeight = 0;
            pp.BackBufferFormat = D3DFMT_UNKNOWN;
        }
        pp.BackBufferCount = 1;
        pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;

}


void DX9Capture::ResetDirectD3D9SharedRingPendingState() {


        directSharedSubmitIdx = 0;
        directSharedDrainIdx = 0;
        directSharedPendingCount = 0;
        zeroCopyPendingCopy = false;
        zeroCopyPendingIdx = -1;
        zeroCopyPendingTimestampQpc = 0;
        zeroCopyQueryWaitUs = 0;
        zeroCopyReadbackUs = 0;
        for (int i = 0; i < CAPTURE_TEXTURE_COUNT; ++i) {
            directSharedPending[i] = false;
            directSharedPendingTimestampQpc[i] = 0;
        }

}


int DX9Capture::AcquireDirectD3D9SharedRingSubmitIndex() {


        SharedMemoryLayout* sharedMem = g_IPC ? g_IPC->GetSharedMem() : nullptr;
        for (int attempt = 0; attempt < CAPTURE_TEXTURE_COUNT; ++attempt) {
            const int idx = (directSharedSubmitIdx + attempt) % CAPTURE_TEXTURE_COUNT;
            if (!directSharedPending[idx] && !IsCaptureTextureSlotOutstanding(sharedMem, idx)) {
                directSharedSubmitIdx = (idx + 1) % CAPTURE_TEXTURE_COUNT;
                return idx;
            }
        }
        return -1;

}


void DX9Capture::SignalDirectD3D9SharedRingFrame(int idx,  int64_t frameTimestampQpc) {


        SignalPublishedTextureFrame(idx, frameTimestampQpc);

}


void DX9Capture::DrainDirectD3D9SharedRingCompletions(bool flushOutstanding) {


        zeroCopyQueryWaitUs = 0;
        zeroCopyReadbackUs = 0;

        if (!useDirectD3D9SharedRing || directSharedPendingCount <= 0) {
            return;
        }

        const DWORD getDataFlags = flushOutstanding ? D3DGETDATA_FLUSH : 0;
        int completedThisPass = 0;

        while (directSharedPendingCount > 0 && completedThisPass < CAPTURE_TEXTURE_COUNT) {
            int idx = -1;
            for (int attempt = 0; attempt < CAPTURE_TEXTURE_COUNT; ++attempt) {
                const int candidate = (directSharedDrainIdx + attempt) % CAPTURE_TEXTURE_COUNT;
                if (directSharedPending[candidate]) {
                    idx = candidate;
                    break;
                }
            }

            if (idx < 0) {
                ResetDirectD3D9SharedRingPendingState();
                return;
            }

            IDirect3DQuery9* query = directSharedQueries9[idx];
            HRESULT queryHr = S_OK;
            if (query) {
                // D3DGETDATA_FLUSH requests submission, but completion remains
                // non-blocking. Never spin the Present/shutdown thread on a GPU
                // query whose device may be lost or no longer making progress.
                queryHr = query->GetData(nullptr, 0, getDataFlags);
            }

            if (queryHr == S_FALSE) {
                break;
            }

            const int64_t frameTimestampQpc = directSharedPendingTimestampQpc[idx];
            directSharedPending[idx] = false;
            directSharedPendingTimestampQpc[idx] = 0;
            if (directSharedPendingCount > 0) {
                directSharedPendingCount--;
            }
            directSharedDrainIdx = (idx + 1) % CAPTURE_TEXTURE_COUNT;

            if (SUCCEEDED(queryHr)) {
                SignalDirectD3D9SharedRingFrame(idx, frameTimestampQpc);
            } else {
                droppedFrames.fetch_add(1, std::memory_order_relaxed);
                static int queryFailLogCount = 0;
                if (queryFailLogCount < 4) {
                    HookLogImportant("DX9: Direct shared-ring query failed idx=%d hr=0x%08x", idx, (unsigned)queryHr);
                    queryFailLogCount++;
                }
            }

            completedThisPass++;
        }

}


void DX9Capture::LogDirectD3D9SharingDiagnostics(IDirect3DDevice9* device,  const D3DDEVICE_CREATION_PARAMETERS& params, 
                                         const char* label) {


        if (!device || !label)
            return;

        IDirect3D9* direct3D = nullptr;
        HRESULT getD3DHr = device->GetDirect3D(&direct3D);
        if (FAILED(getD3DHr) || !direct3D) {
            HookLogImportant("DX9: %s diagnostics unavailable - GetDirect3D failed (hr=0x%08x)", label,
                             (unsigned)getD3DHr);
            return;
        }

        IDirect3DDevice9Ex* deviceEx = nullptr;
        const bool isEx =
            SUCCEEDED(device->QueryInterface(__uuidof(IDirect3DDevice9Ex), (void**)&deviceEx)) && deviceEx;
        if (deviceEx) {
            deviceEx->Release();
        }

        // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization) - zero-initialized placeholder; enum fields are assigned before use
        D3DCAPS9 caps = {};
        D3DADAPTER_IDENTIFIER9 identifier = {};
        D3DDISPLAYMODE displayMode = {};
        const HRESULT capsHr = direct3D->GetDeviceCaps(params.AdapterOrdinal, params.DeviceType, &caps);
        const HRESULT identHr = direct3D->GetAdapterIdentifier(params.AdapterOrdinal, 0, &identifier);
        const HRESULT modeHr = direct3D->GetAdapterDisplayMode(params.AdapterOrdinal, &displayMode);
        const D3DFORMAT adapterFormat = SUCCEEDED(modeHr) ? displayMode.Format : d3d9Format;
        const HRESULT sharedFmtHr =
            direct3D->CheckDeviceFormat(params.AdapterOrdinal, params.DeviceType, adapterFormat, D3DUSAGE_RENDERTARGET,
                                        D3DRTYPE_TEXTURE, d3d9SharedFormat);
        const HRESULT conversionHr = d3d9Format == d3d9SharedFormat
                                         ? D3D_OK
                                         : direct3D->CheckDeviceFormatConversion(
                                               params.AdapterOrdinal, params.DeviceType, d3d9Format, d3d9SharedFormat);
        bool advertisesExSharing = false;
#ifdef D3DCAPS2_CANSHARERESOURCE
        advertisesExSharing = SUCCEEDED(capsHr) && ((caps.Caps2 & D3DCAPS2_CANSHARERESOURCE) != 0);
#endif

        HookLogImportant(
            "DX9: %s diagnostics: adapter=%u type=%u flags=0x%08x ex=%d adapterFmt=%s/%d backBufferFmt=%s/%d "
            "sharedFmt=%s/%d capsHr=0x%08x caps2=0x%08x exShareCap=%d fmtCheck=0x%08x conversion=0x%08x "
            "vendor=%04x device=%04x driver=%s",
            label, params.AdapterOrdinal, (unsigned)params.DeviceType, (unsigned)params.BehaviorFlags, isEx ? 1 : 0,
            D3D9FormatName(adapterFormat), (int)adapterFormat, D3D9FormatName(d3d9Format), (int)d3d9Format,
            D3D9FormatName(d3d9SharedFormat), (int)d3d9SharedFormat, (unsigned)capsHr,
            SUCCEEDED(capsHr) ? (unsigned)caps.Caps2 : 0u, advertisesExSharing ? 1 : 0, (unsigned)sharedFmtHr,
            (unsigned)conversionHr, SUCCEEDED(identHr) ? identifier.VendorId : 0u,
            SUCCEEDED(identHr) ? identifier.DeviceId : 0u, SUCCEEDED(identHr) ? identifier.Driver : "?");

        if (!isEx && !advertisesExSharing) {
            HookLogImportant(
                "DX9: %s classic-device share capability is probe-only; the Ex-only caps bit is not "
                "used as a gate",
                label);
        }

        direct3D->Release();

}


bool DX9Capture::ProbeDirectD3D9SharedTexture(IDirect3DDevice9* device,  const char* label) {


        if (!device || !label)
            return false;

        HANDLE sharedHandle = NULL;
        IDirect3DTexture9* texture = nullptr;
        const HRESULT probeHr = device->CreateTexture(width, height, 1, D3DUSAGE_RENDERTARGET, d3d9SharedFormat,
                                                      D3DPOOL_DEFAULT, &texture, &sharedHandle);
        const bool success = SUCCEEDED(probeHr) && texture && sharedHandle;
        HookLogImportant("DX9: %s shared-texture probe fmt=%s/%d hr=0x%08x tex=%p handle=%p", label,
                         D3D9FormatName(d3d9SharedFormat), (int)d3d9SharedFormat, (unsigned)probeHr, texture,
                         sharedHandle);
        if (texture) {
            texture->Release();
        }
        // D3D9 shared-resource values are legacy resource-owned identifiers,
        // not NT handles returned by CreateSharedHandle.
        return success;

}


bool DX9Capture::EnsureDirectD3D9ExProducerDevice(const D3DDEVICE_CREATION_PARAMETERS& params) {


        const DWORD helperFlags = BuildDirectD3D9HelperBehaviorFlags(params.BehaviorFlags);
        if (directSharedProducerDeviceEx) {
            const bool sameConfig = directSharedExConfig.valid &&
                                    directSharedExConfig.adapterOrdinal == params.AdapterOrdinal &&
                                    directSharedExConfig.deviceType == params.DeviceType &&
                                    directSharedExConfig.behaviorFlags == helperFlags;
            if (sameConfig)
                return true;

            HookLogImportant(
                "DX9: Recreating helper D3D9Ex producer for adapter/type change (oldAdapter=%u oldType=%u "
                "oldFlags=0x%08x newAdapter=%u newType=%u newFlags=0x%08x)",
                directSharedExConfig.adapterOrdinal, (unsigned)directSharedExConfig.deviceType,
                (unsigned)directSharedExConfig.behaviorFlags, params.AdapterOrdinal, (unsigned)params.DeviceType,
                (unsigned)helperFlags);
            directSharedProducerDeviceEx->Release();
            directSharedProducerDeviceEx = nullptr;
            directSharedExConfig = {};
        }

        HMODULE d3d9Module = GetModuleHandleA("d3d9.dll");
        if (!d3d9Module) {
            HookLogImportant("DX9: Direct D3D9 shared ring unavailable - d3d9.dll missing");
            return false;
        }
        if (!EnsureDirectD3D9HelperWindow()) {
            return false;
        }

        if (!directSharedFactoryEx) {
            Direct3DCreate9Ex_t create9Ex =
                reinterpret_cast<Direct3DCreate9Ex_t>(GetProcAddress(d3d9Module, "Direct3DCreate9Ex"));
            if (!create9Ex) {
                HookLogImportant("DX9: Direct D3D9Ex helper unavailable - Direct3DCreate9Ex missing");
                return false;
            }

            const HRESULT factoryHr = create9Ex(D3D_SDK_VERSION, &directSharedFactoryEx);
            if (FAILED(factoryHr) || !directSharedFactoryEx) {
                HookLogImportant("DX9: Direct D3D9Ex helper unavailable - Direct3DCreate9Ex failed (0x%08x)",
                                 (unsigned)factoryHr);
                directSharedFactoryEx = nullptr;
                return false;
            }
        }

        // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization) - zero-initialized placeholder; enum fields are assigned before use
        D3DPRESENT_PARAMETERS pp = {};
        BuildDirectD3D9HelperPresentParameters(pp, true);
        HookLogImportant("DX9: Trying helper D3D9Ex producer (adapter=%u type=%u flags=0x%08x)", params.AdapterOrdinal,
                         (unsigned)params.DeviceType, (unsigned)helperFlags);

        const DX9InternalBypassScope helperBypass;
        const HRESULT deviceHr =
            directSharedFactoryEx->CreateDeviceEx(params.AdapterOrdinal, params.DeviceType, directSharedHelperWindow,
                                                  helperFlags, &pp, nullptr, &directSharedProducerDeviceEx);
        if (FAILED(deviceHr) || !directSharedProducerDeviceEx) {
            HookLogImportant("DX9: Direct D3D9Ex helper unavailable - CreateDeviceEx failed (0x%08x)",
                             (unsigned)deviceHr);
            if (directSharedProducerDeviceEx) {
                directSharedProducerDeviceEx->Release();
                directSharedProducerDeviceEx = nullptr;
            }
            directSharedExConfig = {};
            return false;
        }

        directSharedExConfig.adapterOrdinal = params.AdapterOrdinal;
        directSharedExConfig.deviceType = params.DeviceType;
        directSharedExConfig.behaviorFlags = helperFlags;
        directSharedExConfig.valid = true;

        DX9_RegisterInternalHelperDevice(directSharedProducerDeviceEx);

        ProbeDirectD3D9SharedTexture(directSharedProducerDeviceEx, "helper D3D9Ex producer");
        return true;

}


bool DX9Capture::EnsureDirectD3D9LegacyProducerDevice(const D3DDEVICE_CREATION_PARAMETERS& params) {


        const DWORD helperFlags = BuildDirectD3D9HelperBehaviorFlags(params.BehaviorFlags);
        if (directSharedProducerDevice) {
            const bool sameConfig = directSharedLegacyConfig.valid &&
                                    directSharedLegacyConfig.adapterOrdinal == params.AdapterOrdinal &&
                                    directSharedLegacyConfig.deviceType == params.DeviceType &&
                                    directSharedLegacyConfig.behaviorFlags == helperFlags;
            if (sameConfig)
                return true;

            HookLogImportant(
                "DX9: Recreating helper legacy D3D9 producer for adapter/type change (oldAdapter=%u oldType=%u "
                "oldFlags=0x%08x newAdapter=%u newType=%u newFlags=0x%08x)",
                directSharedLegacyConfig.adapterOrdinal, (unsigned)directSharedLegacyConfig.deviceType,
                (unsigned)directSharedLegacyConfig.behaviorFlags, params.AdapterOrdinal, (unsigned)params.DeviceType,
                (unsigned)helperFlags);
            directSharedProducerDevice->Release();
            directSharedProducerDevice = nullptr;
            directSharedLegacyConfig = {};
        }

        HMODULE d3d9Module = GetModuleHandleA("d3d9.dll");
        if (!d3d9Module) {
            HookLogImportant("DX9: Direct D3D9 shared ring unavailable - d3d9.dll missing");
            return false;
        }
        if (!EnsureDirectD3D9HelperWindow()) {
            return false;
        }

        if (!directSharedFactory) {
            Direct3DCreate9Helper_t create9 =
                reinterpret_cast<Direct3DCreate9Helper_t>(GetProcAddress(d3d9Module, "Direct3DCreate9"));
            if (!create9) {
                HookLogImportant("DX9: Direct D3D9 helper unavailable - Direct3DCreate9 missing");
                return false;
            }

            directSharedFactory = create9(D3D_SDK_VERSION);
            if (!directSharedFactory) {
                HookLogImportant("DX9: Direct D3D9 helper unavailable - Direct3DCreate9 failed");
                return false;
            }
        }

        auto tryCreateLegacyHelper = [&](DWORD attemptFlags, const char* attemptLabel) {
            // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization) - zero-initialized placeholder; enum fields are assigned before use
            D3DPRESENT_PARAMETERS pp = {};
            BuildDirectD3D9HelperPresentParameters(pp, false);
            HookLogImportant(
                "DX9: Trying helper legacy D3D9 producer (%s adapter=%u type=%u flags=0x%08x bbFmt=%s/%d bb=%ux%u)",
                attemptLabel, params.AdapterOrdinal, (unsigned)params.DeviceType, (unsigned)attemptFlags,
                D3D9FormatName(pp.BackBufferFormat), (int)pp.BackBufferFormat, pp.BackBufferWidth, pp.BackBufferHeight);

            const DX9InternalBypassScope helperBypass;
            return directSharedFactory->CreateDevice(params.AdapterOrdinal, params.DeviceType, directSharedHelperWindow,
                                                     attemptFlags, &pp, &directSharedProducerDevice);
        };

        DWORD selectedFlags = helperFlags;
        HRESULT deviceHr = tryCreateLegacyHelper(helperFlags, "game-flags");
        if ((FAILED(deviceHr) || !directSharedProducerDevice)) {
            const DWORD softwareVpFlags = BuildDirectD3D9HelperSoftwareVpFlags(helperFlags);
            if (softwareVpFlags != helperFlags) {
                if (directSharedProducerDevice) {
                    directSharedProducerDevice->Release();
                    directSharedProducerDevice = nullptr;
                }
                HookLogImportant(
                    "DX9: Helper legacy D3D9 producer retrying with software vertex processing after hr=0x%08x",
                    (unsigned)deviceHr);
                deviceHr = tryCreateLegacyHelper(softwareVpFlags, "software-vp fallback");
                selectedFlags = softwareVpFlags;
            }

        }

        if (FAILED(deviceHr) || !directSharedProducerDevice) {
            HookLogImportant("DX9: Direct D3D9 helper unavailable - CreateDevice failed (0x%08x)", (unsigned)deviceHr);
            if (directSharedProducerDevice) {
                directSharedProducerDevice->Release();
                directSharedProducerDevice = nullptr;
            }
            directSharedLegacyConfig = {};
            return false;
        }

        directSharedLegacyConfig.adapterOrdinal = params.AdapterOrdinal;
        directSharedLegacyConfig.deviceType = params.DeviceType;
        directSharedLegacyConfig.behaviorFlags = selectedFlags;
        directSharedLegacyConfig.valid = true;

        DX9_RegisterInternalHelperDevice(directSharedProducerDevice);

        ProbeDirectD3D9SharedTexture(directSharedProducerDevice, "helper legacy D3D9 producer");
        return true;

}


bool DX9Capture::ValidateDirectD3D9SharedHandle(HANDLE sharedHandle) {


        if (!d3d11Device || !sharedHandle)
            return false;

        ID3D11Texture2D* openedTexture = nullptr;
        HRESULT openHr =
            d3d11Device->OpenSharedResource(sharedHandle, __uuidof(ID3D11Texture2D), (void**)&openedTexture);
        if (FAILED(openHr) || !openedTexture) {
            HookLogImportant("DX9: Direct D3D9 shared ring validation failed (OpenSharedResource hr=0x%08x)",
                             (unsigned)openHr);
            return false;
        }

        D3D11_TEXTURE2D_DESC desc = {};
        openedTexture->GetDesc(&desc);
        format = static_cast<uint32_t>(desc.Format);
        HookLogImportant("DX9: Direct D3D9 shared ring validated in D3D11 (format=%u)", (unsigned)desc.Format);
        openedTexture->Release();
        return true;

}


bool DX9Capture::TrySetupDirectD3D9SharedRingWithProducer(IDirect3DDevice9* gameDevice,  IDirect3DDevice9* producerDevice, 
                                                  bool useHelperProducer,  const char* producerLabel) {


        if (!gameDevice || !producerDevice || !producerLabel)
            return false;

        auto failSetup = [&](const char* message, HRESULT failureHr) {
            HookLogImportant("DX9: %s (producer=%s hr=0x%08x)", message, producerLabel, (unsigned)failureHr);
            CleanupSharedHandles();
            ReleaseDirectD3D9RingResources();
            return false;
        };

        ReleaseDirectD3D9RingResources();

        for (int i = 0; i < CAPTURE_TEXTURE_COUNT; ++i) {
            HANDLE sharedHandle = NULL;
            IDirect3DTexture9* producerTexture = nullptr;
            const HRESULT producerHr =
                producerDevice->CreateTexture(width, height, 1, D3DUSAGE_RENDERTARGET, d3d9SharedFormat,
                                              D3DPOOL_DEFAULT, &producerTexture, &sharedHandle);
            if (FAILED(producerHr) || !producerTexture || !sharedHandle) {
                if (producerTexture) {
                    producerTexture->Release();
                }
                return failSetup("Direct D3D9 shared ring producer texture creation failed", producerHr);
            }

            IDirect3DTexture9* captureTexture = producerTexture;
            if (useHelperProducer) {
                HANDLE openHandle = sharedHandle;
                const HRESULT openHr =
                    gameDevice->CreateTexture(width, height, 1, D3DUSAGE_RENDERTARGET, d3d9SharedFormat,
                                              D3DPOOL_DEFAULT, &captureTexture, &openHandle);
                if (FAILED(openHr) || !captureTexture) {
                    producerTexture->Release();
                    return failSetup("Direct D3D9 shared ring open-on-game-device failed", openHr);
                }
                if (openHandle) {
                    sharedHandle = openHandle;
                }
                directSharedProducerTextures9[i] = producerTexture;
            }

            directSharedTextures9[i] = captureTexture;
            sharedTextureHandles[i].store(sharedHandle, std::memory_order_release);

            const HRESULT surfaceHr = captureTexture->GetSurfaceLevel(0, &directSharedSurfaces9[i]);
            if (FAILED(surfaceHr) || !directSharedSurfaces9[i]) {
                return failSetup("Direct D3D9 shared ring GetSurfaceLevel failed", surfaceHr);
            }

            const HRESULT queryHr = gameDevice->CreateQuery(D3DQUERYTYPE_EVENT, &directSharedQueries9[i]);
            if (FAILED(queryHr) || !directSharedQueries9[i]) {
                return failSetup("Direct D3D9 shared ring query creation failed", queryHr);
            }
        }

        if (!ValidateDirectD3D9SharedHandle(sharedTextureHandles[0].load(std::memory_order_acquire))) {
            CleanupSharedHandles();
            ReleaseDirectD3D9RingResources();
            return false;
        }

        useDirectD3D9SharedRing = true;
        directSharedUsesHelperProducer = useHelperProducer;
        HookLogImportant("DX9: Direct D3D9 shared ring zero-copy path active (%s)", producerLabel);
        return true;

}


bool DX9Capture::SetupDirectD3D9SharedRing(IDirect3DDevice9* device,  bool isD3D9Ex) {


        if (!device || IsDXVKD3D9WrapperLoaded())
            return false;

        CleanupSharedHandles();
        ReleaseDirectD3D9SharedRing();

        // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization) - zero-initialized placeholder; enum fields are assigned before use
        D3DDEVICE_CREATION_PARAMETERS params = {};
        if (FAILED(device->GetCreationParameters(&params))) {
            HookLogImportant("DX9: Direct D3D9 shared ring unavailable - GetCreationParameters failed");
            return false;
        }

        const D3DFORMAT preferredSharedFormat = d3d9SharedFormat;
        D3DFORMAT sharedFormatCandidates[2] = {preferredSharedFormat, d3d9Format};
        const int candidateCount = (d3d9Format != D3DFMT_UNKNOWN && d3d9Format != preferredSharedFormat) ? 2 : 1;

        for (int candidateIndex = 0; candidateIndex < candidateCount; ++candidateIndex) {
            d3d9SharedFormat = sharedFormatCandidates[candidateIndex];
            if (candidateIndex > 0) {
                HookLogImportant("DX9: Retrying direct shared ring with native backbuffer format %s/%d",
                                 D3D9FormatName(d3d9SharedFormat), (int)d3d9SharedFormat);
            }

            LogDirectD3D9SharingDiagnostics(device, params, "game device");

            // Some WDDM drivers expose the sharing caps bit through a classic
            // device but reject shared creation/opening with D3DERR_INVALIDCALL.
            // Probe the actual classic device first so an unsupported runtime
            // does not pay for helpers that the game device cannot open.
            const bool nativeProbeOk = ProbeDirectD3D9SharedTexture(device, "game device");
            if (!isD3D9Ex && !nativeProbeOk) {
                HookLogImportant(
                    "DX9: Classic device rejected shared-resource creation for %s/%d; skipping helper producers",
                    D3D9FormatName(d3d9SharedFormat), (int)d3d9SharedFormat);
                CleanupSharedHandles();
                ReleaseDirectD3D9SharedRing();
                continue;
            }

            if (EnsureDirectD3D9ExProducerDevice(params)) {
                // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization) - zero-initialized placeholder; enum fields are assigned before use
                D3DDEVICE_CREATION_PARAMETERS helperParams = {};
                if (SUCCEEDED(directSharedProducerDeviceEx->GetCreationParameters(&helperParams))) {
                    LogDirectD3D9SharingDiagnostics(directSharedProducerDeviceEx, helperParams,
                                                    "helper D3D9Ex producer");
                }
                if (TrySetupDirectD3D9SharedRingWithProducer(device, directSharedProducerDeviceEx, true,
                                                             "helper D3D9Ex producer")) {
                    return true;
                }
            }

            if (EnsureDirectD3D9LegacyProducerDevice(params)) {
                // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization) - zero-initialized placeholder; enum fields are assigned before use
                D3DDEVICE_CREATION_PARAMETERS helperParams = {};
                if (SUCCEEDED(directSharedProducerDevice->GetCreationParameters(&helperParams))) {
                    LogDirectD3D9SharingDiagnostics(directSharedProducerDevice, helperParams,
                                                    "helper legacy D3D9 producer");
                }
                if (TrySetupDirectD3D9SharedRingWithProducer(device, directSharedProducerDevice, true,
                                                             "helper legacy D3D9 producer")) {
                    return true;
                }
            }

            // Native ownership is a last resort: a helper-owned shared resource
            // can survive release of the game device's default-pool view during
            // Reset while queued media frames finish consuming the old generation.
            if (nativeProbeOk) {
                const char* nativeProducerLabel = isD3D9Ex ? "native D3D9Ex producer" : "native D3D9 producer";
                if (TrySetupDirectD3D9SharedRingWithProducer(device, device, false, nativeProducerLabel)) {
                    HookLogImportant(
                        "DX9: Native shared-ring ownership active; reset-time generation retention may "
                        "require an immediate helper handoff");
                    return true;
                }
            }

            CleanupSharedHandles();
            ReleaseDirectD3D9SharedRing();
        }

        d3d9SharedFormat = preferredSharedFormat;

        HookLogImportant("DX9: Direct D3D9 shared ring unavailable after all producer attempts");
        return false;

}


bool DX9Capture::EnsureNativeDirectRingRetirementOwner() {


        if (!useDirectD3D9SharedRing || directSharedUsesHelperProducer)
            return true;
        if (!d3d9Device)
            return false;

// NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization) - zero-initialized placeholder; enum fields are assigned before use
        // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization) - zero-initialized placeholder; enum fields are assigned before use
        D3DDEVICE_CREATION_PARAMETERS params = {};
        if (FAILED(d3d9Device->GetCreationParameters(&params)))
            return false;

        IDirect3DDevice9* ownerDevice = nullptr;
        if (EnsureDirectD3D9ExProducerDevice(params)) {
            ownerDevice = directSharedProducerDeviceEx;
        } else if (EnsureDirectD3D9LegacyProducerDevice(params)) {
            ownerDevice = directSharedProducerDevice;
        }
        if (!ownerDevice)
            return false;

        for (int i = 0; i < CAPTURE_TEXTURE_COUNT; ++i) {
            HANDLE openHandle = sharedTextureHandles[i].load(std::memory_order_acquire);
            if (!openHandle || !directSharedTextures9[i])
                return false;

            IDirect3DTexture9* retirementOwner = nullptr;
            const HRESULT openHr = ownerDevice->CreateTexture(width, height, 1, D3DUSAGE_RENDERTARGET, d3d9SharedFormat,
                                                              D3DPOOL_DEFAULT, &retirementOwner, &openHandle);
            if (FAILED(openHr) || !retirementOwner) {
                for (auto*& owner : directSharedProducerTextures9) {
                    if (owner) {
                        owner->Release();
                        owner = nullptr;
                    }
                }
                HookLogImportant(
                    "DX9: Failed to hand native shared-ring generation to helper owner "
                    "before Reset (slot=%d hr=0x%08x)",
                    i, (unsigned)openHr);
                return false;
            }
            directSharedProducerTextures9[i] = retirementOwner;
        }

        directSharedUsesHelperProducer = true;
        HookLogImportant("DX9: Native shared-ring generation handed to helper owner for nonblocking Reset");
        return true;

}

