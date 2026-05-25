#include "injection.h"
#include "injection_policy.h"
#include <psapi.h>
#include <tlhelp32.h>
#include <algorithm>
#include <array>
#include <filesystem>
#include <iostream>
#include <thread>
#include "../common/logging.h"
#include "../common/raii_helpers.h"

#include <aclapi.h>
#include <bcrypt.h>
#include <ntstatus.h>
#include <oleauto.h>
#include <softpub.h>
#include <wintrust.h>
#include <fstream>
#include <iomanip>
#include <sstream>

#ifdef _MSC_VER
#pragma comment(lib, "wintrust.lib")
#endif

/*
 * ANTI-CHEAT DETECTION LIMITATIONS
 * =================================
 *
 * This injection system uses CreateRemoteThread() for DLL injection, which is:
 *
 * 1. EASILY DETECTABLE by modern anti-cheat systems (EAC, BattlEye, Vanguard,
 * etc.)
 *    - CreateRemoteThread is a well-known injection technique
 *    - Anti-cheat can monitor for remote thread creation
 *    - DLL loading events are tracked by kernel-mode drivers
 *
 * 2. WILL TRIGGER BANS in competitive games with anti-cheat
 *    - Do NOT use this on games with anti-cheat protection
 *    - Intended for single-player games and testing only
 *
 * 3. DETECTION VECTORS:
 *    - CreateRemoteThread API calls
 *    - Unsigned DLL loading (mitigated by signature verification)
 *    - Shared memory with predictable names (mitigated by randomization)
 *    - Hook installation via IAT patching and VTable hooking
 *    - Process memory scanning for known hook patterns
 *
 * 4. SAFER ALTERNATIVES (not implemented):
 *    - Manual mapping (avoids LoadLibrary detection)
 *    - Kernel-mode driver injection (requires signed driver)
 *    - AppInit_DLLs registry (deprecated, requires reboot)
 *    - SetWindowsHookEx (limited to UI thread)
 *
 * USE AT YOUR OWN RISK. This tool is for educational and single-player use
 * only.
 */

namespace fs = std::filesystem;

namespace {
double QpcDeltaToMs(int64_t deltaUs) {
    return static_cast<double>(deltaUs) / 1000.0;
}

constexpr uint64_t kPendingInjectionDelayMs = 1;

struct BstrGuard {
    BSTR value = nullptr;

    explicit BstrGuard(const wchar_t* text) : value(SysAllocString(text)) {}

    ~BstrGuard() {
        if (value) {
            SysFreeString(value);
        }
    }

    BstrGuard(const BstrGuard&) = delete;
    BstrGuard& operator=(const BstrGuard&) = delete;

    operator BSTR() const {
        return value;
    }

    bool valid() const {
        return value != nullptr;
    }
};
}  // namespace

InjectionManager::InjectionManager(const AppConfig& config) : config(config) {
    const int64_t constructorStartUs = Log_GetQpcUs();

    // Determine DLL paths (assume next to exe)
    char buffer[MAX_PATH];
    GetModuleFileNameA(NULL, buffer, MAX_PATH);
    fs::path exePath(buffer);

    hookDllPathX64 = (exePath.parent_path() / "capture_hook_x64.dll").string();
    hookDllPathX86 = (exePath.parent_path() / "capture_hook_x86.dll").string();

    // FIX: Force absolute path resolution to ensure the correct DLL is injected.
    // Relative paths can be ambiguous if the target process has a different CWD.
    try {
        if (fs::exists(hookDllPathX64))
            hookDllPathX64 = fs::absolute(hookDllPathX64).string();
        if (fs::exists(hookDllPathX86))
            hookDllPathX86 = fs::absolute(hookDllPathX86).string();
    } catch (const fs::filesystem_error& e) {
        LogError("Filesystem error resolving absolute paths: %s", e.what());
    }

    // Check if DLLs exist
    if (!fs::exists(hookDllPathX64))
        LogError("Capture Hook X64 DLL not found: %s", hookDllPathX64.c_str());
    if (!fs::exists(hookDllPathX86))
        LogError("Capture Hook X86 DLL not found: %s", hookDllPathX86.c_str());

    const int64_t wmiStartUs = Log_GetQpcUs();
    bool wmiInitialized = InitializeWMI();
    const int64_t wmiTotalUs = Log_GetQpcUs() - wmiStartUs;

    const int64_t scanStartUs = Log_GetQpcUs();
    ScanExistingProcesses();
    const int64_t scanTotalUs = Log_GetQpcUs() - scanStartUs;

    LogInfo(
        "[StartupPerf] InjectionManager startup: InitializeWMI=%.3f ms (ok=%d), ScanExistingProcesses=%.3f ms, "
        "total=%.3f ms",
        QpcDeltaToMs(wmiTotalUs), wmiInitialized ? 1 : 0, QpcDeltaToMs(scanTotalUs),
        QpcDeltaToMs(Log_GetQpcUs() - constructorStartUs));
}

InjectionManager::~InjectionManager() {
    // CRITICAL FIX: Signal all delayed injection threads to stop
    RequestShutdown();
    WaitForInjectionThreads(5000);

    ShutdownWMI();
    EjectAll();
}

void InjectionManager::SetOnInjectCallback(std::function<void(const std::string&)> callback) {
    this->onInjectCallback = callback;
}

void InjectionManager::UpdateConfig(const AppConfig& newConfig) {
    std::lock_guard<std::mutex> lock(configMutex);
    config = newConfig;
}

void InjectionManager::RescanExistingProcesses() {
    ScanExistingProcesses();
}

