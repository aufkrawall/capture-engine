#pragma once

// CRITICAL: windows.h MUST be first for COM headers
#include <windows.h>

#include "../common/config.h"
#include <Wbemidl.h>
#include <atomic>
#include <comdef.h>
#include <functional>
#include <list>
#include <thread>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

// CRITICAL FIX: Inherit from enable_shared_from_this for safe delayed injection
class InjectionManager : public std::enable_shared_from_this<InjectionManager> {
public:
  InjectionManager(const AppConfig &config);
  ~InjectionManager();

  // Check running processes (cleanup) and process pending injections
  void Update();

  // Hot-reload whitelist/injection settings without restarting the injector.
  void UpdateConfig(const AppConfig &newConfig);
  void RescanExistingProcesses();

  // WMI Methods
  bool InitializeWMI();
  void ShutdownWMI();

  // Force remove injection from specific PID
  void Eject(DWORD pid);

  // Eject all
  void EjectAll();

  // Check if any process is currently injected
  bool HasActiveInjections() const;

  // Inject into a specific process (CreateRemoteThread - runs after loader)
  bool Inject(DWORD pid, const std::string &processName);

  // Early injection using APC - runs before loader/import resolution
  // Requires process to be created with CREATE_SUSPENDED
  bool InjectEarly(DWORD pid, const std::string &dllPath, HANDLE hMainThread);

  // Callback to execute before injection (e.g. to reload config)
  void SetOnInjectCallback(std::function<void(const std::string &)> callback);

  // Security Validation
  bool ValidateDllSecurity(const std::string &dllPath);
  bool VerifyDLLSignature(
      const std::string &dllPath,
      bool logFailures = true); // Verify Authenticode signature
  bool VerifyDLLHash(
      const std::string &dllPath); // Verify SHA-256 hash (debug builds)
  std::string
  ComputeFileHash(const std::string &filePath); // Compute SHA-256 hash of file

  // WMI Event Sink
  class ProcessEventSink : public IWbemObjectSink {
    LONG m_lRef;
    bool bDone;
    InjectionManager *pManager;

  public:
    ProcessEventSink(InjectionManager *manager)
        : m_lRef(0), bDone(false), pManager(manager) {}
    virtual ~ProcessEventSink() { bDone = true; }

    virtual ULONG STDMETHODCALLTYPE AddRef();
    virtual ULONG STDMETHODCALLTYPE Release();
    virtual HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv);
    virtual HRESULT STDMETHODCALLTYPE Indicate(LONG lObjectCount,
                                               IWbemClassObject __RPC_FAR *
                                                   __RPC_FAR * apObjArray);
    virtual HRESULT STDMETHODCALLTYPE
    SetStatus(LONG lFlags, HRESULT hResult, BSTR strParam,
              IWbemClassObject __RPC_FAR *pObjParam);
  };

private:
  AppConfig config;
  mutable std::mutex configMutex;
  std::string hookDllPathX64;
  std::string hookDllPathX86;

  struct InjectedProcess {
    DWORD pid;
    std::string name;
    HANDLE hProcess;
    LPVOID remoteMemory;  // Remote memory allocated for DLL path (APC injection)
  };

  mutable std::vector<InjectedProcess> injectedProcesses;

  struct FailedInjection {
    DWORD pid;
    uint64_t timestamp;
  };

  struct PendingInjection {
    DWORD pid;
    std::string name;
    uint64_t injectTime; // When to inject (now + delay)
  };

  std::vector<FailedInjection> failedInjections;
  std::vector<PendingInjection> pendingInjections;

  // WMI Members
  IWbemServices *pSvc = nullptr;
  IWbemLocator *pLoc = nullptr;
  IUnsecuredApartment *pUnsecApp = nullptr;
  ProcessEventSink *pSink = nullptr;
  IWbemObjectSink *pStubSink = nullptr;

  void ScanExistingProcesses();
  bool IsWhitelisted(const std::string &processName);
  bool IsAlreadyInjected(DWORD pid);
  bool IsRecentlyFailed(DWORD pid);

  // CRITICAL FIX: Shutdown flag for thread safety
  std::atomic<bool> shuttingDown{false};

  // CRITICAL FIX: Track delayed injection threads for proper cleanup
  std::list<std::thread> delayedInjectionThreads;
  std::mutex threadListMutex;

  // Inject moved to public
  std::function<void(const std::string &)> onInjectCallback;

public:
  // CRITICAL FIX: Make mutex and shutdown methods accessible to delayed
  // injection threads
  mutable std::mutex injectMutex; // Protects lists shared with WMI thread
  void RequestShutdown() { shuttingDown = true; }
  bool IsShuttingDown() const { return shuttingDown; }
  
  // CRITICAL FIX: Wait for all delayed injection threads to complete
  void WaitForInjectionThreads(int timeoutMs = 5000);
};
