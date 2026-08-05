#include "injection_internal.h"

void InjectionManager::RescanExistingProcesses() {
    ScanExistingProcesses();
}

bool InjectionManager::IsWhitelisted(const std::string& processName) {
    std::string lowerName = processName;
    std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });

    std::lock_guard<std::mutex> configLock(configMutex);

    // Strict Whitelist Mode (WMI only injects explicit whitelist entries)
    if (config.gameWhitelist.empty() && config.overlayWhitelist.empty()) {
        return false;
    }

    // Check Game Whitelist
    for (const auto& entry : config.gameWhitelist) {
        // Window-only entries don't apply to injection (no window handle available here)
        if (!entry.HasProcess())
            continue;

        std::string lowerItem = entry.pattern;
        std::transform(lowerItem.begin(), lowerItem.end(), lowerItem.begin(),
                       [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });

        if (entry.mode == MatchMode::kExact) {
            if (lowerName == lowerItem) {
                LogInfo("[WMI] Whitelist match (Exact): %s matches %s", processName.c_str(), entry.pattern.c_str());
                return true;
            }
        } else {
            // title_executable or title_type: exact match or substring match
            if (lowerName == lowerItem) {
                LogInfo("[WMI] Whitelist match (Exact): %s matches %s", processName.c_str(), entry.pattern.c_str());
                return true;
            }
            if (lowerName.find(lowerItem) != std::string::npos) {
                LogInfo("[WMI] Whitelist match (Partial): %s matches %s", processName.c_str(), entry.pattern.c_str());
                return true;
            }
        }
    }

    // Check Overlay Whitelist
    for (const auto& entry : config.overlayWhitelist) {
        if (!entry.HasProcess())
            continue;

        std::string lowerItem = entry.pattern;
        std::transform(lowerItem.begin(), lowerItem.end(), lowerItem.begin(),
                       [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });

        if (entry.mode == MatchMode::kExact) {
            if (lowerName == lowerItem) {
                LogInfo("[WMI] Overlay target match (Exact): %s matches %s", processName.c_str(),
                        entry.pattern.c_str());
                return true;
            }
        } else {
            if (lowerName == lowerItem) {
                LogInfo("[WMI] Overlay target match (Exact): %s matches %s", processName.c_str(),
                        entry.pattern.c_str());
                return true;
            }
            if (lowerName.find(lowerItem) != std::string::npos) {
                LogInfo("[WMI] Overlay target match (Partial): %s matches %s", processName.c_str(),
                        entry.pattern.c_str());
                return true;
            }
        }
    }
    return false;
}

bool InjectionManager::IsAlreadyInjected(DWORD pid) {
    std::lock_guard<std::mutex> lock(injectMutex);
    return IsAlreadyInjectedLocked(pid);
}

bool InjectionManager::IsAlreadyInjectedLocked(DWORD pid) {
    // Caller must hold injectMutex

    // First check our in-memory tracking
    for (const auto& p : injectedProcesses) {
        if (p.pid == pid)
            return true;
    }

    // Also check if hook DLL is already loaded in the target process
    // This handles the case where a previous captureengine instance injected
    // and we're a new instance that doesn't know about it
    HANDLE hProcess = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!hProcess)
        return false;

    bool found = false;
    std::vector<HMODULE> hMods;
    if (ce::EnumerateProcessModules(hProcess, hMods)) {
        for (size_t i = 0; i < hMods.size(); i++) {
            char szModName[MAX_PATH];
            if (GetModuleFileNameExA(hProcess, hMods[i], szModName, sizeof(szModName))) {
                std::string modName = szModName;
                if (modName.find("capture_hook_x64.dll") != std::string::npos ||
                    modName.find("capture_hook_x86.dll") != std::string::npos) {
                    found = true;
                    break;
                }
            }
        }
    }
    CloseHandle(hProcess);
    return found;
}