bool InjectionManager::IsWhitelisted(const std::string& processName) {
    std::string lowerName = processName;
    std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(), ::tolower);

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
        std::transform(lowerItem.begin(), lowerItem.end(), lowerItem.begin(), ::tolower);

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
        std::transform(lowerItem.begin(), lowerItem.end(), lowerItem.begin(), ::tolower);

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
    HMODULE hMods[1024];
    DWORD cbNeeded;
    if (EnumProcessModules(hProcess, hMods, sizeof(hMods), &cbNeeded)) {
        for (unsigned int i = 0; i < (cbNeeded / sizeof(HMODULE)); i++) {
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
            } else if (!IsAlreadyInjected(pid) && !IsRecentlyFailed(pid)) {
                std::shared_ptr<InjectionManager> managerShared = shared_from_this();
                LaunchDelayedInjectionThread(managerShared, pid, name, it->source.c_str());
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
        LogInfo("[StartupPerf] InitializeWMI failed at CoInitializeEx after %.3f ms (hr=0x%lX)", QpcDeltaToMs(coInitUs),
                (unsigned long)hres);
        LogError("Failed to initialize COM library. Error code = 0x%lX", hres);
        return false;
    }

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
                QpcDeltaToMs(securityUs), (unsigned long)hres);
        LogError("Failed to initialize security. Error code = 0x%lX", hres);
        return false;  // Don't return false if RPC_E_TOO_LATE (already init)
    }

    // Obtain the initial locator to WMI
    phaseStartUs = Log_GetQpcUs();
    hres = CoCreateInstance(CLSID_WbemLocator, 0, CLSCTX_INPROC_SERVER, IID_IWbemLocator, (LPVOID*)&pLoc);
    locatorUs = Log_GetQpcUs() - phaseStartUs;

    if (FAILED(hres)) {
        LogInfo("[StartupPerf] InitializeWMI failed at CoCreateInstance after %.3f ms (hr=0x%lX)",
                QpcDeltaToMs(locatorUs), (unsigned long)hres);
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
        LogInfo("[StartupPerf] InitializeWMI failed at ConnectServer after %.3f ms (hr=0x%lX)", QpcDeltaToMs(connectUs),
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
                QpcDeltaToMs(proxyBlanketUs), (unsigned long)hres);
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
                QpcDeltaToMs(notificationUs), (unsigned long)hres);
        LogError("ExecNotificationQueryAsync failed. Error code = 0x%lX", hres);
        return false;
    }

    LogInfo(
        "[StartupPerf] InitializeWMI: CoInitializeEx=%.3f ms, CoInitializeSecurity=%.3f ms, "
        "CoCreateInstance=%.3f ms, ConnectServer=%.3f ms, CoSetProxyBlanket=%.3f ms, SinkSetup=%.3f ms, "
        "NotificationQuery=%.3f ms, total=%.3f ms",
        QpcDeltaToMs(coInitUs), QpcDeltaToMs(securityUs), QpcDeltaToMs(locatorUs), QpcDeltaToMs(connectUs),
        QpcDeltaToMs(proxyBlanketUs), QpcDeltaToMs(sinkSetupUs), QpcDeltaToMs(notificationUs),
        QpcDeltaToMs(Log_GetQpcUs() - initStartUs));
    LogInfo("WMI Event Sink Initialized");
    return true;
}

void InjectionManager::ShutdownWMI() {
    // Mark the sink as done FIRST so any in-flight callback returns immediately
    if (pSink) {
        pSink->MarkDone();
    }

    if (pSvc) {
        pSvc->CancelAsyncCall(pStubSink);
        // Brief delay to let thread pool drain any WMI callbacks already queued.
        // CancelAsyncCall prevents NEW notifications but callbacks already in the
        // LRPC thread pool queue can still dispatch. 100ms is sufficient for the
        // thread pool to complete any pending dispatch.
        Sleep(100);
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
    CoUninitialize();
}

// Event Sink Implementation
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
    if (riid == IID_IUnknown || riid == IID_IWbemObjectSink) {
        *ppv = (IWbemObjectSink*)this;
        AddRef();
        return WBEM_S_NO_ERROR;
    }
    return E_NOINTERFACE;
}

HRESULT STDMETHODCALLTYPE InjectionManager::ProcessEventSink::Indicate(LONG lObjectCount, IWbemClassObject __RPC_FAR *
                                                                                              __RPC_FAR * apObjArray) {
    // Exception handling: WMI callbacks can throw COM exceptions
    // Catching them prevents crashes and allows graceful degradation
    try {
        if (bDone.load(std::memory_order_acquire) || !pManager)
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
                    std::shared_ptr<InjectionManager> managerShared = pManager->shared_from_this();
                    pManager->LaunchDelayedInjectionThread(managerShared, pid, name, "WMI");
                }
            }

            pTargetCase->Release();
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

        HANDLE existingThreadHandle = reinterpret_cast<HANDLE>(it->native_handle());
        if (WaitForSingleObject(existingThreadHandle, 0) == WAIT_OBJECT_0) {
            it->join();
            it = delayedInjectionThreads.erase(it);
        } else {
            ++it;
        }
    }
}

