#include "startup_control.h"
#include "common/ipc/elevation_windows.h"
#include <initguid.h>
#include <taskschd.h>
#include <filesystem>
#include "common/logging/logging.h"

namespace ce::startup {
namespace {
template <typename T>
struct Com {
    T* value = nullptr;
    ~Com() {
        if (value)
            value->Release();
    }
    T** Put() {
        return &value;
    }
    T* operator->() const {
        return value;
    }
};
struct Text {
    BSTR value;
    explicit Text(const std::wstring& text) : value(SysAllocStringLen(text.data(), static_cast<UINT>(text.size()))) {}
    ~Text() {
        SysFreeString(value);
    }
    operator BSTR() const {
        return value;
    }
};
struct Apartment {
    HRESULT status = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ~Apartment() {
        if (SUCCEEDED(status))
            CoUninitialize();
    }
};

DWORD FromResult(HRESULT result) {
    return SUCCEEDED(result)                            ? ERROR_SUCCESS
           : HRESULT_FACILITY(result) == FACILITY_WIN32 ? HRESULT_CODE(result)
                                                        : static_cast<DWORD>(result);
}

struct RunValue {
    bool present = false;
    DWORD type = REG_SZ;
    std::vector<BYTE> bytes;
};

DWORD OpenRunKey(HKEY& key) {
    const std::wstring path = OwnerSid() + kPerUserRunSubkey;
    return RegCreateKeyExW(HKEY_USERS, path.c_str(), 0, nullptr, 0, KEY_QUERY_VALUE | KEY_SET_VALUE, nullptr, &key,
                           nullptr);
}

DWORD WriteRun(HKEY key, const std::wstring& name, const RunValue& value) {
    if (!value.present) {
        const LSTATUS result = RegDeleteValueW(key, name.c_str());
        return result == ERROR_FILE_NOT_FOUND ? ERROR_SUCCESS : result;
    }
    return RegSetValueExW(key, name.c_str(), 0, value.type, value.bytes.data(),
                          static_cast<DWORD>(value.bytes.size()));
}

HRESULT MakeDefinition(ITaskService* service, const AutostartTarget& target, ITaskDefinition** output) {
    Com<ITaskDefinition> definition;
    HRESULT result = service->NewTask(0, definition.Put());
    if (FAILED(result))
        return result;
    Com<IPrincipal> principal;
    Com<ITriggerCollection> triggers;
    Com<ITrigger> trigger;
    Com<ILogonTrigger> logon;
    Com<IActionCollection> actions;
    Com<IAction> action;
    Com<IExecAction> exec;
    Com<ITaskSettings> settings;
    Text owner(OwnerSid());
    Text executable(target.executable);
    Text directory(std::filesystem::path(target.executable).parent_path().wstring());
    Text arguments(target.arguments);
    Text duration(L"PT0S");
    if (FAILED(result = definition->get_Principal(principal.Put())) || FAILED(result = principal->put_UserId(owner)) ||
        FAILED(result = principal->put_LogonType(TASK_LOGON_INTERACTIVE_TOKEN)) ||
        FAILED(result = principal->put_RunLevel(TASK_RUNLEVEL_HIGHEST)) ||
        FAILED(result = definition->get_Triggers(triggers.Put())) ||
        FAILED(result = triggers->Create(TASK_TRIGGER_LOGON, trigger.Put())) ||
        FAILED(result = trigger->QueryInterface(IID_ILogonTrigger, reinterpret_cast<void**>(logon.Put()))) ||
        FAILED(result = logon->put_UserId(owner)) || FAILED(result = definition->get_Actions(actions.Put())) ||
        FAILED(result = actions->Create(TASK_ACTION_EXEC, action.Put())) ||
        FAILED(result = action->QueryInterface(IID_IExecAction, reinterpret_cast<void**>(exec.Put()))) ||
        FAILED(result = exec->put_Path(executable)) || FAILED(result = exec->put_Arguments(arguments)) ||
        FAILED(result = exec->put_WorkingDirectory(directory)) ||
        FAILED(result = definition->get_Settings(settings.Put())) ||
        FAILED(result = settings->put_DisallowStartIfOnBatteries(VARIANT_FALSE)) ||
        FAILED(result = settings->put_StopIfGoingOnBatteries(VARIANT_FALSE)) ||
        FAILED(result = settings->put_ExecutionTimeLimit(duration)) ||
        FAILED(result = settings->put_StartWhenAvailable(VARIANT_TRUE)) ||
        FAILED(result = settings->put_MultipleInstances(TASK_INSTANCES_IGNORE_NEW)))
        return result;
    *output = definition.value;
    definition.value = nullptr;
    return S_OK;
}
}  // namespace

std::wstring AutostartTaskName(const AutostartTarget& target) {
    return target.name + L".Autostart." + OwnerSid();
}

// The Run value is the quoted program followed by its argument tail.
std::wstring AutostartCommandLine(const AutostartTarget& target) {
    const std::wstring program = ce::elevation::QuoteArgument(target.executable);
    return target.arguments.empty() ? program : program + L" " + target.arguments;
}

AutostartTarget ApplicationAutostartTarget() {
    AutostartTarget target;
    target.executable = ce::elevation::ExecutablePath();
    target.arguments = kApplicationAutostartArguments;
    return target;
}

bool IsApplicationAutostart(const AutostartTarget& target) {
    const AutostartTarget application = ApplicationAutostartTarget();
    return _wcsicmp(target.executable.c_str(), application.executable.c_str()) == 0 &&
           target.arguments == application.arguments && target.name == application.name;
}

DWORD ConfigureAutostart(const Preferences& preferences, bool administratorAccount) {
    return ConfigureClientAutostart(ApplicationAutostartTarget(),
                                    SelectRegistration(preferences, administratorAccount));
}

DWORD ConfigureClientAutostart(const AutostartTarget& target, Registration mode) {
    if (!IsValidAutostartTarget(target))
        return ERROR_INVALID_PARAMETER;
    Apartment apartment;
    if (FAILED(apartment.status) && apartment.status != RPC_E_CHANGED_MODE)
        return FromResult(apartment.status);
    HKEY run = nullptr;
    DWORD error = OpenRunKey(run);
    if (error != ERROR_SUCCESS)
        return error;
    RunValue oldRun;
    DWORD bytes = 0;
    const LSTATUS query = RegQueryValueExW(run, target.name.c_str(), nullptr, &oldRun.type, nullptr, &bytes);
    if (query == ERROR_SUCCESS && bytes <= 65536) {
        oldRun.bytes.resize(bytes);
        const LSTATUS read =
            RegQueryValueExW(run, target.name.c_str(), nullptr, &oldRun.type, oldRun.bytes.data(), &bytes);
        if (read != ERROR_SUCCESS) {
            RegCloseKey(run);
            return read;
        }
        oldRun.present = true;
    } else if (query != ERROR_FILE_NOT_FOUND) {
        RegCloseKey(run);
        return ERROR_INVALID_DATA;
    }
    Com<ITaskService> scheduler;
    Com<ITaskFolder> root;
    VARIANT empty;
    VariantInit(&empty);
    HRESULT result = CoCreateInstance(CLSID_TaskScheduler, nullptr, CLSCTX_INPROC_SERVER, IID_ITaskService,
                                      reinterpret_cast<void**>(scheduler.Put()));
    if (SUCCEEDED(result))
        result = scheduler->Connect(empty, empty, empty, empty);
    Text rootPath(L"\\");
    if (SUCCEEDED(result))
        result = scheduler->GetFolder(rootPath, root.Put());
    if (FAILED(result)) {
        RegCloseKey(run);
        return FromResult(result);
    }
    Text name(AutostartTaskName(target));
    Com<IRegisteredTask> previous;
    BSTR previousXml = nullptr;
    const HRESULT lookup = root->GetTask(name, previous.Put());
    if (SUCCEEDED(lookup))
        result = previous->get_Xml(&previousXml);
    else if (lookup != HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND))
        result = lookup;
    if (FAILED(result)) {
        RegCloseKey(run);
        return FromResult(result);
    }
    VARIANT user;
    VariantInit(&user);
    user.vt = VT_BSTR;
    user.bstrVal = SysAllocString(OwnerSid().c_str());
    VARIANT security;
    VariantInit(&security);
    security.vt = VT_BSTR;
    security.bstrVal = SysAllocString((L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGXSD;;;" + OwnerSid() + L")").c_str());
    bool taskChanged = false;
    if (mode == Registration::ElevatedTask) {
        Com<ITaskDefinition> definition;
        Com<IRegisteredTask> registered;
        result = MakeDefinition(scheduler.value, target, definition.Put());
        if (SUCCEEDED(result))
            result = root->RegisterTaskDefinition(name, definition.value, TASK_CREATE_OR_UPDATE, user, empty,
                                                  TASK_LOGON_INTERACTIVE_TOKEN, security, registered.Put());
        taskChanged = SUCCEEDED(result);
        error = taskChanged ? WriteRun(run, target.name, {}) : FromResult(result);
    } else {
        RunValue desired;
        if (mode == Registration::UserRun) {
            const std::wstring command = AutostartCommandLine(target);
            desired.present = true;
            desired.bytes.resize((command.size() + 1) * sizeof(wchar_t));
            std::memcpy(desired.bytes.data(), command.c_str(), desired.bytes.size());
        }
        error = WriteRun(run, target.name, desired);
        if (error == ERROR_SUCCESS && previous.value) {
            result = root->DeleteTask(name, 0);
            taskChanged = SUCCEEDED(result);
            error = FromResult(result);
        }
    }
    if (error != ERROR_SUCCESS) {
        const DWORD restoreRun = WriteRun(run, target.name, oldRun);
        if (restoreRun)
            LogError("[Startup] Run registration rollback failed (error=%lu)", restoreRun);
        if (taskChanged) {
            if (previousXml) {
                Com<IRegisteredTask> restored;
                const HRESULT restoredResult =
                    root->RegisterTask(name, previousXml, TASK_CREATE_OR_UPDATE, user, empty,
                                       TASK_LOGON_INTERACTIVE_TOKEN, security, restored.Put());
                if (FAILED(restoredResult))
                    LogError("[Startup] Scheduled task rollback failed (error=%lu)", FromResult(restoredResult));
            } else {
                const HRESULT removed = root->DeleteTask(name, 0);
                if (FAILED(removed))
                    LogError("[Startup] New task rollback failed (error=%lu)", FromResult(removed));
            }
        }
    }
    SysFreeString(previousXml);
    VariantClear(&user);
    VariantClear(&security);
    RegCloseKey(run);
    return error;
}

AutostartState QueryClientAutostart(const AutostartTarget& target) {
    if (!IsValidAutostartTarget(target))
        return AutostartState::Unknown;
    const std::wstring path = OwnerSid() + kPerUserRunSubkey;
    wchar_t command[32768]{};
    DWORD size = sizeof(command);
    const bool hasRun = RegGetValueW(HKEY_USERS, path.c_str(), target.name.c_str(), RRF_RT_REG_SZ, nullptr, command,
                                     &size) == ERROR_SUCCESS;
    const bool runMatches = hasRun && AutostartCommandLine(target) == command;
    Apartment apartment;
    if (FAILED(apartment.status) && apartment.status != RPC_E_CHANGED_MODE)
        return AutostartState::Unknown;
    Com<ITaskService> scheduler;
    Com<ITaskFolder> root;
    Com<IRegisteredTask> task;
    VARIANT empty;
    VariantInit(&empty);
    if (FAILED(CoCreateInstance(CLSID_TaskScheduler, nullptr, CLSCTX_INPROC_SERVER, IID_ITaskService,
                                reinterpret_cast<void**>(scheduler.Put()))) ||
        FAILED(scheduler->Connect(empty, empty, empty, empty)))
        return AutostartState::Unknown;
    Text rootPath(L"\\");
    Text name(AutostartTaskName(target));
    if (FAILED(scheduler->GetFolder(rootPath, root.Put())))
        return AutostartState::Unknown;
    const HRESULT lookup = root->GetTask(name, task.Put());
    const bool hasTask = SUCCEEDED(lookup);
    if (!hasTask && lookup != HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND))
        return AutostartState::Unknown;
    bool taskMatches = false;
    if (hasTask) {
        Com<ITaskDefinition> definition;
        Com<IActionCollection> actions;
        Com<IAction> action;
        Com<IExecAction> exec;
        VARIANT_BOOL enabled = VARIANT_FALSE;
        if (SUCCEEDED(task->get_Enabled(&enabled)) && enabled == VARIANT_TRUE &&
            SUCCEEDED(task->get_Definition(definition.Put())) && SUCCEEDED(definition->get_Actions(actions.Put())) &&
            SUCCEEDED(actions->get_Item(1, action.Put())) &&
            SUCCEEDED(action->QueryInterface(IID_IExecAction, reinterpret_cast<void**>(exec.Put())))) {
            BSTR executable = nullptr;
            BSTR arguments = nullptr;
            taskMatches = SUCCEEDED(exec->get_Path(&executable)) && SUCCEEDED(exec->get_Arguments(&arguments)) &&
                          executable && arguments && _wcsicmp(executable, target.executable.c_str()) == 0 &&
                          target.arguments == arguments;
            SysFreeString(executable);
            SysFreeString(arguments);
        }
    }
    return ClassifyAutostart(hasRun, runMatches, hasTask, taskMatches);
}
}  // namespace ce::startup