bool InjectionManager::IsAlreadyPendingLocked(DWORD pid) {
    return std::any_of(pendingInjections.begin(), pendingInjections.end(),
                       [pid](const PendingInjection& pending) { return pending.pid == pid; });
}

void InjectionManager::Update() {
    std::lock_guard<std::mutex> lock(injectMutex);

    // Cleanup dead processes
    injectedProcesses.erase(
        std::remove_if(injectedProcesses.begin(), injectedProcesses.end(),
                       [](const InjectedProcess& p) {
                           DWORD exitCode;
                           if (GetExitCodeProcess(p.hProcess, &exitCode) && exitCode != STILL_ACTIVE) {
                               LogInfo(
                                   "[Inject] Tracked injected process exited: %s (PID: %lu, exit=0x%08lX). If no "
                                   "session dump exists, the process ended outside CE's in-process crash/dump path.",
                                   p.name.c_str(), (unsigned long)p.pid, (unsigned long)exitCode);
                               CloseHandle(p.hProcess);
                               return true;
                           }
                           return false;
                       }),
        injectedProcesses.end());

    // Cleanup old failed injections (expire after 30 seconds)
    uint64_t now = GetTickCount64();
    failedInjections.erase(std::remove_if(failedInjections.begin(), failedInjections.end(),
                                          [now](const FailedInjection& f) { return (now - f.timestamp) > 30000; }),
                           failedInjections.end());

    ReapCompletedDelayedInjectionThreadsLocked();

    // Process pending injections
    auto it = pendingInjections.begin();
    while (it != pendingInjections.end()) {
        if (now >= it->injectTime) {
            DWORD pid = it->pid;
            std::string name = it->name;

            // Re-verify it's still running and not injected
            if (!IsWhitelisted(name)) {
                LogInfo("[%s] Skipping deferred injection for %s (PID: %lu) - no longer whitelisted",
                        it->source.c_str(), name.c_str(), (unsigned long)pid);
            } else if (ce::injection_policy::ShouldLaunchPendingInjection(true, IsAlreadyInjectedLocked(pid),
                                                                          IsRecentlyFailedLocked(pid))) {
                LogInfo("[%s] Launching deferred injection thread for %s (PID: %lu)", it->source.c_str(), name.c_str(),
                        (unsigned long)pid);
                LaunchDelayedInjectionThread(pid, name, it->source.c_str());
            }
            it = pendingInjections.erase(it);
        } else {
            ++it;
        }
    }
}

