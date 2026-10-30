#pragma once

// CRITICAL: windows.h MUST be first for COM headers
#include <windows.h>

#include "common/config/config.h"
#include "common/logging/logging.h"
#include "process_start_poll.h"
#include "process_start_etw.h"
#include "common/platform/loader_init_apc_load.h"
#include <wbemidl.h>
#include <atomic>
#include <comdef.h>
#include <condition_variable>
#include <functional>
#include <list>
#include <map>
#include <thread>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

// Where an injector finds its package and whether it owns the process it runs in (DR-38, DR-39). A helper
// process owns itself; a runtime thread inside a client does not and leaves process-wide state to the client.
struct InjectionEnvironment {
  std::wstring packageDir; // the package directory, without a trailing separator
  bool ownsProcess = true;
};

class InjectionManager {
public:
  // The package host's default: the executable's own directory, owning its process. A runtime that is not the
  // package's own process passes its package directory explicitly.
  InjectionManager(const AppConfig &config);
  InjectionManager(const AppConfig &config, InjectionEnvironment environment);
  ~InjectionManager();

  // Begin process discovery only after the pre-injection publication callback
  // is installed. This closes the process-start callback/config race.
  bool StartMonitoring();

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
  bool HasPendingInjections();

  // Inject into a specific process (CreateRemoteThread - runs after loader)
  bool Inject(DWORD pid, const std::string &processName);

  // Early APC injection before application startup. Static import substitution
  // requires the separate creator-side pre-import transaction.
  // Requires process to be created with CREATE_SUSPENDED
  // The hook DLL is chosen from the target's architecture, exactly as Inject() does.
  bool InjectEarly(DWORD pid, HANDLE hMainThread);

  // Callback to execute before injection (e.g. to reload config)
  void SetOnInjectCallback(std::function<void(DWORD, const std::string &)> callback);

  using NgxOtaModeQuery = std::function<uint8_t()>;
  void SetNgxOtaModeQuery(NgxOtaModeQuery query);
  bool IsNgxOtaDisabled();
  bool TerminateNgxUpdaterIfDisabled(DWORD pid, const std::string &imageName,
                                    const char *sourceTag);
  void SweepRunningNgxUpdatersIfDisabled(const char *reason);

  // Security Validation
  bool ValidateDllSecurity(const std::wstring &dllPath);
  bool VerifyDLLSignature(
      const std::wstring &dllPath,
      bool logFailures = true); // Verify Authenticode signature

  // WMI Event Sink
  class ProcessEventSink : public IWbemObjectSink {
    LONG m_lRef;
    bool bDone;
    uint32_t activeCallbacks;
    std::mutex callbackMutex;
    std::condition_variable callbacksDrained;
    InjectionManager *pManager;
    // The log sink of the thread that created the sink; COM runs the callbacks on its own threads (DR-40).
    std::shared_ptr<ce::log::Sink> logSink;

  public:
    ProcessEventSink(InjectionManager *manager)
        : m_lRef(0), bDone(false), activeCallbacks(0), pManager(manager), logSink(ce::log::CurrentSink()) {}
    virtual ~ProcessEventSink() { MarkDoneAndDrain(); }

    bool EnterCallback();
    void LeaveCallback();
    void MarkDoneAndDrain();

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
  // Hook DLLs, their trust check and the benchmark folder resolve from here, never from the executable (DR-38).
  std::wstring packageDir_;
  bool ownsProcess_ = true;
  // UTF-16 is authoritative (it reaches the remote LoadLibraryW unchanged); the
  // UTF-8 copies exist only for logging.
  std::wstring hookDllPathX64W;
  std::wstring hookDllPathX86W;
  std::string hookDllPathX64;
  std::string hookDllPathX86;

  struct InjectedProcess {
    DWORD pid = 0;
    std::string name;
    HANDLE hProcess = nullptr;
    LPVOID remoteMemory = nullptr;  // Remote DLL-path memory retained until LoadLibrary consumes it
    HANDLE injectionThread = nullptr;  // Pending remote LoadLibrary thread, if any
    std::shared_ptr<ce::loader_init_apc::LoadRequest> apcLoad;
    HANDLE reactivateEvent = nullptr;  // Retained so a pre-load reactivation signal cannot disappear
    HANDLE vulkanReactivateEvent = nullptr;
    bool creationOnly = false;
    HANDLE startupRole = nullptr;
    HANDLE startupActive = nullptr;
  };

  mutable std::vector<InjectedProcess> injectedProcesses;

  bool InjectImpl(DWORD pid, const std::string& processName, bool creationOnly);
  void ScanStartupLaunchHosts();
  bool AttachStartupLaunchHost(DWORD pid, const std::string& name);
  void ServiceStartupLaunchHosts();
  // Removes the creation-only role from every creator again (no substitution
  // configured any more): the creator's own worker uninstalls its hooks and
  // leaves the process clean. Failing removal keeps the safe pass-through.
  void DetachStartupLaunchHosts(const char* reason);
  std::map<DWORD, std::string> pendingStartupLaunchHosts;

