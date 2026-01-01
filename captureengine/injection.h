#pragma once

#include "../common/config.h"
#include <string>
#include <vector>
#include <windows.h>
#include <Wbemidl.h>
#include <comdef.h>
#include <mutex>
#include <mutex>
#include <functional>

class InjectionManager {
public:
  InjectionManager(const AppConfig &config);
  ~InjectionManager();

  // Check running processes (cleanup) and process pending injections
  void Update();

  // WMI Methods
  bool InitializeWMI();
  void ShutdownWMI();

  // Force remove injection from specific PID
  void Eject(DWORD pid);

  // Eject all
  void EjectAll();

  // Check if any process is currently injected
  bool HasActiveInjections() const;
  
  // Inject into a specific process
  bool Inject(DWORD pid, const std::string &processName);

  // Callback to execute before injection (e.g. to reload config)
  void SetOnInjectCallback(std::function<void(const std::string&)> callback);

  // Security Validation
  bool ValidateDllSecurity(const std::string &dllPath);
  bool VerifyDLLSignature(const std::string &dllPath); // Verify Authenticode signature

  // WMI Event Sink
  class ProcessEventSink : public IWbemObjectSink {
      LONG m_lRef;
      bool bDone;
      InjectionManager* pManager;

  public:
      ProcessEventSink(InjectionManager* manager) : m_lRef(0), bDone(false), pManager(manager) {}
      virtual ~ProcessEventSink() { bDone = true; }

      virtual ULONG STDMETHODCALLTYPE AddRef();
      virtual ULONG STDMETHODCALLTYPE Release();
      virtual HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv);
      virtual HRESULT STDMETHODCALLTYPE Indicate(LONG lObjectCount, IWbemClassObject __RPC_FAR* __RPC_FAR* apObjArray);
      virtual HRESULT STDMETHODCALLTYPE SetStatus(LONG lFlags, HRESULT hResult, BSTR strParam, IWbemClassObject __RPC_FAR* pObjParam);
  };

private:
  const AppConfig &config;
  std::string hookDllPathX64;
  std::string hookDllPathX86;

  struct InjectedProcess {
    DWORD pid;
    std::string name;
    HANDLE hProcess;
  };

  std::vector<InjectedProcess> injectedProcesses;

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
  mutable std::mutex injectMutex; // Protects lists shared with WMI thread

  // WMI Members
  IWbemServices* pSvc = nullptr;
  IWbemLocator* pLoc = nullptr;
  IUnsecuredApartment* pUnsecApp = nullptr;
  ProcessEventSink* pSink = nullptr;
  IWbemObjectSink* pStubSink = nullptr;

  void ScanExistingProcesses();
  bool IsWhitelisted(const std::string &processName);
  bool IsAlreadyInjected(DWORD pid);
  bool IsRecentlyFailed(DWORD pid);
  // Inject moved to public
  std::function<void(const std::string&)> onInjectCallback;
};