// WMI Implementation
bool InjectionManager::InitializeWMI() {
    HRESULT hres;
    const int64_t initStartUs = Log_GetQpcUs();
    int64_t coInitUs = 0;
    int64_t securityUs = 0;
    int64_t locatorUs = 0;
    int64_t connectUs = 0;
    int64_t proxyBlanketUs = 0;
    int64_t sinkSetupUs = 0;
    int64_t notificationUs = 0;

    // Initialize COM
    int64_t phaseStartUs = Log_GetQpcUs();
    hres = CoInitializeEx(0, COINIT_MULTITHREADED);
    coInitUs = Log_GetQpcUs() - phaseStartUs;
    if (FAILED(hres)) {
        LogInfo("[StartupPerf] InitializeWMI failed at CoInitializeEx after %.3f ms (hr=0x%lX)", injection_QpcDeltaToMs(coInitUs),
                (unsigned long)hres);
        LogError("Failed to initialize COM library. Error code = 0x%lX", hres);
        return false;
    }
    // S_FALSE is still a successful COM initialization call and must be
    // balanced exactly once on this thread.
    wmiCoInitNeedsUninitialize = true;

    // Initialize Security
    phaseStartUs = Log_GetQpcUs();
    hres = CoInitializeSecurity(NULL,
                                -1,                           // COM authentication
                                NULL,                         // Authentication services
                                NULL,                         // Reserved
                                RPC_C_AUTHN_LEVEL_DEFAULT,    // Default authentication
                                RPC_C_IMP_LEVEL_IMPERSONATE,  // Default Impersonation
                                NULL,                         // Authentication info
                                EOAC_NONE,                    // Additional capabilities
                                NULL                          // Reserved
    );
    securityUs = Log_GetQpcUs() - phaseStartUs;

    if (FAILED(hres) && hres != RPC_E_TOO_LATE) {
        LogInfo("[StartupPerf] InitializeWMI failed at CoInitializeSecurity after %.3f ms (hr=0x%lX)",
                injection_QpcDeltaToMs(securityUs), (unsigned long)hres);
        LogError("Failed to initialize security. Error code = 0x%lX", hres);
        return false;  // Don't return false if RPC_E_TOO_LATE (already init)
    }

    // Obtain the initial locator to WMI
    phaseStartUs = Log_GetQpcUs();
    hres = CoCreateInstance(CLSID_WbemLocator, 0, CLSCTX_INPROC_SERVER, IID_IWbemLocator, (LPVOID*)&pLoc);
    locatorUs = Log_GetQpcUs() - phaseStartUs;

    if (FAILED(hres)) {
        LogInfo("[StartupPerf] InitializeWMI failed at CoCreateInstance after %.3f ms (hr=0x%lX)",
                injection_QpcDeltaToMs(locatorUs), (unsigned long)hres);
        LogError("Failed to create IWbemLocator object. Err: 0x%lX", hres);
        return false;
    }

    // Connect to WMI
    phaseStartUs = Log_GetQpcUs();
    const BstrGuard namespaceName(L"ROOT\\CIMV2");
    if (!namespaceName.valid()) {
        LogError("Failed to allocate WMI namespace BSTR");
        pLoc->Release();
        pLoc = nullptr;
        return false;
    }

    hres = pLoc->ConnectServer(namespaceName,  // Object path of WMI namespace
                               NULL,           // User name
                               NULL,           // User password
                               0,              // Locale
                               NULL,           // Security flags
                               0,              // Authority
                               0,              // Context object
                               &pSvc           // IWbemServices proxy
    );
    connectUs = Log_GetQpcUs() - phaseStartUs;

    if (FAILED(hres)) {
        LogInfo("[StartupPerf] InitializeWMI failed at ConnectServer after %.3f ms (hr=0x%lX)", injection_QpcDeltaToMs(connectUs),
                (unsigned long)hres);
        LogError("Could not connect. Error code = 0x%lX", hres);
        pLoc->Release();
        pLoc = nullptr;
        return false;
    }

    // Set security levels on the proxy
    phaseStartUs = Log_GetQpcUs();
    hres = CoSetProxyBlanket(pSvc,                         // Indicates the proxy to set
                             RPC_C_AUTHN_WINNT,            // RPC_C_AUTHN_xxx
                             RPC_C_AUTHZ_NONE,             // RPC_C_AUTHZ_xxx
                             NULL,                         // Server principal name
                             RPC_C_AUTHN_LEVEL_CALL,       // RPC_C_AUTHN_LEVEL_xxx
                             RPC_C_IMP_LEVEL_IMPERSONATE,  // RPC_C_IMP_LEVEL_xxx
                             NULL,                         // client identity
                             EOAC_NONE                     // proxy capabilities
    );
    proxyBlanketUs = Log_GetQpcUs() - phaseStartUs;

    if (FAILED(hres)) {
        LogInfo("[StartupPerf] InitializeWMI failed at CoSetProxyBlanket after %.3f ms (hr=0x%lX)",
                injection_QpcDeltaToMs(proxyBlanketUs), (unsigned long)hres);
        LogError("Could not set proxy blanket. Error code = 0x%lX", hres);
        pSvc->Release();
        pSvc = nullptr;
        pLoc->Release();
        pLoc = nullptr;
        return false;
    }

    // Setup Unsecured Apartment for async callbacks
    phaseStartUs = Log_GetQpcUs();
    hres = CoCreateInstance(CLSID_UnsecuredApartment, NULL, CLSCTX_LOCAL_SERVER, IID_IUnsecuredApartment,
                            (void**)&pUnsecApp);
    if (FAILED(hres)) {
        LogError("Failed to create UnsecuredApartment: 0x%lX", hres);
        // Continue anyway? Callbacks might fail permission checks without it
    }

    // Create Event Sink
    pSink = new ProcessEventSink(this);
    pSink->AddRef();

    // Create Stub Sink
    if (pUnsecApp) {
        hres = pUnsecApp->CreateObjectStub(pSink, (IUnknown**)&pStubSink);
        if (FAILED(hres)) {
            LogError("CreateObjectStub failed: 0x%lX", hres);
            // Fallback?
            pStubSink = pSink;
            pStubSink->AddRef();
        }
    } else {
        pStubSink = pSink;
        pStubSink->AddRef();
    }
    sinkSetupUs = Log_GetQpcUs() - phaseStartUs;

    // Exec Notification Query
    // Use WITHIN 0.5 to reduce idle WMI churn while still reacting to launches
    // within half a second.
    phaseStartUs = Log_GetQpcUs();
    const BstrGuard queryLanguage(L"WQL");
    const BstrGuard queryText(
        L"SELECT * FROM __InstanceCreationEvent WITHIN 0.5 WHERE "
        L"TargetInstance ISA 'Win32_Process'");
    if (!queryLanguage.valid() || !queryText.valid()) {
        LogError("Failed to allocate WMI query BSTR");
        return false;
    }

    hres = pSvc->ExecNotificationQueryAsync(queryLanguage, queryText, WBEM_FLAG_SEND_STATUS, NULL, pStubSink);
    notificationUs = Log_GetQpcUs() - phaseStartUs;

    if (FAILED(hres)) {
        LogInfo("[StartupPerf] InitializeWMI failed at ExecNotificationQueryAsync after %.3f ms (hr=0x%lX)",
                injection_QpcDeltaToMs(notificationUs), (unsigned long)hres);
        LogError("ExecNotificationQueryAsync failed. Error code = 0x%lX", hres);
        return false;
    }

    LogInfo(
        "[StartupPerf] InitializeWMI: CoInitializeEx=%.3f ms, CoInitializeSecurity=%.3f ms, "
        "CoCreateInstance=%.3f ms, ConnectServer=%.3f ms, CoSetProxyBlanket=%.3f ms, SinkSetup=%.3f ms, "
        "NotificationQuery=%.3f ms, total=%.3f ms",
        injection_QpcDeltaToMs(coInitUs), injection_QpcDeltaToMs(securityUs), injection_QpcDeltaToMs(locatorUs), injection_QpcDeltaToMs(connectUs),
        injection_QpcDeltaToMs(proxyBlanketUs), injection_QpcDeltaToMs(sinkSetupUs), injection_QpcDeltaToMs(notificationUs),
        injection_QpcDeltaToMs(Log_GetQpcUs() - initStartUs));
    LogInfo("WMI Event Sink Initialized");
    return true;
}

