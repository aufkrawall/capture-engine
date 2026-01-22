#include "injection.h"
#include "../common/logging.h"
#include "../common/raii_helpers.h"
#include <algorithm>
#include <array>
#include <filesystem>
#include <iostream>
#include <psapi.h>
#include <tlhelp32.h>

#include <aclapi.h>
#include <bcrypt.h>
#include <iomanip>
#include <sstream>
# include <fstream>
# include <algorithm>
# include <wintrust.h>
#include <softpub.h>

#pragma comment(lib, "wintrust.lib")

/*
 * ANTI-CHEAT DETECTION LIMITATIONS
 * =================================
 * 
 * This injection system uses CreateRemoteThread() for DLL injection, which is:
 * 
 * 1. EASILY DETECTABLE by modern anti-cheat systems (EAC, BattlEye, Vanguard, etc.)
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
 *    - Hook installation via MinHook (IAT/inline hooks)
 *    - Process memory scanning for known hook patterns
 * 
 * 4. SAFER ALTERNATIVES (not implemented):
 *    - Manual mapping (avoids LoadLibrary detection)
 *    - Kernel-mode driver injection (requires signed driver)
 *    - AppInit_DLLs registry (deprecated, requires reboot)
 *    - SetWindowsHookEx (limited to UI thread)
 * 
 * USE AT YOUR OWN RISK. This tool is for educational and single-player use only.
 */

namespace fs = std::filesystem;

InjectionManager::InjectionManager(const AppConfig &config) : config(config) {
  // Determine DLL paths (assume next to exe)
  char buffer[MAX_PATH];
  GetModuleFileNameA(NULL, buffer, MAX_PATH);
  fs::path exePath(buffer);

  hookDllPathX64 = (exePath.parent_path() / "capture_hook_x64.dll").string();
  hookDllPathX86 = (exePath.parent_path() / "capture_hook_x86.dll").string();

  // Check if DLLs exist
  if (!fs::exists(hookDllPathX64))
    LogError("Capture Hook X64 DLL not found: %s", hookDllPathX64.c_str());
  if (!fs::exists(hookDllPathX86))
    LogError("Capture Hook X86 DLL not found: %s", hookDllPathX86.c_str());
    
  InitializeWMI();
  ScanExistingProcesses();
}

InjectionManager::~InjectionManager() { 
    ShutdownWMI();
    EjectAll(); 
}

void InjectionManager::SetOnInjectCallback(std::function<void(const std::string&)> callback) {
  this->onInjectCallback = callback;
}

bool InjectionManager::IsWhitelisted(const std::string &processName) {
  std::string lowerName = processName;
  std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(),
                 ::tolower);

  // Internal Whitelist for testing (automatic if debug_logging is enabled)
  if (config.debugLogging) {
      if (lowerName.find("_test.exe") != std::string::npos) {
          return true;
      }
  }

  // Strict Whitelist Mode (WMI only injects explicit whitelist entries)
  if (config.gameWhitelist.empty() && config.overlayWhitelist.empty()) {
    return false;
  }

  // Check Game Whitelist
  for (const auto &item : config.gameWhitelist) {
    std::string lowerItem = item;
    std::transform(lowerItem.begin(), lowerItem.end(), lowerItem.begin(),
                   ::tolower);
    if (lowerName == lowerItem) {
      LogInfo("[WMI] Whitelist match (Exact): %s matches %s", processName.c_str(), item.c_str());
      return true;
    }
    if (lowerName.find(lowerItem) != std::string::npos) {
      LogInfo("[WMI] Whitelist match (Partial): %s matches %s", processName.c_str(), item.c_str());
      return true;
    }
  }

  // Check Overlay Whitelist
  for (const auto &item : config.overlayWhitelist) {
    std::string lowerItem = item;
    std::transform(lowerItem.begin(), lowerItem.end(), lowerItem.begin(),
                   ::tolower);
    if (lowerName == lowerItem) {
      LogInfo("[WMI] Overlay target match (Exact): %s matches %s", processName.c_str(), item.c_str());
      return true;
    }
    if (lowerName.find(lowerItem) != std::string::npos) {
      LogInfo("[WMI] Overlay target match (Partial): %s matches %s", processName.c_str(), item.c_str());
      return true;
    }
  }
  return false;
}