void InjectionManager::LaunchDelayedInjectionThread(const std::shared_ptr<InjectionManager>& managerShared, DWORD pid,
                                                    const std::string& name, const char* sourceTag) {
    std::string source = sourceTag ? sourceTag : "Inject";
    std::thread delayedThread([managerShared, pid, name, source]() {
        try {
            LogInfo("[%s] %s (PID: %lu) - Waiting for graphics API initialization before injection...", source.c_str(),
                    name.c_str(), (unsigned long)pid);

            bool ready = false;
            bool d3d12Loaded = false;
            bool loggedModuleEnumFailure = false;
            int waitMs = 0;
            for (int i = 0; i < 300 && !ready; i++) {
                if (managerShared->IsShuttingDown()) {
                    LogInfo("[%s] %s (PID: %lu) - Shutdown requested, aborting delayed injection", source.c_str(),
                            name.c_str(), (unsigned long)pid);
                    return;
                }

                HANDLE hProcess = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | SYNCHRONIZE, FALSE, pid);
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
                    HMODULE hMods[1024];
                    DWORD cbNeeded;
                    if (EnumProcessModules(hProcess, hMods, sizeof(hMods), &cbNeeded)) {
                        for (unsigned int j = 0; j < (cbNeeded / sizeof(HMODULE)); j++) {
                            char szModName[MAX_PATH];
                            if (GetModuleFileNameExA(hProcess, hMods[j], szModName, sizeof(szModName))) {
                                if (strstr(szModName, "d3d12.dll")) {
                                    d3d12Loaded = true;
                                    LogInfo("[%s] %s (PID: %lu) - D3D12.dll detected, injecting without fixed delay...",
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

            std::lock_guard<std::mutex> lock(managerShared->injectMutex);
            if (!managerShared->IsWhitelisted(name)) {
                LogInfo("[%s] %s (PID: %lu) - No longer whitelisted, skipping injection", source.c_str(), name.c_str(),
                        (unsigned long)pid);
            } else if (!managerShared->IsAlreadyInjectedLocked(pid) && !managerShared->IsRecentlyFailedLocked(pid)) {
                LogInfo("[%s] %s (PID: %lu) - Injecting (%s detected, waited %d ms)", source.c_str(), name.c_str(),
                        (unsigned long)pid, d3d12Loaded ? "D3D12" : "non-D3D12 (DX11/DX9/Vulkan)", waitMs);
                if (managerShared->Inject(pid, name)) {
                    LogInfo("[%s] Injection successful.", source.c_str());
                } else {
                    LogError("[%s] Injection failed.", source.c_str());
                    managerShared->failedInjections.push_back({pid, GetTickCount64()});
                }
            } else {
                LogInfo("[%s] %s (PID: %lu) - Already injected or recently failed, skipping", source.c_str(),
                        name.c_str(), (unsigned long)pid);
            }
        } catch (const std::exception& e) {
            LogError("[%s] Delayed injection thread exception for PID %lu: %s", source.c_str(), (unsigned long)pid,
                     e.what());
        } catch (...) {
            LogError("[%s] Delayed injection thread unknown exception for PID %lu", source.c_str(), (unsigned long)pid);
        }
    });

    std::lock_guard<std::mutex> threadLock(threadListMutex);
    delayedInjectionThreads.emplace_back(std::move(delayedThread));
}

bool InjectionManager::IsRecentlyFailedLocked(DWORD pid) {
    // Caller must hold injectMutex
    for (const auto& fail : failedInjections) {
        if (fail.pid == pid)
            return true;
    }
    return false;
}

bool InjectionManager::Inject(DWORD pid, const std::string& processName) {
    // Execute callback if set (e.g. to reload config for this specific process)
    if (onInjectCallback) {
        LogInfo("[Inject] Executing pre-injection callback for %s", processName.c_str());
        onInjectCallback(processName);
    }

    // Determine architecture - use RAII HandleGuard to prevent leaks
    ce::HandleGuard hProcess(OpenProcess(
        PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_VM_OPERATION | PROCESS_CREATE_THREAD,
        FALSE, pid));
    if (!hProcess) {
        LogError("Failed to open process %lu for injection", (unsigned long)pid);
        return false;
    }

    BOOL isWow64 = FALSE;
    IsWow64Process(hProcess.get(), &isWow64);

    std::string dllPath = isWow64 ? hookDllPathX86 : hookDllPathX64;

    // SECURITY: Verify DLL integrity before injection
    // This prevents injection of tampered DLLs and protects against DLL hijacking
    //
    // CE_PRODUCTION_BUILD is defined only for signed production releases.
    // Dev/CI builds do NOT define it and fall through to the warning path below.
    // To create a production build, pass --production to build.py.
#ifdef CE_PRODUCTION_BUILD
    // PRODUCTION BUILD: Require valid Authenticode signature
    if (!VerifyDLLSignature(dllPath, true)) {
        LogError(
            "[SECURITY] DLL signature verification failed for %s - refusing "
            "to inject",
            dllPath.c_str());
        LogError(
            "[SECURITY] In production builds, only properly signed DLLs can "
            "be injected");
        return false;
    }
    LogInfo("[SECURITY] DLL signature verified: %s", dllPath.c_str());
#else
    // DEVELOPMENT BUILD: Warn about unsigned DLL but allow injection.
    // SKIP_DLL_VERIFICATION=1 bypasses signature checks for local dev.
    // This env var is never read in production builds (CE_PRODUCTION_BUILD).
    const char* skipVerification = getenv("SKIP_DLL_VERIFICATION");
    if (skipVerification && strcmp(skipVerification, "1") == 0) {
        LogWarn("[SECURITY] Skipping DLL verification (SKIP_DLL_VERIFICATION=1)");
    } else if (!VerifyDLLSignature(dllPath, false)) {
        LogWarn("[SECURITY] DLL is not Authenticode-signed: %s", dllPath.c_str());
        LogWarn(
            "[SECURITY] This is expected for development builds. Set "
            "CE_PRODUCTION_BUILD to enforce signing.");
    } else {
        LogInfo("[SECURITY] DLL signature verified: %s", dllPath.c_str());
    }
#endif

    LogInfo("Using DLL: %s (WoW64: %d)", dllPath.c_str(), isWow64);

    if (!fs::exists(dllPath)) {
        LogError("Required DLL for %s injection missing: %s", isWow64 ? "x86" : "x64", dllPath.c_str());
        return false;
    }

    // Helper to find remote function address
    auto GetRemoteProcAddress = [&](HANDLE hProc, HMODULE hModule, const char* funcName) -> LPVOID {
        // Read DOS Header
        IMAGE_DOS_HEADER dosHeader;
        if (!ReadProcessMemory(hProc, hModule, &dosHeader, sizeof(dosHeader), NULL))
            return nullptr;
        if (dosHeader.e_magic != IMAGE_DOS_SIGNATURE)
            return nullptr;

        // Read NT Headers (32-bit)
        // Note: We assume target is 32-bit here because this logic is inside 'if
        // (isWow64)' But we should be careful. We can read NT Headers signature
        // first? Let's just read the signature + file header + optional header
        // structure. Since we know we are in isWow64 block, we expect 32-bit
        // headers.

        // Calculate NT Headers address
        BYTE* pNtHeaders = (BYTE*)hModule + dosHeader.e_lfanew;
        IMAGE_NT_HEADERS32 ntHeaders;
        if (!ReadProcessMemory(hProc, pNtHeaders, &ntHeaders, sizeof(ntHeaders), NULL))
            return nullptr;
        if (ntHeaders.Signature != IMAGE_NT_SIGNATURE)
            return nullptr;

        // Get Export Directory RVA and Size
        DWORD exportDirRVA = ntHeaders.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress;
        if (exportDirRVA == 0)
            return nullptr;

        IMAGE_EXPORT_DIRECTORY exportDir;
        if (!ReadProcessMemory(hProc, (BYTE*)hModule + exportDirRVA, &exportDir, sizeof(exportDir), NULL))
            return nullptr;

        // Read Name Table
        std::vector<DWORD> nameRVAs(exportDir.NumberOfNames);
        if (!ReadProcessMemory(hProc, (BYTE*)hModule + exportDir.AddressOfNames, nameRVAs.data(),
                               nameRVAs.size() * sizeof(DWORD), NULL))
            return nullptr;

        // Search for function name
        for (DWORD i = 0; i < exportDir.NumberOfNames; i++) {
            char buffer[256];
            if (ReadProcessMemory(hProc, (BYTE*)hModule + nameRVAs[i], buffer, sizeof(buffer), NULL)) {
                buffer[255] = '\0';  // Ensure null term
                if (strcmp(buffer, funcName) == 0) {
                    // Found name, get ordinal
                    WORD ordinal;
                    if (!ReadProcessMemory(hProc, (BYTE*)hModule + exportDir.AddressOfNameOrdinals + (i * sizeof(WORD)),
                                           &ordinal, sizeof(WORD), NULL))
                        return nullptr;

                    // Get Function RVA
                    DWORD funcRVA;
                    if (!ReadProcessMemory(hProc,
                                           (BYTE*)hModule + exportDir.AddressOfFunctions + (ordinal * sizeof(DWORD)),
                                           &funcRVA, sizeof(DWORD), NULL))
                        return nullptr;

                    return (BYTE*)hModule + funcRVA;
                }
            }
        }
        return nullptr;
    };

    LPVOID pLoadLibrary = nullptr;
    if (!isWow64) {
        // 64-bit target, same address as ours usually
        pLoadLibrary = (LPVOID)GetProcAddress(GetModuleHandleA("kernel32.dll"), "LoadLibraryA");
    } else {
        // 32-bit target (WoW64)
        // We must wait for kernel32.dll to be loaded. It might take a moment during
        // startup.
        int maxRetries = 20;  // 2 seconds (20 * 100ms)

        for (int retry = 0; retry < maxRetries; retry++) {
            HMODULE hMods[1024];
            DWORD cbNeeded;
            if (EnumProcessModulesEx(hProcess.get(), hMods, sizeof(hMods), &cbNeeded, LIST_MODULES_32BIT)) {
                for (unsigned int i = 0; i < (cbNeeded / sizeof(HMODULE)); i++) {
                    char szModName[MAX_PATH];
                    if (GetModuleFileNameExA(hProcess.get(), hMods[i], szModName, sizeof(szModName))) {
                        std::string modName = szModName;
                        // Case insensitive check
                        std::transform(modName.begin(), modName.end(), modName.begin(), ::tolower);

                        if (modName.find("kernel32.dll") != std::string::npos) {
                            // Found kernel32!
                            pLoadLibrary = GetRemoteProcAddress(hProcess.get(), hMods[i], "LoadLibraryA");

                            if (pLoadLibrary)
                                LogInfo("Resolved LoadLibraryA in x86 process at 0x%p (Base: 0x%p)", pLoadLibrary,
                                        hMods[i]);
                            else
                                LogError(
                                    "Failed to resolve LoadLibraryA in x86 process via PE "
                                    "parsing");
                            goto found_kernel32;
                        }
                    }
                }
            }
            Sleep(100);  // Wait for WoW64 init
        }
        LogError("Timeout waiting for kernel32.dll in WoW64 process");

    found_kernel32:;
    }

    if (!pLoadLibrary) {
        LogError("Failed to resolve LoadLibrary for PID %lu", pid);
        return false;
    }

    // Allocate memory in remote process - use RAII VirtualAllocGuard
    ce::VirtualAllocGuard pRemotePath(
        hProcess.get(), VirtualAllocEx(hProcess.get(), NULL, dllPath.size() + 1, MEM_COMMIT, PAGE_READWRITE));
    if (!pRemotePath) {
        LogError("VirtualAllocEx failed for PID %lu", pid);
        return false;
    }

    if (!WriteProcessMemory(hProcess.get(), pRemotePath.get(), dllPath.c_str(), dllPath.size() + 1, NULL)) {
        LogError("WriteProcessMemory failed for PID %lu", pid);
        return false;
    }

    ce::HandleGuard hThread(
        CreateRemoteThread(hProcess.get(), NULL, 0, (LPTHREAD_START_ROUTINE)pLoadLibrary, pRemotePath.get(), 0, NULL));
    if (!hThread) {
        LogError("CreateRemoteThread failed for PID %lu", pid);
        return false;
    }

    WaitForSingleObject(hThread.get(), 5000);  // Wait up to 5s

    DWORD exitCode = 0;
    if (GetExitCodeThread(hThread.get(), &exitCode)) {
        if (exitCode == 0) {
            // LoadLibraryA returned NULL
            LogError(
                "LoadLibraryA failed in remote process (Exit Code: 0). DLL "
                "failed to load.");
            return false;
        } else {
            LogInfo("LoadLibraryA succeeded (Remote Handle: 0x%lX)", (unsigned long)exitCode);
        }
    } else {
        LogError("Failed to get thread exit code.");
    }

    // Verify DLL is actually loaded in remote process
    {
        HMODULE hMods[256];
        DWORD cbNeeded = 0;
        DWORD filterFlag = isWow64 ? LIST_MODULES_32BIT : LIST_MODULES_64BIT;
        bool dllFound = false;
        if (EnumProcessModulesEx(hProcess.get(), hMods, sizeof(hMods), &cbNeeded, filterFlag)) {
            int moduleCount = cbNeeded / sizeof(HMODULE);
            for (int i = 0; i < moduleCount; i++) {
                char szModName[MAX_PATH];
                if (GetModuleFileNameExA(hProcess.get(), hMods[i], szModName, sizeof(szModName))) {
                    if (strstr(szModName, "capture_hook_x64.dll") || strstr(szModName, "capture_hook_x86.dll")) {
                        dllFound = true;
                        break;
                    }
                }
            }
        }
        if (!dllFound) {
            LogError(
                "DLL injection verification failed - hook DLL not found in "
                "module list for PID %lu",
                (unsigned long)pid);
        }
    }

    // Success - track the injected process
    // Note: We need to keep a handle to monitor the process, so release from RAII
    InjectedProcess ip;
    ip.pid = pid;
    ip.name = processName;
    ip.hProcess = hProcess.release();  // Transfer ownership
    ip.remoteMemory = nullptr;         // No remote memory for CreateRemoteThread injection (freed by RAII)
    injectedProcesses.push_back(ip);

    LogInfo("Injected %s into %s (PID: %d)", isWow64 ? "x86" : "x64", processName.c_str(), pid);
    return true;
}

bool InjectionManager::InjectEarly(DWORD pid, const std::string& dllPath, HANDLE hMainThread) {
    LogInfo("[APC] Attempting early APC injection for PID %lu", pid);

    HANDLE hProcess = OpenProcess(
        PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_VM_OPERATION | PROCESS_CREATE_THREAD,
        FALSE, pid);
    if (!hProcess) {
        LogError("[APC] OpenProcess failed for PID %d: %d", pid, GetLastError());
        return false;
    }

    HMODULE hKernel32 = GetModuleHandleA("kernel32.dll");
    FARPROC pLoadLibraryA = GetProcAddress(hKernel32, "LoadLibraryA");
    if (!pLoadLibraryA) {
        LogError("[APC] Failed to get LoadLibraryA address");
        CloseHandle(hProcess);
        return false;
    }

    SIZE_T pathSize = dllPath.size() + 1;
    LPVOID pRemotePath = VirtualAllocEx(hProcess, NULL, pathSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!pRemotePath) {
        LogError("[APC] VirtualAllocEx failed: %lu", GetLastError());
        CloseHandle(hProcess);
        return false;
    }

    if (!WriteProcessMemory(hProcess, pRemotePath, dllPath.c_str(), pathSize, NULL)) {
        LogError("[APC] WriteProcessMemory failed: %lu", GetLastError());
        VirtualFreeEx(hProcess, pRemotePath, 0, MEM_RELEASE);
        CloseHandle(hProcess);
        return false;
    }

    DWORD result = QueueUserAPC((PAPCFUNC)pLoadLibraryA, hMainThread, (ULONG_PTR)pRemotePath);
    if (!result) {
        LogError("[APC] QueueUserAPC failed: %lu", GetLastError());
        VirtualFreeEx(hProcess, pRemotePath, 0, MEM_RELEASE);
        CloseHandle(hProcess);
        return false;
    }

    LogInfo(
        "[APC] APC queued successfully for PID %d - DLL will load before "
        "import resolution",
        pid);

    InjectedProcess ip;
    ip.pid = pid;
    ip.name = dllPath;
    ip.hProcess = hProcess;
    ip.remoteMemory = pRemotePath;  // Store for later cleanup
    injectedProcesses.push_back(ip);

    return true;
}

void InjectionManager::EjectAll() {
    for (const auto& proc : injectedProcesses) {
        Eject(proc.pid);
    }
    injectedProcesses.clear();
}

// CRITICAL FIX: Wait for all delayed injection threads with timeout
void InjectionManager::WaitForInjectionThreads(int timeoutMs) {
    std::list<std::thread> threadsToJoin;

    {
        std::lock_guard<std::mutex> lock(threadListMutex);
        threadsToJoin = std::move(delayedInjectionThreads);
        delayedInjectionThreads.clear();
    }

    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    for (auto& t : threadsToJoin) {
        if (!t.joinable()) {
            continue;
        }

        auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            LogWarn("[Injection] Timeout waiting for injection threads, detaching remaining");
            t.detach();
            continue;
        }

        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
        DWORD waitMs = static_cast<DWORD>(std::max<int64_t>(remaining, 1));
        HANDLE threadHandle = reinterpret_cast<HANDLE>(t.native_handle());
        DWORD waitResult = WaitForSingleObject(threadHandle, waitMs);

        if (waitResult == WAIT_OBJECT_0) {
            t.join();
        } else {
            if (waitResult == WAIT_TIMEOUT) {
                LogWarn("[Injection] Timeout waiting for injection thread, detaching");
            } else {
                LogWarn("[Injection] WaitForSingleObject failed for injection thread (error=%lu), detaching",
                        GetLastError());
            }
            t.detach();
        }
    }
    LogInfo("[Injection] All delayed injection threads cleaned up");
}

// Check if any process is currently injected
bool InjectionManager::HasActiveInjections() const {
    std::lock_guard<std::mutex> lock(injectMutex);
    return !injectedProcesses.empty();
}

bool InjectionManager::HasPendingInjections() {
    std::lock_guard<std::mutex> lock(injectMutex);
    return !pendingInjections.empty() || !delayedInjectionThreads.empty();
}

void InjectionManager::Eject(DWORD pid) {
    std::lock_guard<std::mutex> lock(injectMutex);
    auto it = std::find_if(injectedProcesses.begin(), injectedProcesses.end(),
                           [&](const InjectedProcess& p) { return p.pid == pid; });
    HANDLE hProcess = (it != injectedProcesses.end()) ? it->hProcess : NULL;
    bool openedProcessHandle = false;

    if (!hProcess) {
        hProcess = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
        if (!hProcess)
            return;
        openedProcessHandle = true;
    }

    HMODULE hMods[1024];
    DWORD cbNeeded;
    if (EnumProcessModules(hProcess, hMods, sizeof(hMods), &cbNeeded)) {
        for (unsigned int i = 0; i < (cbNeeded / sizeof(HMODULE)); i++) {
            char szModName[MAX_PATH];
            if (GetModuleFileNameExA(hProcess, hMods[i], szModName, sizeof(szModName))) {
                std::string modName = szModName;
                if (modName.find("capture_hook_x64.dll") != std::string::npos ||
                    modName.find("capture_hook_x86.dll") != std::string::npos) {
                    HMODULE hKernel32 = GetModuleHandleA("kernel32.dll");
                    LPTHREAD_START_ROUTINE pFreeLibrary =
                        (LPTHREAD_START_ROUTINE)GetProcAddress(hKernel32, "FreeLibrary");

                    HANDLE hThread = CreateRemoteThread(hProcess, NULL, 0, pFreeLibrary, (LPVOID)hMods[i], 0, NULL);
                    if (hThread) {
                        WaitForSingleObject(hThread, 500);
                        CloseHandle(hThread);
                    }

                    // CRITICAL FIX: Free remote memory allocated during APC injection
                    if (it != injectedProcesses.end() && it->remoteMemory) {
                        VirtualFreeEx(hProcess, it->remoteMemory, 0, MEM_RELEASE);
                        LogInfo("[Eject] Freed remote memory at %p for PID %d", it->remoteMemory, pid);
                    }
                    break;
                }
            }
        }
    }

    if (openedProcessHandle)
        CloseHandle(hProcess);
}

// SHA256 using Windows CNG (bcrypt.dll)
[[maybe_unused]] static std::string ComputeFileHash(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file)
        return "";

    BCRYPT_ALG_HANDLE hAlg = NULL;
    BCRYPT_HASH_HANDLE hHash = NULL;
    if (BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA256_ALGORITHM, NULL, 0) < 0)
        return "";

    // Calculate buffer size
    DWORD cbHashObject = 0, cbData = 0;
    BCryptGetProperty(hAlg, BCRYPT_OBJECT_LENGTH, (PBYTE)&cbHashObject, sizeof(DWORD), &cbData, 0);

    std::vector<BYTE> pbHashObject(cbHashObject);
    if (BCryptCreateHash(hAlg, &hHash, pbHashObject.data(), cbHashObject, NULL, 0, 0) < 0) {
        BCryptCloseAlgorithmProvider(hAlg, 0);
        return "";
    }

    char buffer[4096];
    while (file.read(buffer, sizeof(buffer)) || file.gcount() > 0) {
        BCryptHashData(hHash, (PBYTE)buffer, (ULONG)file.gcount(), 0);
        if (file.eof())
            break;
    }

    DWORD cbHash = 0;
    BCryptGetProperty(hAlg, BCRYPT_HASH_LENGTH, (PBYTE)&cbHash, sizeof(DWORD), &cbData, 0);
    std::vector<BYTE> pbHash(cbHash);
    BCryptFinishHash(hHash, pbHash.data(), cbHash, 0);

    BCryptDestroyHash(hHash);
    BCryptCloseAlgorithmProvider(hAlg, 0);

    std::stringstream ss;
    for (BYTE b : pbHash)
        ss << std::hex << std::setw(2) << std::setfill('0') << (int)b;
    return ss.str();
}

bool InjectionManager::ValidateDllSecurity(const std::string& dllPath) {
    char exePathBuf[MAX_PATH];
    GetModuleFileNameA(NULL, exePathBuf, MAX_PATH);
    fs::path exePath = fs::path(exePathBuf).parent_path();
    fs::path checkPath = fs::absolute(dllPath);

    // 1. Path Validation
    std::error_code ec;
    auto canonicalCheck = fs::weakly_canonical(checkPath, ec);
    auto canonicalExe = fs::weakly_canonical(exePath, ec);
    if (ec || canonicalCheck.string().find(canonicalExe.string()) != 0 ||
        (canonicalCheck.string().size() > canonicalExe.string().size() &&
         canonicalCheck.string()[canonicalExe.string().size()] != '\\')) {
        LogError("[Security] DLL path is outside application directory: %s", checkPath.string().c_str());
        return false;
    }

    // 2. ACL Check (Check if World/Everyone has Write Access)
    PACL pDacl = NULL;
    PSECURITY_DESCRIPTOR pSD = NULL;
    if (GetNamedSecurityInfoA(dllPath.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, NULL, NULL, &pDacl, NULL,
                              &pSD) == ERROR_SUCCESS) {
        TRUSTEE_A trustee = {};
        trustee.TrusteeForm = TRUSTEE_IS_SID;
        trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;

        // Check "Everyone" (S-1-1-0)
        SID_IDENTIFIER_AUTHORITY SIDAuth = SECURITY_WORLD_SID_AUTHORITY;
        PSID pEveryoneSid = NULL;
        AllocateAndInitializeSid(&SIDAuth, 1, SECURITY_WORLD_RID, 0, 0, 0, 0, 0, 0, 0, &pEveryoneSid);
        trustee.ptstrName = (LPSTR)pEveryoneSid;

        ACCESS_MASK access = 0;
        GetEffectiveRightsFromAclA(pDacl, &trustee, &access);

        FreeSid(pEveryoneSid);
        LocalFree(pSD);  // also frees pDacl if it points into pSD

        if (access & (FILE_WRITE_DATA | FILE_APPEND_DATA | WRITE_DAC | WRITE_OWNER)) {
            LogError("[Security] DLL is writable by Everyone! Access Mask: 0x%lX", access);
#ifdef CE_PRODUCTION_BUILD
            return false;  // Strict mode in production
#else
            LogError("[Security] WARNING: Proceeding despite world-writable DLL (dev build)");
#endif
        }
    }

    LogInfo("[Security] DLL security validation passed for %s", dllPath.c_str());
    return true;
}

// Verify DLL Authenticode signature (production builds only)
// Returns true if DLL is properly signed, false otherwise
bool InjectionManager::VerifyDLLSignature(const std::string& dllPath, bool logFailures) {
    // Convert to wide string for WinVerifyTrust
    std::wstring widePath(dllPath.begin(), dllPath.end());

    // Setup WINTRUST_FILE_INFO structure
    WINTRUST_FILE_INFO fileInfo = {};
    fileInfo.cbStruct = sizeof(WINTRUST_FILE_INFO);
    fileInfo.pcwszFilePath = widePath.c_str();
    fileInfo.hFile = NULL;
    fileInfo.pgKnownSubject = NULL;

    // Setup WINTRUST_DATA structure
    GUID policyGUID = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    WINTRUST_DATA trustData = {};
    trustData.cbStruct = sizeof(WINTRUST_DATA);
    trustData.pPolicyCallbackData = NULL;
    trustData.pSIPClientData = NULL;
    trustData.dwUIChoice = WTD_UI_NONE;
    trustData.fdwRevocationChecks = WTD_REVOKE_NONE;  // Skip revocation check for performance
    trustData.dwUnionChoice = WTD_CHOICE_FILE;
    trustData.dwStateAction = WTD_STATEACTION_VERIFY;
    trustData.hWVTStateData = NULL;
    trustData.pwszURLReference = NULL;
    trustData.dwProvFlags = WTD_SAFER_FLAG;
    trustData.dwUIContext = 0;
    trustData.pFile = &fileInfo;

    // Verify signature
    LONG status = WinVerifyTrust(NULL, &policyGUID, &trustData);

    // Cleanup
    trustData.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust(NULL, &policyGUID, &trustData);

    if (status == ERROR_SUCCESS) {
        LogInfo("[Security] DLL signature valid: %s", dllPath.c_str());
        return true;
    } else {
        if (logFailures) {
            LogError("[Security] DLL signature verification failed: %s (error 0x%lu)", dllPath.c_str(),
                     (unsigned long)status);
            if (status == TRUST_E_NOSIGNATURE) {
                LogError("[Security] DLL is not signed");
            } else if (status == TRUST_E_EXPLICIT_DISTRUST) {
                LogError("[Security] DLL signature is explicitly distrusted");
            } else if (status == TRUST_E_SUBJECT_NOT_TRUSTED) {
                LogError("[Security] DLL signer is not trusted");
            } else if (status == CRYPT_E_SECURITY_SETTINGS) {
                LogError("[Security] Security settings prevent verification");
            }
        }
        return false;
    }
}

// Verify DLL integrity using SHA-256 hash comparison
// This is a fallback for debug builds when Authenticode signing is not
// available Expected hashes are stored in a .hashes file next to the DLL
bool InjectionManager::VerifyDLLHash(const std::string& dllPath) {
    // Check if hash file exists (debug builds only)
    std::string hashFilePath = dllPath + ".hash";
    if (!fs::exists(hashFilePath)) {
        // No hash file - skip verification in debug builds
        LogDebug("[Security] No hash file found for %s, skipping hash verification", dllPath.c_str());
        return true;
    }

    // Read expected hash from file
    std::ifstream hashFile(hashFilePath);
    if (!hashFile.is_open()) {
        LogWarn("[Security] Failed to open hash file: %s", hashFilePath.c_str());
        return true;  // Allow in debug mode
    }

    std::string expectedHash;
    std::getline(hashFile, expectedHash);
    hashFile.close();

    // Trim whitespace
    expectedHash.erase(0, expectedHash.find_first_not_of(" \t\r\n"));
    expectedHash.erase(expectedHash.find_last_not_of(" \t\r\n") + 1);

    if (expectedHash.empty()) {
        LogWarn("[Security] Empty hash file: %s", hashFilePath.c_str());
        return true;  // Allow in debug mode
    }

    // Compute actual hash of DLL
    std::string actualHash = ComputeFileHash(dllPath);
    if (actualHash.empty()) {
        LogError("[Security] Failed to compute hash for: %s", dllPath.c_str());
        return false;
    }

    // Compare hashes (case-insensitive)
    bool match = (actualHash.length() == expectedHash.length());
    if (match) {
        for (size_t i = 0; i < actualHash.length(); ++i) {
            if (std::tolower(actualHash[i]) != std::tolower(expectedHash[i])) {
                match = false;
                break;
            }
        }
    }

    if (match) {
        LogInfo("[Security] DLL hash verified: %s", dllPath.c_str());
        return true;
    } else {
        LogError("[SECURITY] DLL hash mismatch for %s", dllPath.c_str());
        LogError("[SECURITY] Expected: %s", expectedHash.c_str());
        LogError("[SECURITY] Actual:   %s", actualHash.c_str());
        return false;
    }
}

// Compute SHA-256 hash of a file
std::string InjectionManager::ComputeFileHash(const std::string& filePath) {
    // Open file
    HANDLE hFile = CreateFileA(filePath.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                               FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (hFile == INVALID_HANDLE_VALUE) {
        LogError("[Security] Failed to open file for hashing: %s", filePath.c_str());
        return "";
    }

    // Get file size
    LARGE_INTEGER fileSize;
    if (!GetFileSizeEx(hFile, &fileSize)) {
        CloseHandle(hFile);
        return "";
    }

    // Map file into memory for efficient hashing
    HANDLE hMapping = CreateFileMapping(hFile, NULL, PAGE_READONLY, 0, 0, NULL);
    if (!hMapping) {
        CloseHandle(hFile);
        return "";
    }

    LPVOID pData = MapViewOfFile(hMapping, FILE_MAP_READ, 0, 0, 0);
    if (!pData) {
        CloseHandle(hMapping);
        CloseHandle(hFile);
        return "";
    }

    // Compute SHA-256 hash using BCrypt
    BCRYPT_ALG_HANDLE hAlg = NULL;
    BCRYPT_HASH_HANDLE hHash = NULL;
    std::string result;

    if (BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA256_ALGORITHM, NULL, 0) == STATUS_SUCCESS) {
        DWORD hashLen = 0;
        DWORD dataLen = 0;

        if (BCryptGetProperty(hAlg, BCRYPT_HASH_LENGTH, (PUCHAR)&hashLen, sizeof(hashLen), &dataLen, 0) ==
            STATUS_SUCCESS) {
            std::vector<BYTE> hashBytes(hashLen);

            if (BCryptCreateHash(hAlg, &hHash, NULL, 0, NULL, 0, 0) == STATUS_SUCCESS) {
                if (BCryptHashData(hHash, (PUCHAR)pData, (ULONG)fileSize.QuadPart, 0) == STATUS_SUCCESS) {
                    if (BCryptFinishHash(hHash, hashBytes.data(), hashLen, 0) == STATUS_SUCCESS) {
                        // Convert to hex string
                        std::stringstream ss;
                        for (BYTE b : hashBytes) {
                            ss << std::hex << std::setw(2) << std::setfill('0') << (int)b;
                        }
                        result = ss.str();
                    }
                }
                BCryptDestroyHash(hHash);
            }
        }
        BCryptCloseAlgorithmProvider(hAlg, 0);
    }

    // Cleanup
    UnmapViewOfFile(pData);
    CloseHandle(hMapping);
    CloseHandle(hFile);

    return result;
}

void InjectionManager::ScanExistingProcesses() {
    const int64_t scanStartUs = Log_GetQpcUs();
    const int64_t snapshotStartUs = scanStartUs;
    HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    const int64_t snapshotUs = Log_GetQpcUs() - snapshotStartUs;
    if (hSnapshot == INVALID_HANDLE_VALUE) {
        LogInfo("[StartupPerf] ScanExistingProcesses: CreateToolhelp32Snapshot failed after %.3f ms (error=%lu)",
                QpcDeltaToMs(snapshotUs), GetLastError());
        return;
    }

    PROCESSENTRY32 pe32;
    pe32.dwSize = sizeof(PROCESSENTRY32);
    int scannedProcesses = 0;
    int whitelistedProcesses = 0;
    int injectAttempts = 0;
    int injectSuccesses = 0;

    if (Process32First(hSnapshot, &pe32)) {
        do {
            ++scannedProcesses;
            std::string name = pe32.szExeFile;
            std::lock_guard<std::mutex> injectLock(injectMutex);
            if (IsWhitelisted(name)) {
                ++whitelistedProcesses;
                if (!IsAlreadyInjectedLocked(pe32.th32ProcessID) && !IsRecentlyFailedLocked(pe32.th32ProcessID) &&
                    !IsAlreadyPendingLocked(pe32.th32ProcessID)) {
                    ++injectAttempts;
                    LogInfo("[Scan] Found existing whitelisted process: %s (PID: %lu)", name.c_str(),
                            (unsigned long)pe32.th32ProcessID);
                    pendingInjections.push_back(
                        {pe32.th32ProcessID, name, "StartupScan", GetTickCount64() + kPendingInjectionDelayMs});
                    ++injectSuccesses;
                }
            }
        } while (Process32Next(hSnapshot, &pe32));
    }
    CloseHandle(hSnapshot);
    LogInfo(
        "[StartupPerf] ScanExistingProcesses: snapshot=%.3f ms, total=%.3f ms, scanned=%d, whitelisted=%d, "
        "injectAttempts=%d, queuedForDelayedInjection=%d",
        QpcDeltaToMs(snapshotUs), QpcDeltaToMs(Log_GetQpcUs() - scanStartUs), scannedProcesses, whitelistedProcesses,
        injectAttempts, injectSuccesses);
}
