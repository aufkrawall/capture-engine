// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall
#include "startup_launch_control.h"

#include <tlhelp32.h>
#include <array>
#include <cwchar>
#include <algorithm>
#include <cctype>

namespace ce::startup_launch {

std::wstring ObjectName(Object object, DWORD pid) {
    const wchar_t* tag = object == Object::Role ? L"Role" : object == Object::Ready ? L"Ready" :
        object == Object::Active ? L"Active" : object == Object::ResumeRequested ? L"ResumeRequested" : L"Gate";
    wchar_t name[80]{};
    swprintf(name, _countof(name), L"Local\\CE_Startup_%s_%08lX", tag, static_cast<unsigned long>(pid));
    return name;
}

bool IsLaunchHost(const std::string& name) {
    // Creation-only coverage of desktop launches, command shells and storefront
    // clients. These processes never become capture/overlay targets through this role.
    constexpr std::array names = {"explorer.exe", "cmd.exe", "powershell.exe", "pwsh.exe", "steam.exe",
                                  "epicgameslauncher.exe", "galaxyclient.exe", "eadesktop.exe",
                                  "origin.exe", "upc.exe", "uplay.exe"};
    for (const char* candidate : names) if (_stricmp(name.c_str(), candidate) == 0) return true;
    std::string normalized = name;
    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                    [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return normalized.ends_with(".exe") && normalized.find("launcher") != std::string::npos;
}

bool CanHostCreationHook(HANDLE process) {
    PROCESS_MITIGATION_SYSTEM_CALL_DISABLE_POLICY win32k{};
    PROCESS_MITIGATION_DYNAMIC_CODE_POLICY dynamicCode{};
    PROCESS_MITIGATION_BINARY_SIGNATURE_POLICY signature{};
    if (!GetProcessMitigationPolicy(process, ProcessSystemCallDisablePolicy, &win32k, sizeof(win32k)) ||
        !GetProcessMitigationPolicy(process, ProcessDynamicCodePolicy, &dynamicCode, sizeof(dynamicCode)) ||
        !GetProcessMitigationPolicy(process, ProcessSignaturePolicy, &signature, sizeof(signature)) ||
        win32k.DisallowWin32kSystemCalls || dynamicCode.ProhibitDynamicCode ||
        signature.MicrosoftSignedOnly || signature.StoreSignedOnly) return false;
    HandleGuard token;
    DWORD container = 0;
    DWORD bytes = 0;
    return OpenProcessToken(process, TOKEN_QUERY, token.addressof()) &&
        GetTokenInformation(token.get(), TokenIsAppContainer, &container, sizeof(container), &bytes) && !container;
}

bool HasRole(DWORD pid) {
    HandleGuard role(OpenEventW(SYNCHRONIZE, FALSE, ObjectName(Object::Role, pid).c_str()));
    return static_cast<bool>(role);
}

Gate::Gate(HANDLE mutex, DWORD timeout) : mutex_(mutex), acquired_(false) {
    if (mutex_ && mutex_ != INVALID_HANDLE_VALUE) {
        const DWORD result = WaitForSingleObject(mutex_, timeout);
        acquired_ = result == WAIT_OBJECT_0 || result == WAIT_ABANDONED;
    }
}

void Gate::Release() {
    if (acquired_) { ReleaseMutex(mutex_); acquired_ = false; }
}

Gate::~Gate() { Release(); }

bool WaitForCreator(DWORD childPid, DWORD timeout) {
    HandleGuard snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (!snapshot) return false;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    DWORD parent = 0;
    if (Process32FirstW(snapshot.get(), &entry)) {
        do {
            if (entry.th32ProcessID == childPid) {
                parent = entry.th32ParentProcessID;
                break;
            }
        } while (Process32NextW(snapshot.get(), &entry));
    }
    if (!parent) return true;
    HandleGuard ready(OpenEventW(SYNCHRONIZE, FALSE, ObjectName(Object::Ready, parent).c_str()));
    if (!ready && !HasRole(parent)) return true;
    if (!ready || WaitForSingleObject(ready.get(), timeout) != WAIT_OBJECT_0) return false;
    HandleGuard mutex(OpenMutexW(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE, ObjectName(Object::Gate, parent).c_str()));
    Gate gate(mutex.get(), timeout);
    return gate.Acquired();
}

}  // namespace ce::startup_launch
