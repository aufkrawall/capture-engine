#pragma once

#include <Wbemidl.h>
#include <comdef.h>
#include <windows.h>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include "../common/config.h"

// CRITICAL FIX: Inherit from enable_shared_from_this for safe delayed injection
class InjectionManager : public std::enable_shared_from_this<InjectionManager> {
public:
    InjectionManager(const AppConfig& config);
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
    bool Inject(DWORD pid, const std::string& processName);

    // Callback to execute before injection (e.g. to reload config)
    void SetOnInjectCallback(std::function<void(const std::string&)> callback);

    // Security Validation
    bool ValidateDllSecurity(const std::string& dllPath);
    bool VerifyDLLSignature(const std::string& dllPath);       // Verify Authenticode signature
    bool VerifyDLLHash(const std::string& dllPath);            // Verify SHA-256 hash (debug builds)
    std::string ComputeFileHash(const std::string& filePath);  // Compute SHA-256 hash of file

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
        virtual HRESULT STDMETHODCALLTYPE Indicate(LONG lObjectCount,
                                                   IWbemClassObject __RPC_FAR * __RPC_FAR * apObjArray);
        virtual HRESULT STDMETHODCALLTYPE SetStatus(LONG lFlags, HRESULT hResult, BSTR strParam,
                                                    IWbemClassObject __RPC_FAR* pObjParam);
    };

private:
    const AppConfig& config;
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
        uint64_t injectTime;  // When to inject (now + delay)
    };

    std::vector<FailedInjection> failedInjections;
    std::vector<PendingInjection> pendingInjections;

    // WMI Members
    IWbemServices* pSvc = nullptr;
    IWbemLocator* pLoc = nullptr;
    IUnsecuredApartment* pUnsecApp = nullptr;
    ProcessEventSink* pSink = nullptr;
    IWbemObjectSink* pStubSink = nullptr;

    void ScanExistingProcesses();
    bool IsWhitelisted(const std::string& processName);
    bool IsAlreadyInjected(DWORD pid);
    bool IsRecentlyFailed(DWORD pid);

    // CRITICAL FIX: Shutdown flag for thread safety
    std::atomic<bool> shuttingDown{false};

    // Inject moved to public
    std::function<void(const std::string&)> onInjectCallback;

public:
    // CRITICAL FIX: Make mutex and shutdown methods accessible to delayed injection threads
    mutable std::mutex injectMutex;  // Protects lists shared with WMI thread
    void RequestShutdown() { shuttingDown = true; }
    bool IsShuttingDown() const { return shuttingDown; }
};
