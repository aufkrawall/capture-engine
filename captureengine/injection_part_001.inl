#include "injection.h"
#include <psapi.h>
#include <tlhelp32.h>
#include <algorithm>
#include <array>
#include <filesystem>
#include <iostream>
#include <system_error>
#include <thread>
#include <vector>
#include "../common/logging.h"
#include "../common/module_enumeration.h"
#include "../common/raii_helpers.h"
#include "injection_policy.h"

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

// Read a null-terminated string from a remote process.
static bool ReadRemoteString(HANDLE hProc, LPCVOID address, char* buffer, size_t bufferSize) {
    if (!buffer || bufferSize == 0)
        return false;
    buffer[0] = '\0';
    size_t offset = 0;
    while (offset < bufferSize - 1) {
        char c;
        if (!ReadProcessMemory(hProc, static_cast<const char*>(address) + offset, &c, 1, NULL))
            return false;
        buffer[offset++] = c;
        if (c == '\0')
            return true;
    }
    buffer[bufferSize - 1] = '\0';
    return true;
}

// Resolve a function address in a remote 32-bit (WoW64) module by manually
// parsing its PE export directory. Used for cross-bitness injection where
// GetProcAddress from the local 64-bit module returns the wrong address.
static LPVOID GetRemoteProcAddress(HANDLE hProc, HMODULE hModule, const char* funcName) {
    IMAGE_DOS_HEADER dosHeader;
    if (!ReadProcessMemory(hProc, hModule, &dosHeader, sizeof(dosHeader), NULL))
        return nullptr;
    if (dosHeader.e_magic != IMAGE_DOS_SIGNATURE)
        return nullptr;

    BYTE* pNtHeaders = (BYTE*)hModule + dosHeader.e_lfanew;
    IMAGE_NT_HEADERS32 ntHeaders;
    if (!ReadProcessMemory(hProc, pNtHeaders, &ntHeaders, sizeof(ntHeaders), NULL))
        return nullptr;
    if (ntHeaders.Signature != IMAGE_NT_SIGNATURE)
        return nullptr;

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

    for (DWORD i = 0; i < exportDir.NumberOfNames; i++) {
        char buffer[256];
        if (ReadRemoteString(hProc, (BYTE*)hModule + nameRVAs[i], buffer, sizeof(buffer))) {
            if (strcmp(buffer, funcName) == 0) {
                WORD ordinal;
                if (!ReadProcessMemory(hProc, (BYTE*)hModule + exportDir.AddressOfNameOrdinals + (i * sizeof(WORD)),
                                       &ordinal, sizeof(WORD), NULL))
                    return nullptr;

                DWORD funcRVA;
                if (!ReadProcessMemory(hProc, (BYTE*)hModule + exportDir.AddressOfFunctions + (ordinal * sizeof(DWORD)),
                                       &funcRVA, sizeof(DWORD), NULL))
                    return nullptr;

                return (BYTE*)hModule + funcRVA;
            }
        }
    }
    return nullptr;
}

// Resolve a function address in a remote 32-bit module by module name.
// Opens the remote module enumeration and delegates to GetRemoteProcAddress above.
static LPVOID GetRemoteModuleProcAddress(HANDLE hProc, const wchar_t* moduleName, const char* funcName) {
    int maxRetries = 20;
    for (int retry = 0; retry < maxRetries; retry++) {
        std::vector<HMODULE> hMods;
        if (ce::EnumerateProcessModulesEx(hProc, LIST_MODULES_32BIT, hMods)) {
            for (size_t i = 0; i < hMods.size(); i++) {
                char szModName[MAX_PATH];
                if (GetModuleFileNameExA(hProc, hMods[i], szModName, sizeof(szModName))) {
                    std::string modName = szModName;
                    std::transform(modName.begin(), modName.end(), modName.begin(),
                                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });

                    // Convert wide module name to lower for comparison
                    char narrowModuleName[MAX_PATH];
                    WideCharToMultiByte(CP_UTF8, 0, moduleName, -1, narrowModuleName, sizeof(narrowModuleName), NULL,
                                        NULL);
                    std::string lowerTarget = narrowModuleName;
                    std::transform(lowerTarget.begin(), lowerTarget.end(), lowerTarget.begin(),
                                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });

                    if (modName.find(lowerTarget) != std::string::npos) {
                        return GetRemoteProcAddress(hProc, hMods[i], funcName);
                    }
                }
            }
        }
        Sleep(100);
    }
    return nullptr;
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
    // Reject and drain WMI callbacks before joining raw-owner worker threads.
    // Launch and worker-list transfer are serialized by threadListMutex, so no
    // worker can appear after the list has been claimed for shutdown.
    RequestShutdown();
    ShutdownWMI();
    WaitForInjectionThreads(5000);
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
        LogInfo("[StartupPerf] InitializeWMI failed at CoInitializeEx after %.3f ms (hr=0x%lX)", QpcDeltaToMs(coInitUs),
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

// Event Sink Implementation
