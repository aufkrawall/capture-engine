// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall
#include "injection_internal.h"
#include "common/platform/startup_launch_control.h"
#include "common/platform/ansi_path.h"
#include "common/ipc/elevation_windows.h"

bool InjectionManager::AttachStartupLaunchHost(DWORD pid, const std::string& name) {
    if (!ce::startup_launch::IsLaunchHost(name)) return false;
    {
        std::lock_guard<std::mutex> lock(configMutex);
        if (config.gameWhitelist.empty() && config.overlayWhitelist.empty()) return true;
        for (const auto& entry : config.gameWhitelist) if (MatchesProcessName(entry, name)) return false;
        for (const auto& entry : config.overlayWhitelist) if (MatchesProcessName(entry, name)) return false;
    }
    std::lock_guard<std::mutex> lock(injectMutex);
    if (IsShuttingDown()) return true;
    if (!ce::startup_launch::WaitForCreator(pid)) {
        LogError("[StartupImport] Launcher creator transaction unavailable: %s PID=%lu", name.c_str(), pid);
        return true;
    }
    ce::HandleGuard requested(OpenEventW(SYNCHRONIZE, FALSE,
        ce::startup_launch::ObjectName(ce::startup_launch::Object::ResumeRequested, pid).c_str()));
    if (requested && WaitForSingleObject(requested.get(), 0) != WAIT_OBJECT_0) {
        // The creator has not finished Win32/CSRSS setup yet. The first resume
        // publishes readiness; remote LoadLibrary must not run ahead of that.
        pendingStartupLaunchHosts[pid] = name;
        return true;
    }
    DWORD session = 0;
    DWORD ownSession = 0;
    ce::HandleGuard peer(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
    const auto owner = ce::elevation::ProcessUserSid(GetCurrentProcess());
    if (!peer || owner.empty() || ce::elevation::ProcessUserSid(peer.get()) != owner ||
        !ProcessIdToSessionId(pid, &session) || !ProcessIdToSessionId(GetCurrentProcessId(), &ownSession) ||
        session != ownSession) return true;
    if (!ce::startup_launch::CanHostCreationHook(peer.get())) return true;
    // An elevated inject host must allow its same-user medium-integrity
    // launcher to signal readiness and consume explicit activation requests.
    ce::elevation::Security security(L"D:P(A;;GA;;;SY)(A;;GA;;;" + owner + L")S:(ML;;NW;;;ME)");
    if (!security.Get()) return true;
    ce::HandleGuard role(CreateEventW(security.Get(), TRUE, FALSE,
        ce::startup_launch::ObjectName(ce::startup_launch::Object::Role, pid).c_str()));
    ce::HandleGuard ready(CreateEventW(security.Get(), TRUE, FALSE,
        ce::startup_launch::ObjectName(ce::startup_launch::Object::Ready, pid).c_str()));
    ce::HandleGuard active(CreateEventW(security.Get(), TRUE, FALSE,
        ce::startup_launch::ObjectName(ce::startup_launch::Object::Active, pid).c_str()));
    if (!role || !ready || !active) {
        LogError("[StartupImport] Cannot create launcher control objects for %s PID=%lu error=%lu",
                 name.c_str(), pid, GetLastError());
        return true;
    }
    if (!IsAlreadyInjectedLocked(pid)) {
        if (!InjectImpl(pid, name, true)) {
            LogError("[StartupImport] Creation-only attachment failed for %s PID=%lu", name.c_str(), pid);
            return true;
        }
    }
    auto target = std::find_if(injectedProcesses.begin(), injectedProcesses.end(),
                               [pid](const InjectedProcess& process) { return process.pid == pid; });
    if (target == injectedProcesses.end()) return true;
    target->creationOnly = true;
    if (!target->startupRole) target->startupRole = role.release();
    if (!target->startupActive) target->startupActive = active.release();
    if (WaitForSingleObject(ready.get(), 0) == WAIT_OBJECT_0) {
        SetEvent(target->startupActive);
        return true;
    }
    SetEvent(target->startupActive);
    SetEvent(target->startupRole);
    const DWORD result = WaitForSingleObject(ready.get(), 5000);
    if (result == WAIT_OBJECT_0)
        LogInfo("[StartupImport] Creation-only launcher ready: %s PID=%lu", name.c_str(), pid);
    else {
        ResetEvent(target->startupActive);
        LogError("[StartupImport] Launcher interception unavailable: %s PID=%lu wait=%lu", name.c_str(), pid, result);
    }
    return true;
}

void InjectionManager::ServiceStartupLaunchHosts() {
    std::vector<std::pair<DWORD, std::string>> ready;
    {
        std::lock_guard<std::mutex> lock(injectMutex);
        for (auto it = pendingStartupLaunchHosts.begin(); it != pendingStartupLaunchHosts.end();) {
            ce::HandleGuard process(OpenProcess(SYNCHRONIZE, FALSE, it->first));
            ce::HandleGuard requested(OpenEventW(SYNCHRONIZE, FALSE,
                ce::startup_launch::ObjectName(ce::startup_launch::Object::ResumeRequested, it->first).c_str()));
            if (!process || WaitForSingleObject(process.get(), 0) == WAIT_OBJECT_0) {
                it = pendingStartupLaunchHosts.erase(it);
            } else if (requested && WaitForSingleObject(requested.get(), 0) == WAIT_OBJECT_0) {
                ready.push_back(*it);
                it = pendingStartupLaunchHosts.erase(it);
            } else ++it;
        }
    }
    for (const auto& [pid, name] : ready) AttachStartupLaunchHost(pid, name);
}

void InjectionManager::ScanStartupLaunchHosts() {
    ce::HandleGuard snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (!snapshot) return;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot.get(), &entry)) {
        do {
            std::string name;
            if (ce::ansi_path::TryNarrowAcpExactly(entry.szExeFile, &name))
                AttachStartupLaunchHost(entry.th32ProcessID, name);
        } while (Process32NextW(snapshot.get(), &entry));
    }
}