void InjectionManager::ShutdownWMI() {
    // Close the callback lifetime gate first. This rejects callbacks that have
    // not entered yet and synchronously drains callbacks already using the raw
    // manager pointer; CancelAsyncCall itself does not wait for client callback
    // responses.
    if (pSink) {
        pSink->MarkDoneAndDrain();
    }

    if (pSvc) {
        if (pStubSink) {
            const HRESULT cancelHr = pSvc->CancelAsyncCall(pStubSink);
            if (FAILED(cancelHr) && cancelHr != WBEM_E_NOT_FOUND) {
                LogWarn("[Inject] WMI CancelAsyncCall failed during shutdown: 0x%08lX",
                        static_cast<unsigned long>(cancelHr));
            }
        }
        pSvc->Release();
        pSvc = nullptr;
    }
    if (pStubSink) {
        pStubSink->Release();
        pStubSink = nullptr;
    }
    if (pSink) {
        pSink->Release();
        pSink = nullptr;
    }
    if (pUnsecApp) {
        pUnsecApp->Release();
        pUnsecApp = nullptr;
    }
    if (pLoc) {
        pLoc->Release();
        pLoc = nullptr;
    }
    if (wmiCoInitNeedsUninitialize) {
        CoUninitialize();
        wmiCoInitNeedsUninitialize = false;
    }
}