  struct FailedInjection {
    DWORD pid;
    uint64_t timestamp;
  };

  struct PendingInjection {
    DWORD pid;
    std::string name;
    std::string source;
    uint64_t injectTime; // When to inject (now + delay)
  };

  // A tracked process that died where no CE in-process handler could run (a
  // __fastfail termination reaches neither VEH, SEH nor the unhandled filter).
  // WER writes the only dump in that case, and WerFault is still producing it
  // when the exit is observed, so the claim is retried on later poll ticks
  // until it succeeds or the window expires - never waited for.
  struct PendingWerDumpAdoption {
    DWORD pid = 0;
    std::string name;
    DWORD exitCode = 0;
    uint64_t firstAttemptMs = 0;
  };

  std::vector<FailedInjection> failedInjections;
  std::vector<PendingInjection> pendingInjections;
  std::vector<PendingWerDumpAdoption> pendingWerDumpAdoptions;

  // WMI Members
  enum class WmiSubscriptionState : uint8_t {
    kInactive,
    kRealtimeSubscribing,
    kRealtimeActive,
    kFallbackRequested,
    kFallbackActive,
    kStopped,
  };

  IWbemServices *pSvc = nullptr;
  IWbemLocator *pLoc = nullptr;
  IUnsecuredApartment *pUnsecApp = nullptr;
  ProcessEventSink *pSink = nullptr;
  IWbemObjectSink *pStubSink = nullptr;
  bool wmiCoInitNeedsUninitialize = false;
  std::atomic<WmiSubscriptionState> wmiSubscriptionState{
      WmiSubscriptionState::kInactive};
  std::atomic<HRESULT> wmiFallbackReason{S_OK};
  // Real-time ETW monitor (via elevation service) and unelevated polling fallback.
  ce::process_start::EtwMonitor processStartEtwMonitor;
  bool etwMonitoringSelected = false;
  ce::process_start::Poller processStartPoller;
  std::mutex monitoringMutex;
  bool monitoringStarted = false;
  bool monitoringInitialized = false;
  std::atomic<bool> startupLaunchScanPending{false};

  void ScanExistingProcesses();
  HRESULT StartPolledWmiFallback(HRESULT reason, const char *failurePhase);
  void HandlePolledProcessStart(DWORD pid, const std::string &imageName);
  void HandleEtwProcessStart(DWORD pid, const std::string &imageName);
  bool RequestWmiFallback(HRESULT reason);
  void ServiceWmiFallbackRequest();
  void ServiceEtwFallback();
  void EjectWithDeadline(DWORD pid, ULONGLONG deadline);
  void LaunchDelayedInjectionThread(DWORD pid, const std::string &name,
                                    const char *sourceTag);
  void ReapCompletedDelayedInjectionThreadsLocked();
  bool IsWhitelisted(DWORD pid, const std::string &processName);
  // True (after one rate-limited log line) when `pid` belongs to this CaptureEngine runtime: never injected,
  // whatever the whitelist says (injection_policy::RuntimeTargetRefusalReason).
  bool RefuseRuntimeProcess(DWORD pid, const std::string &name,
                            const char *stage);
  bool IsAlreadyInjected(DWORD pid);
  bool IsAlreadyInjectedLocked(DWORD pid);  // Assumes injectMutex is held
  bool IsAlreadyPendingLocked(DWORD pid);
  bool IsRecentlyFailed(DWORD pid);
  bool IsRecentlyFailedLocked(DWORD pid);   // Assumes injectMutex is held
  void NoteTrackedProcessExitLocked(DWORD pid, const std::string &name,
                                    DWORD exitCode);
  void ServicePendingWerDumpAdoptionsLocked(uint64_t nowMs);

  // CRITICAL FIX: Shutdown flag for thread safety
  std::atomic<bool> shuttingDown{false};

  // CRITICAL FIX: Track delayed injection threads for proper cleanup
  std::list<std::thread> delayedInjectionThreads;
  std::unordered_set<DWORD> delayedInjectionPids;
  std::mutex threadListMutex;

  // Inject moved to public
  std::mutex injectCallbackMutex;
  std::function<void(DWORD, const std::string &)> onInjectCallback;
  NgxOtaModeQuery ngxOtaModeQuery;

public:
  // CRITICAL FIX: Make mutex and shutdown methods accessible to delayed
  // injection threads
  mutable std::mutex injectMutex; // Protects lists shared with WMI thread
  void RequestShutdown() { shuttingDown = true; }
  bool IsShuttingDown() const { return shuttingDown; }
  
  // CRITICAL FIX: Wait for all delayed injection threads to complete
  void WaitForInjectionThreads(int timeoutMs = 5000);
};
