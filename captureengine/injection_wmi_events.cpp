#include "injection_internal.h"

#include <limits>

namespace {

struct ProcessStartNotification {
    DWORD pid = 0;
    std::string name;
    const char* sourceTag = "WMI";
};

bool TryReadProcessId(const _variant_t& value, DWORD* pid) {
    if (!pid) {
        return false;
    }

    uint64_t raw = 0;
    switch (value.vt) {
        case VT_I4:
            if (value.lVal <= 0)
                return false;
            raw = static_cast<uint32_t>(value.lVal);
            break;
        case VT_UI4:
            raw = value.ulVal;
            break;
        case VT_I8:
            if (value.llVal <= 0)
                return false;
            raw = static_cast<uint64_t>(value.llVal);
            break;
        case VT_UI8:
            raw = value.ullVal;
            break;
        default:
            return false;
    }
    if (raw == 0 || raw > std::numeric_limits<DWORD>::max()) {
        return false;
    }
    *pid = static_cast<DWORD>(raw);
    return true;
}

std::string Utf8FromBstr(BSTR value) {
    if (!value) {
        return {};
    }
    const UINT wideLength = SysStringLen(value);
    const int byteLength = WideCharToMultiByte(CP_UTF8, 0, value, static_cast<int>(wideLength), nullptr, 0, nullptr,
                                                nullptr);
    if (byteLength <= 0) {
        return {};
    }
    std::string result(static_cast<size_t>(byteLength), '\0');
    if (WideCharToMultiByte(CP_UTF8, 0, value, static_cast<int>(wideLength), result.data(), byteLength, nullptr,
                            nullptr) != byteLength) {
        return {};
    }
    return result;
}

bool TryReadProcessFields(IWbemClassObject* object, const wchar_t* nameProperty, const wchar_t* pidProperty,
                          ProcessStartNotification* notification) {
    if (!object || !notification) {
        return false;
    }
    _variant_t nameValue;
    _variant_t pidValue;
    if (FAILED(object->Get(nameProperty, 0, &nameValue, nullptr, nullptr)) || nameValue.vt != VT_BSTR ||
        FAILED(object->Get(pidProperty, 0, &pidValue, nullptr, nullptr))) {
        return false;
    }
    notification->name = Utf8FromBstr(nameValue.bstrVal);
    return !notification->name.empty() && TryReadProcessId(pidValue, &notification->pid);
}

bool TryReadProcessStartNotification(IWbemClassObject* eventObject, ProcessStartNotification* notification) {
    if (TryReadProcessFields(eventObject, L"ProcessName", L"ProcessID", notification)) {
        notification->sourceTag = "WMIStartTrace";
        return true;
    }

    _variant_t targetValue;
    if (!eventObject || FAILED(eventObject->Get(L"TargetInstance", 0, &targetValue, nullptr, nullptr))) {
        return false;
    }
    IUnknown* targetUnknown = nullptr;
    if (targetValue.vt == VT_UNKNOWN) {
        targetUnknown = targetValue.punkVal;
    } else if (targetValue.vt == VT_DISPATCH) {
        targetUnknown = targetValue.pdispVal;
    }
    if (!targetUnknown) {
        return false;
    }

    IWbemClassObject* target = nullptr;
    if (FAILED(targetUnknown->QueryInterface(IID_IWbemClassObject, reinterpret_cast<void**>(&target))) || !target) {
        return false;
    }
    const bool parsed = TryReadProcessFields(target, L"Name", L"ProcessId", notification);
    target->Release();
    if (parsed) {
        notification->sourceTag = "WMIPollFallback";
    }
    return parsed;
}

double QueryProcessAgeMs(DWORD pid) {
    ce::HandleGuard process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
    FILETIME creation = {};
    FILETIME exit = {};
    FILETIME kernel = {};
    FILETIME user = {};
    FILETIME now = {};
    if (!process || !GetProcessTimes(process.get(), &creation, &exit, &kernel, &user)) {
        return -1.0;
    }
    GetSystemTimeAsFileTime(&now);
    ULARGE_INTEGER creationTicks = {};
    ULARGE_INTEGER nowTicks = {};
    creationTicks.LowPart = creation.dwLowDateTime;
    creationTicks.HighPart = creation.dwHighDateTime;
    nowTicks.LowPart = now.dwLowDateTime;
    nowTicks.HighPart = now.dwHighDateTime;
    if (nowTicks.QuadPart < creationTicks.QuadPart) {
        return -1.0;
    }
    return static_cast<double>(nowTicks.QuadPart - creationTicks.QuadPart) / 10000.0;
}

}  // namespace

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
        *ppv = static_cast<IWbemObjectSink*>(this);
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