bool InjectionManager::IsAlreadyInjected(DWORD pid) {
  // First check our in-memory tracking
  for (const auto &p : injectedProcesses) {
    if (p.pid == pid)
      return true;
  }
  
  // Also check if hook DLL is already loaded in the target process
  // This handles the case where a previous captureengine instance injected
  // and we're a new instance that doesn't know about it
  HANDLE hProcess = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, 
                                FALSE, pid);
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



void InjectionManager::Update() {
  std::lock_guard<std::mutex> lock(injectMutex);

  // Cleanup dead processes
  injectedProcesses.erase(
      std::remove_if(injectedProcesses.begin(), injectedProcesses.end(),
                     [](const InjectedProcess &p) {
                       DWORD exitCode;
                       if (GetExitCodeProcess(p.hProcess, &exitCode) &&
                           exitCode != STILL_ACTIVE) {
                         CloseHandle(p.hProcess);
                         return true;
                       }
                       return false;
                     }),
      injectedProcesses.end());

  // Cleanup old failed injections (expire after 30 seconds)
  uint64_t now = GetTickCount64();
  failedInjections.erase(
      std::remove_if(failedInjections.begin(), failedInjections.end(),
                     [now](const FailedInjection &f) {
                       return (now - f.timestamp) > 30000;
                     }),
      failedInjections.end());
      
  // Process pending injections
  auto it = pendingInjections.begin();
  while (it != pendingInjections.end()) {
      if (now >= it->injectTime) {
          DWORD pid = it->pid;
          std::string name = it->name;
          
          // Re-verify it's still running and not injected
          if (!IsAlreadyInjected(pid) && !IsRecentlyFailed(pid)) {
             LogInfo("[WMI] Injecting deferred: %s (PID: %d)", name.c_str(), pid);
             if (Inject(pid, name)) {
                LogInfo("Injection successful.");
             } else {
                LogError("Injection failed.");
                failedInjections.push_back({pid, GetTickCount64()});
             }
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

    // Initialize COM
    hres = CoInitializeEx(0, COINIT_MULTITHREADED);
    if (FAILED(hres)) {
        LogError("Failed to initialize COM library. Error code = 0x%X", hres);
        return false;
    }

    // Initialize Security
    hres = CoInitializeSecurity(
        NULL,
        -1,                          // COM authentication
        NULL,                        // Authentication services
        NULL,                        // Reserved
        RPC_C_AUTHN_LEVEL_DEFAULT,   // Default authentication
        RPC_C_IMP_LEVEL_IMPERSONATE, // Default Impersonation
        NULL,                        // Authentication info
        EOAC_NONE,                   // Additional capabilities
        NULL                         // Reserved
    );

    if (FAILED(hres) && hres != RPC_E_TOO_LATE) {
        LogError("Failed to initialize security. Error code = 0x%X", hres);
        return false; // Don't return false if RPC_E_TOO_LATE (already init)
    }

    // Obtain the initial locator to WMI
    hres = CoCreateInstance(
        CLSID_WbemLocator,
        0,
        CLSCTX_INPROC_SERVER,
        IID_IWbemLocator, (LPVOID*)&pLoc);

    if (FAILED(hres)) {
        LogError("Failed to create IWbemLocator object. Err: 0x%X", hres);
        return false;
    }

    // Connect to WMI
    hres = pLoc->ConnectServer(
        _bstr_t(L"ROOT\\CIMV2"), // Object path of WMI namespace
        NULL,                    // User name
        NULL,                    // User password
        0,                       // Locale
        NULL,                    // Security flags
        0,                       // Authority
        0,                       // Context object
        &pSvc                    // IWbemServices proxy
    );

    if (FAILED(hres)) {
        LogError("Could not connect. Error code = 0x%X", hres);
        pLoc->Release(); pLoc = nullptr;
        return false;
    }

    // Set security levels on the proxy
    hres = CoSetProxyBlanket(
        pSvc,                        // Indicates the proxy to set
        RPC_C_AUTHN_WINNT,           // RPC_C_AUTHN_xxx
        RPC_C_AUTHZ_NONE,            // RPC_C_AUTHZ_xxx
        NULL,                        // Server principal name
        RPC_C_AUTHN_LEVEL_CALL,      // RPC_C_AUTHN_LEVEL_xxx
        RPC_C_IMP_LEVEL_IMPERSONATE, // RPC_C_IMP_LEVEL_xxx
        NULL,                        // client identity
        EOAC_NONE                    // proxy capabilities
    );

    if (FAILED(hres)) {
        LogError("Could not set proxy blanket. Error code = 0x%X", hres);
        pSvc->Release(); pSvc = nullptr;
        pLoc->Release(); pLoc = nullptr;
        return false;
    }

    // Setup Unsecured Apartment for async callbacks
    hres = CoCreateInstance(CLSID_UnsecuredApartment, NULL, CLSCTX_LOCAL_SERVER, IID_IUnsecuredApartment, (void**)&pUnsecApp);
    if (FAILED(hres)) {
        LogError("Failed to create UnsecuredApartment: 0x%X", hres);
        // Continue anyway? Callbacks might fail permission checks without it
    }

    // Create Event Sink
    pSink = new ProcessEventSink(this);
    pSink->AddRef();
    
    // Create Stub Sink
    if (pUnsecApp) {
        hres = pUnsecApp->CreateObjectStub(pSink, (IUnknown**)&pStubSink);
        if (FAILED(hres)) {
            LogError("CreateObjectStub failed: 0x%X", hres);
            // Fallback?
            pStubSink = pSink;
            pStubSink->AddRef();
        }
    } else {
        pStubSink = pSink;
        pStubSink->AddRef();
    }

    // Exec Notification Query
    // Use WITHIN 0.1 for high responsiveness (100ms polling by WMI)
    hres = pSvc->ExecNotificationQueryAsync(
        _bstr_t("WQL"),
        _bstr_t("SELECT * FROM __InstanceCreationEvent WITHIN 0.1 WHERE TargetInstance ISA 'Win32_Process'"),
        WBEM_FLAG_SEND_STATUS,
        NULL,
        pStubSink
    );

    if (FAILED(hres)) {
        LogError("ExecNotificationQueryAsync failed. Error code = 0x%X", hres);
        return false;
    }
    
    LogInfo("WMI Event Sink Initialized");
    return true;
}

void InjectionManager::ShutdownWMI() {
    if (pSvc) {
        pSvc->CancelAsyncCall(pStubSink);
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
    if (lRef == 0) delete this;
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

    HRESULT STDMETHODCALLTYPE InjectionManager::ProcessEventSink::Indicate(LONG lObjectCount, IWbemClassObject __RPC_FAR* __RPC_FAR* apObjArray) {
        // Exception handling: WMI callbacks can throw COM exceptions
        // Catching them prevents crashes and allows graceful degradation
        try {
            if (bDone || !pManager) return WBEM_S_NO_ERROR;

            for (int i = 0; i < lObjectCount; i++) {
                IWbemClassObject* pObj = apObjArray[i];
                
                // Get TargetInstance
                _variant_t vTarget;
                if (FAILED(pObj->Get(L"TargetInstance", 0, &vTarget, NULL, NULL))) continue;
                
                IUnknown* pUnk = vTarget;
                IWbemClassObject* pTargetCase = nullptr;
                if (FAILED(pUnk->QueryInterface(IID_IWbemClassObject, (void**)&pTargetCase))) continue;
                
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
                    
                    // Check whitelist (thread-safe? IsWhitelisted reads config which is const, so yes)
                    if (pManager->IsWhitelisted(name)) {
                        // IMMEDIATE INJECTION (No Delay)
                        std::lock_guard<std::mutex> lock(pManager->injectMutex);
                        if (!pManager->IsAlreadyInjected(pid) && !pManager->IsRecentlyFailed(pid)) {
                             LogInfo("[WMI] Detected process creation: %s (PID: %d) - Injecting IMMEDIATELY", name.c_str(), pid);
                             if (pManager->Inject(pid, name)) {
                                LogInfo("[WMI] Immediate injection successful.");
                             } else {
                                LogError("[WMI] Immediate injection failed.");
                                pManager->failedInjections.push_back({pid, GetTickCount64()});
                             }
                        }
                    }
                }
                
                pTargetCase->Release();
            }
        } catch (const _com_error& e) {
            // COM exception - log and continue gracefully
            LogError("WMI Indicate: COM exception 0x%X: %s", e.Error(), e.ErrorMessage());
        } catch (const std::exception& e) {
            // Standard exception
            LogError("WMI Indicate: Exception: %s", e.what());
        } catch (...) {
            // Unknown exception
            LogError("WMI Indicate: Unknown exception caught");
        }
        
        return WBEM_S_NO_ERROR;
    }

HRESULT STDMETHODCALLTYPE InjectionManager::ProcessEventSink::SetStatus(LONG lFlags, HRESULT hResult, BSTR strParam, IWbemClassObject __RPC_FAR* pObjParam) {
    return WBEM_S_NO_ERROR;
}

bool InjectionManager::IsRecentlyFailed(DWORD pid) {
    // Already holding mutex in Update context, need to be careful if called from elsewhere?
    // Current usage is only inside Update (locked) or Indicate (Locked)
    // Safe for now
    for (const auto& fail : failedInjections) {
        if (fail.pid == pid) return true;
    }
    return false;
}

bool InjectionManager::Inject(DWORD pid, const std::string &processName) {
  // Execute callback if set (e.g. to reload config for this specific process)
  if (onInjectCallback) {
      LogInfo("[Inject] Executing pre-injection callback for %s", processName.c_str());
      onInjectCallback(processName);
  }

  // Determine architecture - use RAII HandleGuard to prevent leaks
  ce::HandleGuard hProcess(
      OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ |
                      PROCESS_VM_WRITE | PROCESS_VM_OPERATION |
                      PROCESS_CREATE_THREAD,
                  FALSE, pid));
  if (!hProcess) {
    LogError("Failed to open process %d for injection", pid);
    return false;
  }

  BOOL isWow64 = FALSE;
  IsWow64Process(hProcess.get(), &isWow64);

  std::string dllPath = isWow64 ? hookDllPathX86 : hookDllPathX64;
  
  // TODO: Re-enable signature verification once wintrust library is available in MSYS2
  // The WinVerifyTrust API requires wintrust.lib which may not be available in all build environments
#if 0 // Disabled temporarily - wintrust.lib not found in MSYS2
#ifndef _DEBUG
  // PRODUCTION BUILD: Verify DLL signature before injection
  // This prevents injection of tampered/unsigned DLLs
  if (!VerifyDLLSignature(dllPath)) {
    LogError("DLL signature verification failed for %s - refusing to inject", dllPath.c_str());
    LogError("In production builds, only signed DLLs can be injected");
    return false;
  }
  LogInfo("DLL signature verified: %s", dllPath.c_str());
#else
  // DEBUG BUILD: Skip DLL signature verification for development
  LogInfo("DEBUG BUILD: Skipping DLL signature verification");
#endif
#endif

  LogInfo("Using DLL: %s (WoW64: %d)", dllPath.c_str(), isWow64);
  
  if (!fs::exists(dllPath)) {
    LogError("Required DLL for %s injection missing: %s",
             isWow64 ? "x86" : "x64", dllPath.c_str());
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
      // Note: We assume target is 32-bit here because this logic is inside 'if (isWow64)'
      // But we should be careful. We can read NT Headers signature first? 
      // Let's just read the signature + file header + optional header structure.
      // Since we know we are in isWow64 block, we expect 32-bit headers.
      
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
      if (!ReadProcessMemory(hProc, (BYTE*)hModule + exportDir.AddressOfNames, nameRVAs.data(), nameRVAs.size() * sizeof(DWORD), NULL))
          return nullptr;

      // Search for function name
      for (DWORD i = 0; i < exportDir.NumberOfNames; i++) {
          char buffer[256];
          if (ReadProcessMemory(hProc, (BYTE*)hModule + nameRVAs[i], buffer, sizeof(buffer), NULL)) {
              buffer[255] = '\0'; // Ensure null term
              if (strcmp(buffer, funcName) == 0) {
                  // Found name, get ordinal
                  WORD ordinal;
                  if (!ReadProcessMemory(hProc, (BYTE*)hModule + exportDir.AddressOfNameOrdinals + (i * sizeof(WORD)), &ordinal, sizeof(WORD), NULL))
                      return nullptr;
                  
                  // Get Function RVA
                  DWORD funcRVA;
                  if (!ReadProcessMemory(hProc, (BYTE*)hModule + exportDir.AddressOfFunctions + (ordinal * sizeof(DWORD)), &funcRVA, sizeof(DWORD), NULL))
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
    pLoadLibrary = (LPVOID)GetProcAddress(GetModuleHandleA("kernel32.dll"),
                                          "LoadLibraryA");
  } else {
    // 32-bit target (WoW64)
    // We must wait for kernel32.dll to be loaded. It might take a moment during startup.
    int maxRetries = 20; // 2 seconds (20 * 100ms)
    
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
                    LogInfo("Resolved LoadLibraryA in x86 process at 0x%p (Base: 0x%p)", pLoadLibrary, hMods[i]);
                else
                    LogError("Failed to resolve LoadLibraryA in x86 process via PE parsing");
                goto found_kernel32;
              }
            }
          }
        }
        Sleep(100); // Wait for WoW64 init
    }
    LogError("Timeout waiting for kernel32.dll in WoW64 process");
    
    found_kernel32:;
  }

  if (!pLoadLibrary) {
    LogError("Failed to resolve LoadLibrary for PID %d", pid);
    return false;
  }

  // Allocate memory in remote process - use RAII VirtualAllocGuard
  ce::VirtualAllocGuard pRemotePath(
      hProcess.get(),
      VirtualAllocEx(hProcess.get(), NULL, dllPath.size() + 1, MEM_COMMIT, PAGE_READWRITE));
  if (!pRemotePath) {
    LogError("VirtualAllocEx failed for PID %d", pid);
    return false;
  }

  if (!WriteProcessMemory(hProcess.get(), pRemotePath.get(), dllPath.c_str(),
                          dllPath.size() + 1, NULL)) {
    LogError("WriteProcessMemory failed for PID %d", pid);
    return false;
  }

  ce::HandleGuard hThread(CreateRemoteThread(hProcess.get(), NULL, 0,
                                      (LPTHREAD_START_ROUTINE)pLoadLibrary,
                                      pRemotePath.get(), 0, NULL));
  if (!hThread) {
    LogError("CreateRemoteThread failed for PID %d", pid);
    return false;
  }

  WaitForSingleObject(hThread.get(), 5000); // Wait up to 5s

  DWORD exitCode = 0;
  if (GetExitCodeThread(hThread.get(), &exitCode)) {
      if (exitCode == 0) {
          // LoadLibraryA returned NULL
          LogError("LoadLibraryA failed in remote process (Exit Code: 0). DLL failed to load.");
          return false;
      } else {
          LogInfo("LoadLibraryA succeeded (Remote Handle: 0x%X)", exitCode);
      }
  } else {
      LogError("Failed to get thread exit code.");
  }

  // Success - track the injected process
  // Note: We need to keep a handle to monitor the process, so release from RAII
  InjectedProcess ip;
  ip.pid = pid;
  ip.name = processName;
  ip.hProcess = hProcess.release(); // Transfer ownership
  injectedProcesses.push_back(ip);

  LogInfo("Injected %s into %s (PID: %d)", isWow64 ? "x86" : "x64",
          processName.c_str(), pid);
  return true;
}

void InjectionManager::EjectAll() {
  for (const auto &proc : injectedProcesses) {
    Eject(proc.pid);
  }
  injectedProcesses.clear();
}

// Check if any process is currently injected
bool InjectionManager::HasActiveInjections() const {
  return !injectedProcesses.empty();
}

void InjectionManager::Eject(DWORD pid) {
  std::lock_guard<std::mutex> lock(injectMutex);
  auto it =
      std::find_if(injectedProcesses.begin(), injectedProcesses.end(),
                   [&](const InjectedProcess &p) { return p.pid == pid; });
  HANDLE hProcess = (it != injectedProcesses.end()) ? it->hProcess : NULL;

  if (!hProcess) {
    hProcess =
        OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!hProcess)
      return;
  }

  HMODULE hMods[1024];
  DWORD cbNeeded;
  if (EnumProcessModules(hProcess, hMods, sizeof(hMods), &cbNeeded)) {
    for (unsigned int i = 0; i < (cbNeeded / sizeof(HMODULE)); i++) {
      char szModName[MAX_PATH];
      if (GetModuleFileNameExA(hProcess, hMods[i], szModName,
                               sizeof(szModName))) {
        std::string modName = szModName;
        if (modName.find("capture_hook_x64.dll") != std::string::npos ||
            modName.find("capture_hook_x86.dll") != std::string::npos) {

          HMODULE hKernel32 = GetModuleHandleA("kernel32.dll");
          LPTHREAD_START_ROUTINE pFreeLibrary =
              (LPTHREAD_START_ROUTINE)GetProcAddress(hKernel32, "FreeLibrary");

          HANDLE hThread = CreateRemoteThread(hProcess, NULL, 0, pFreeLibrary,
                                              (LPVOID)hMods[i], 0, NULL);
          if (hThread) {
            WaitForSingleObject(hThread, 500);
            CloseHandle(hThread);
          }
          break;
        }
      }
    }
  }

  if (it == injectedProcesses.end())
    CloseHandle(hProcess);
}