ULONG STDMETHODCALLTYPE InjectionManager::ProcessEventSink::AddRef() {
    return InterlockedIncrement(&m_lRef);
}

ULONG STDMETHODCALLTYPE InjectionManager::ProcessEventSink::Release() {
    LONG lRef = InterlockedDecrement(&m_lRef);
    if (lRef == 0)
        delete this;
    return lRef;
}

HRESULT STDMETHODCALLTYPE InjectionManager::ProcessEventSink::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) {
        return E_POINTER;
    }
    *ppv = nullptr;
    if (riid == IID_IUnknown || riid == IID_IWbemObjectSink) {
        *ppv = (IWbemObjectSink*)this;
        AddRef();
        return WBEM_S_NO_ERROR;
    }
    return E_NOINTERFACE;
}

bool InjectionManager::ProcessEventSink::EnterCallback() {
    std::lock_guard<std::mutex> lock(callbackMutex);
    if (bDone || !pManager) {
        return false;
    }
    ++activeCallbacks;
    return true;
}

void InjectionManager::ProcessEventSink::LeaveCallback() {
    std::lock_guard<std::mutex> lock(callbackMutex);
    if (activeCallbacks > 0) {
        --activeCallbacks;
    }
    if (activeCallbacks == 0) {
        callbacksDrained.notify_all();
    }
}

void InjectionManager::ProcessEventSink::MarkDoneAndDrain() {
    std::unique_lock<std::mutex> lock(callbackMutex);
    bDone = true;
    callbacksDrained.wait(lock, [this]() { return activeCallbacks == 0; });
    pManager = nullptr;
}

HRESULT STDMETHODCALLTYPE InjectionManager::ProcessEventSink::Indicate(LONG lObjectCount, IWbemClassObject __RPC_FAR *
                                                                                              __RPC_FAR * apObjArray) {
    if (!EnterCallback()) {
        return WBEM_S_NO_ERROR;
    }
    CE_SCOPE_EXIT(LeaveCallback());

    // Exception handling: WMI callbacks can throw COM exceptions
    // Catching them prevents crashes and allows graceful degradation
    try {
        if (!pManager)
            return WBEM_S_NO_ERROR;

        for (int i = 0; i < lObjectCount; i++) {
            IWbemClassObject* pObj = apObjArray[i];

            // Get TargetInstance
            _variant_t vTarget;
            if (FAILED(pObj->Get(L"TargetInstance", 0, &vTarget, NULL, NULL)))
                continue;

            IUnknown* pUnk = vTarget;
            IWbemClassObject* pTargetCase = nullptr;
            if (FAILED(pUnk->QueryInterface(IID_IWbemClassObject, (void**)&pTargetCase)))
                continue;

            struct TargetCaseGuard {
                IWbemClassObject* obj;
                ~TargetCaseGuard() {
                    if (obj)
                        obj->Release();
                }
            } guard{pTargetCase};

            // Get Name
            _variant_t vName;
            pTargetCase->Get(L"Name", 0, &vName, NULL, NULL);

            // Get PID
            _variant_t vPid;
            pTargetCase->Get(L"ProcessId", 0, &vPid, NULL, NULL);

            if (vName.vt == VT_BSTR && vPid.vt == VT_I4) {
                std::wstring wName = vName.bstrVal;
                std::string name(wName.begin(), wName.end());
                DWORD pid = vPid.intVal;

                // Check whitelist (thread-safe? IsWhitelisted reads config which is
                // const, so yes)
                if (pManager->IsWhitelisted(name)) {
                    pManager->LaunchDelayedInjectionThread(pid, name, "WMI");
                }
            }
        }
    } catch (const _com_error& e) {
        // COM exception - log and continue gracefully
        LogError("WMI Indicate: COM exception 0x%lX: %s", (unsigned long)e.Error(), e.ErrorMessage());
    } catch (const std::exception& e) {
        // Standard exception
        LogError("WMI Indicate: Exception: %s", e.what());
    } catch (...) {
        // Unknown exception
        LogError("WMI Indicate: Unknown exception caught");
    }

    return WBEM_S_NO_ERROR;
}