HRESULT STDMETHODCALLTYPE InjectionManager::ProcessEventSink::Indicate(LONG objectCount,
                                                                        IWbemClassObject** eventObjects) {
    if (!EnterCallback()) {
        return WBEM_S_NO_ERROR;
    }
    CE_SCOPE_EXIT(LeaveCallback());

    try {
        if (!pManager || !eventObjects) {
            return WBEM_S_NO_ERROR;
        }
        for (LONG index = 0; index < objectCount; ++index) {
            ProcessStartNotification notification;
            if (!TryReadProcessStartNotification(eventObjects[index], &notification) ||
                !pManager->IsWhitelisted(notification.name)) {
                continue;
            }
            const double processAgeMs = QueryProcessAgeMs(notification.pid);
            LogInfo("[%s] Process start received for %s (PID: %lu, age=%.3f ms)", notification.sourceTag,
                    notification.name.c_str(), static_cast<unsigned long>(notification.pid), processAgeMs);
            pManager->LaunchDelayedInjectionThread(notification.pid, notification.name, notification.sourceTag);
        }
    } catch (const _com_error& error) {
        LogError("WMI Indicate: COM exception 0x%lX: %s", static_cast<unsigned long>(error.Error()),
                 error.ErrorMessage());
    } catch (const std::exception& error) {
        LogError("WMI Indicate: Exception: %s", error.what());
    } catch (...) {
        LogError("WMI Indicate: Unknown exception caught");
    }
    return WBEM_S_NO_ERROR;
}

HRESULT STDMETHODCALLTYPE InjectionManager::ProcessEventSink::SetStatus(LONG flags, HRESULT result, BSTR parameter,
                                                                         IWbemClassObject* object) {
    (void)object;
    if (flags != WBEM_STATUS_COMPLETE || !FAILED(result) || !EnterCallback()) {
        return WBEM_S_NO_ERROR;
    }
    CE_SCOPE_EXIT(LeaveCallback());
    if (!pManager) {
        return WBEM_S_NO_ERROR;
    }

    // Microsoft explicitly forbids calling back into WMI from an event sink.
    // Publish the failure for Update() to service on the manager's owning
    // thread instead. The boolean also distinguishes the active realtime
    // subscription from cancellation completion belonging to a rejected or
    // intentionally stopped call.
    const bool fallbackQueued = pManager->RequestWmiFallback(result);
    if (result == WBEM_E_CALL_CANCELLED && !fallbackQueued) {
        LogDebug("[Inject] WMI process-start subscription cancellation acknowledged");
    } else if (fallbackQueued) {
        LogWarn("[Inject] Event-driven WMI process-start subscription stopped (hr=0x%08lX, status=%ls); "
                "owner-thread fallback queued",
                static_cast<unsigned long>(result), parameter ? parameter : L"unavailable");
    } else {
        LogError("[Inject] WMI process-start fallback subscription stopped (hr=0x%08lX, status=%ls)",
                 static_cast<unsigned long>(result), parameter ? parameter : L"unavailable");
    }
    return WBEM_S_NO_ERROR;
}