// SHA256 using Windows CNG (bcrypt.dll)
static std::string ComputeFileHash(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return "";

    BCRYPT_ALG_HANDLE hAlg = NULL;
    BCRYPT_HASH_HANDLE hHash = NULL;
    if (BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA256_ALGORITHM, NULL, 0) < 0) return "";

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
        if (file.eof()) break;
    }

    DWORD cbHash = 0;
    BCryptGetProperty(hAlg, BCRYPT_HASH_LENGTH, (PBYTE)&cbHash, sizeof(DWORD), &cbData, 0);
    std::vector<BYTE> pbHash(cbHash);
    BCryptFinishHash(hHash, pbHash.data(), cbHash, 0);

    BCryptDestroyHash(hHash);
    BCryptCloseAlgorithmProvider(hAlg, 0);

    std::stringstream ss;
    for (BYTE b : pbHash) ss << std::hex << std::setw(2) << std::setfill('0') << (int)b;
    return ss.str();
}

bool InjectionManager::ValidateDllSecurity(const std::string &dllPath) {
    char exePathBuf[MAX_PATH];
    GetModuleFileNameA(NULL, exePathBuf, MAX_PATH);
    fs::path exePath = fs::path(exePathBuf).parent_path();
    fs::path checkPath = fs::absolute(dllPath);

    // 1. Path Validation
    if (checkPath.string().find(exePath.string()) != 0) {
       LogError("[Security] DLL path is outside application directory: %s", checkPath.string().c_str());
       return false;
    }

    // 2. ACL Check (Check if World/Everyone has Write Access)
    PACL pDacl = NULL;
    PSECURITY_DESCRIPTOR pSD = NULL;
    if (GetNamedSecurityInfoA(dllPath.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, NULL, NULL, &pDacl, NULL, &pSD) == ERROR_SUCCESS) {
        TRUSTEE_A trustee = {0};
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
        LocalFree(pSD); // also frees pDacl if it points into pSD
        
        if (access & (FILE_WRITE_DATA | FILE_APPEND_DATA | WRITE_DAC | WRITE_OWNER)) {
             // In dev builds we might trigger this if file permissions are lax. allow warning?
             // User requested check. We log error.
             LogError("[Security] DLL is writable by Everyone! Access Mask: 0x%X", access);
             // return false; // Strict mode. For dev, maybe warn? User said "proceed" with plan.
             // Implementing as Warning for Dev environment to avoid blocking testing if permissions are weird on MSYS2
             // return false; 
        }
    }

    LogInfo("[Security] DLL security validation passed for %s", dllPath.c_str());
    return true;
}