HRESULT STDMETHODCALLTYPE InjectionManager::ProcessEventSink::SetStatus(LONG lFlags, HRESULT hResult, BSTR strParam,
                                                                        IWbemClassObject __RPC_FAR* pObjParam) {
    return WBEM_S_NO_ERROR;
}

bool InjectionManager::IsRecentlyFailed(DWORD pid) {
    std::lock_guard<std::mutex> lock(injectMutex);
    return IsRecentlyFailedLocked(pid);
}

void InjectionManager::ReapCompletedDelayedInjectionThreadsLocked() {
    std::lock_guard<std::mutex> threadLock(threadListMutex);
    for (auto it = delayedInjectionThreads.begin(); it != delayedInjectionThreads.end();) {
        if (!it->joinable()) {
            it = delayedInjectionThreads.erase(it);
            continue;
        }

        HANDLE existingThreadHandle = ce::Win32ThreadHandle(*it);
        if (WaitForSingleObject(existingThreadHandle, 0) == WAIT_OBJECT_0) {
            it->join();
            it = delayedInjectionThreads.erase(it);
        } else {
            ++it;
        }
    }
}

void InjectionManager::LaunchDelayedInjectionThread(DWORD pid, const std::string& name, const char* sourceTag) {
    std::string source = sourceTag ? sourceTag : "Inject";
    std::lock_guard<std::mutex> threadLock(threadListMutex);
    if (IsShuttingDown()) {
        LogInfo("[%s] Skipping delayed injection launch during shutdown for %s (PID: %lu)", source.c_str(),
                name.c_str(), static_cast<unsigned long>(pid));
        return;
    }

    try {
        // NOLINTNEXTLINE(bugprone-exception-escape) - lambda body already catches all exceptions below
        delayedInjectionThreads.emplace_back([this, pid, name, source]() {
            try {
                LogInfo("[%s] %s (PID: %lu) - Waiting for graphics API initialization before injection...",
                        source.c_str(), name.c_str(), (unsigned long)pid);

                bool ready = false;
                bool d3d12Loaded = false;
                bool loggedModuleEnumFailure = false;
                int waitMs = 0;
                for (int i = 0; i < 300 && !ready; i++) {
                    if (IsShuttingDown()) {
                        LogInfo("[%s] %s (PID: %lu) - Shutdown requested, aborting delayed injection", source.c_str(),
                                name.c_str(), (unsigned long)pid);
                        return;
                    }

                    HANDLE hProcess =
                        OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | SYNCHRONIZE, FALSE, pid);
                    if (!hProcess) {
                        LogInfo("[%s] %s (PID: %lu) - Process exited before injection (OpenProcess failed, error=%lu)",
                                source.c_str(), name.c_str(), (unsigned long)pid, (unsigned long)GetLastError());
                        return;
                    }

                    DWORD exitCode = 0;
                    if (GetExitCodeProcess(hProcess, &exitCode) && exitCode != STILL_ACTIVE) {
                        CloseHandle(hProcess);
                        LogInfo("[%s] %s (PID: %lu) - Process exited before injection (exit code=%lu)", source.c_str(),
                                name.c_str(), (unsigned long)pid, (unsigned long)exitCode);
                        return;
                    }

                    if (!d3d12Loaded) {
                        std::vector<HMODULE> hMods;
                        if (ce::EnumerateProcessModules(hProcess, hMods)) {
                            for (size_t j = 0; j < hMods.size(); j++) {
                                char szModName[MAX_PATH];
                                if (GetModuleFileNameExA(hProcess, hMods[j], szModName, sizeof(szModName))) {
                                    if (strstr(szModName, "d3d12.dll")) {
                                        d3d12Loaded = true;
                                        LogInfo(
                                            "[%s] %s (PID: %lu) - D3D12.dll detected, injecting without fixed delay...",
                                            source.c_str(), name.c_str(), (unsigned long)pid);
                                        break;
                                    }
                                }
                            }
                        } else {
                            DWORD err = GetLastError();
                            if (!loggedModuleEnumFailure) {
                                LogInfo(
                                    "[%s] %s (PID: %lu) - EnumProcessModules failed (error=%lu, access=0x%lX); "
                                    "continuing with conservative non-D3D12 injection timing",
                                    source.c_str(), name.c_str(), (unsigned long)pid, (unsigned long)err,
                                    (unsigned long)(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | SYNCHRONIZE));
                                loggedModuleEnumFailure = true;
                            }
                            if (err == ERROR_ACCESS_DENIED && i < 2) {
                                CloseHandle(hProcess);
                                Sleep(100);
                                waitMs += 100;
                                continue;
                            }
                        }
                    }
                    CloseHandle(hProcess);

                    if (ce::injection_policy::ShouldInjectAfterGraphicsProbe(d3d12Loaded)) {
                        ready = true;
                    }

                    if (!ready) {
                        Sleep(100);
                        waitMs += 100;
                    }
                }

                LogInfo("[%s] %s (PID: %lu) - Wait loop exited (ready=%d, d3d12=%d, waitMs=%d), attempting injection",
                        source.c_str(), name.c_str(), (unsigned long)pid, (int)ready, (int)d3d12Loaded, waitMs);

                std::lock_guard<std::mutex> lock(injectMutex);
                if (!IsWhitelisted(name)) {
                    LogInfo("[%s] %s (PID: %lu) - No longer whitelisted, skipping injection", source.c_str(),
                            name.c_str(), (unsigned long)pid);
                } else if (!IsAlreadyInjectedLocked(pid) && !IsRecentlyFailedLocked(pid)) {
                    LogInfo("[%s] %s (PID: %lu) - Injecting (%s detected, waited %d ms)", source.c_str(), name.c_str(),
                            (unsigned long)pid, d3d12Loaded ? "D3D12" : "non-D3D12 (DX11/DX9/Vulkan)", waitMs);
                    if (Inject(pid, name)) {
                        LogInfo("[%s] Injection successful.", source.c_str());
                    } else {
                        LogError("[%s] Injection failed.", source.c_str());
                        if (failedInjections.size() >= 1024) {
                            failedInjections.erase(failedInjections.begin());
                        }
                        failedInjections.push_back({pid, GetTickCount64()});
                    }
                } else {
                    LogInfo("[%s] %s (PID: %lu) - Already injected or recently failed, skipping", source.c_str(),
                            name.c_str(), (unsigned long)pid);
                }
            } catch (const std::exception& e) {
                LogError("[%s] Delayed injection thread exception for PID %lu: %s", source.c_str(), (unsigned long)pid,
                         e.what());
            } catch (...) {
                LogError("[%s] Delayed injection thread unknown exception for PID %lu", source.c_str(),
                         (unsigned long)pid);
            }
        });
    } catch (const std::system_error& error) {
        LogError("[%s] Failed to create delayed injection thread for %s (PID: %lu): %s", source.c_str(), name.c_str(),
                 static_cast<unsigned long>(pid), error.what());
    }
}

bool InjectionManager::IsRecentlyFailedLocked(DWORD pid) {
    // Caller must hold injectMutex
    for (const auto& fail : failedInjections) {
        if (fail.pid == pid)
            return true;
    }
    return false;
}
