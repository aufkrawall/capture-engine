#pragma once

#include "app_audio_capture.h"

#include <combaseapi.h>

#include <propvarutil.h>

#include <psapi.h>

#include <tlhelp32.h>

#include <algorithm>

#include <array>

#include <chrono>

#include <cstring>

#include <exception>

#include <functional>

#include <limits>

#include <new>

#include <utility>

#include "../common/raii_helpers.h"

#include "audio_capture.h"  // For AudioPacket

#include "audio_time_utils.h"

#include "mediaengine.h"  // For DLL_Log

#include "process_tree_selection.h"

// Required for ActivateAudioInterfaceAsync
#pragma comment(lib, "mmdevapi.lib")

#define REFTIMES_PER_SEC 10000000

#define REFTIMES_PER_MILLISEC 10000

// Virtual audio device string for process loopback
#ifndef VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK
#define VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK L"VAD\\Process_Loopback"
#endif

// Activation type enum
typedef enum AUDIOCLIENT_ACTIVATION_TYPE {
    AUDIOCLIENT_ACTIVATION_TYPE_DEFAULT = 0,
    AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK = 1
} AUDIOCLIENT_ACTIVATION_TYPE;

// Process loopback mode enum
typedef enum PROCESS_LOOPBACK_MODE {
    PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE = 0,
    PROCESS_LOOPBACK_MODE_EXCLUDE_TARGET_PROCESS_TREE = 1
} PROCESS_LOOPBACK_MODE;

// Process loopback parameters
typedef struct AUDIOCLIENT_PROCESS_LOOPBACK_PARAMS {
    DWORD TargetProcessId;
    PROCESS_LOOPBACK_MODE ProcessLoopbackMode;
} AUDIOCLIENT_PROCESS_LOOPBACK_PARAMS;

// Activation parameters structure
typedef struct AUDIOCLIENT_ACTIVATION_PARAMS {
    AUDIOCLIENT_ACTIVATION_TYPE ActivationType;
    union {
        AUDIOCLIENT_PROCESS_LOOPBACK_PARAMS ProcessLoopbackParams;
    };
} AUDIOCLIENT_ACTIVATION_PARAMS;

class AppAudioCapture;

// IEEE Float subformat GUID
inline bool app_audio_capture_IsIEEEFloat(const GUID& g) {
    return g.Data1 == 0x00000003 && g.Data2 == 0x0000 && g.Data3 == 0x0010 && g.Data4[0] == 0x80 &&
           g.Data4[1] == 0x00 && g.Data4[2] == 0x00 && g.Data4[3] == 0xaa && g.Data4[4] == 0x00 && g.Data4[5] == 0x38 &&
           g.Data4[6] == 0x9b && g.Data4[7] == 0x71;
}

inline std::vector<ce::process_loopback::ProcessTreeEntry> app_audio_capture_SnapshotProcessTree() {
    std::vector<ce::process_loopback::ProcessTreeEntry> processes;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return processes;
    }

    PROCESSENTRY32W processEntry = {};
    processEntry.dwSize = sizeof(processEntry);
    if (Process32FirstW(snapshot, &processEntry)) {
        do {
            char executableName[MAX_PATH] = {};
            if (WideCharToMultiByte(CP_UTF8, 0, processEntry.szExeFile, -1, executableName, MAX_PATH, nullptr,
                                    nullptr) > 0) {
                processes.push_back({processEntry.th32ProcessID, processEntry.th32ParentProcessID, executableName});
            }
        } while (Process32NextW(snapshot, &processEntry));
    }
    CloseHandle(snapshot);
    return processes;
}

class AppAudioCapture::ActivationHandler : public IActivateAudioInterfaceCompletionHandler {
public:
    ActivationHandler()
        : refCount(1),
          completeEvent(CreateEventW(nullptr, TRUE, FALSE, nullptr)),
          resultCode(E_FAIL),
          audioClient(nullptr) {}

    virtual ~ActivationHandler() {
        if (completeEvent) {
            CloseHandle(completeEvent);
        }
        // Deliberately do not release audioClient here. Process-loopback client
        // release crosses the same AudioSes teardown crash boundary documented in
        // AbandonClientInterfaces(). A timed-out late activation is rare and the
        // short-lived media process reclaims it at exit.
    }

    // IUnknown - must return S_OK for IAgileObject to prevent
    // E_ILLEGAL_METHOD_CALL
    STDMETHODIMP QueryInterface(REFIID riid, void** ppvObject) override {
        if (!ppvObject) {
            return E_POINTER;
        }
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IActivateAudioInterfaceCompletionHandler) ||
            riid == IID_IAgileObject) {
            *ppvObject = static_cast<IActivateAudioInterfaceCompletionHandler*>(this);
            AddRef();
            return S_OK;
        }
        *ppvObject = nullptr;
        return E_NOINTERFACE;
    }

    STDMETHODIMP_(ULONG) AddRef() override {
        return InterlockedIncrement(&refCount);
    }

    STDMETHODIMP_(ULONG) Release() override {
        ULONG count = InterlockedDecrement(&refCount);
        if (count == 0) {
            delete this;
        }
        return count;
    }

    // IActivateAudioInterfaceCompletionHandler
    STDMETHODIMP ActivateCompleted(IActivateAudioInterfaceAsyncOperation* activateOperation) override {
        HRESULT hrActivate = E_FAIL;
        IUnknown* pUnk = nullptr;

        HRESULT hr = activateOperation ? activateOperation->GetActivateResult(&hrActivate, &pUnk) : E_POINTER;
        if (SUCCEEDED(hr) && SUCCEEDED(hrActivate) && pUnk) {
            hr = pUnk->QueryInterface(__uuidof(IAudioClient), reinterpret_cast<void**>(&audioClient));
        }
        if (pUnk) {
            pUnk->Release();
        }

        resultCode = SUCCEEDED(hr) ? hrActivate : hr;

        // Signal completion
        if (completeEvent) {
            SetEvent(completeEvent);
        }
        return S_OK;
    }

    HRESULT GetResult() const {
        return resultCode;
    }
    HANDLE GetEvent() const {
        return completeEvent;
    }
    IAudioClient* TakeAudioClient() {
        IAudioClient* client = audioClient;
        audioClient = nullptr;
        return client;
    }

private:
    LONG refCount;
    HANDLE completeEvent;
    HRESULT resultCode;
    IAudioClient* audioClient;
};