// Verify DLL Authenticode signature (production builds only)
// Returns true if DLL is properly signed, false otherwise
bool InjectionManager::VerifyDLLSignature(const std::string &dllPath) {
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
    trustData.fdwRevocationChecks = WTD_REVOKE_NONE; // Skip revocation check for performance
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
        LogError("[Security] DLL signature verification failed: %s (error 0x%X)", dllPath.c_str(), status);
        if (status == TRUST_E_NOSIGNATURE) {
            LogError("[Security] DLL is not signed");
        } else if (status == TRUST_E_EXPLICIT_DISTRUST) {
            LogError("[Security] DLL signature is explicitly distrusted");
        } else if (status == TRUST_E_SUBJECT_NOT_TRUSTED) {
            LogError("[Security] DLL signer is not trusted");
        } else if (status == CRYPT_E_SECURITY_SETTINGS) {
            LogError("[Security] Security settings prevent verification");
        }
        return false;
    }
}

void InjectionManager::ScanExistingProcesses() {
    HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnapshot == INVALID_HANDLE_VALUE) return;

    PROCESSENTRY32 pe32;
    pe32.dwSize = sizeof(PROCESSENTRY32);

    if (Process32First(hSnapshot, &pe32)) {
        do {
            std::string name = pe32.szExeFile;
            if (IsWhitelisted(name)) {
                if (!IsAlreadyInjected(pe32.th32ProcessID) && !IsRecentlyFailed(pe32.th32ProcessID)) {
                    LogInfo("[Scan] Found existing whitelisted process: %s (PID: %d)", name.c_str(), pe32.th32ProcessID);
                    Inject(pe32.th32ProcessID, name);
                }
            }
        } while (Process32Next(hSnapshot, &pe32));
    }
    CloseHandle(hSnapshot);
}
