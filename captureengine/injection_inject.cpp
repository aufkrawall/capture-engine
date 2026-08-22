#include "injection_internal.h"

bool InjectionManager::Inject(DWORD pid, const std::string& processName) {
    // Execute callback if set (e.g. to reload config for this specific process)
    if (onInjectCallback) {
        LogInfo("[Inject] Executing pre-injection callback for %s", processName.c_str());
        onInjectCallback(processName);
    }

    // Determine architecture - use RAII HandleGuard to prevent leaks
    ce::HandleGuard hProcess(OpenProcess(
        PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_VM_OPERATION | PROCESS_CREATE_THREAD |
            SYNCHRONIZE,
        FALSE, pid));
    if (!hProcess) {
        LogError("Failed to open process %lu for injection", (unsigned long)pid);
        return false;
    }

    BOOL isWow64 = FALSE;
    IsWow64Process(hProcess.get(), &isWow64);

    std::string dllPath = isWow64 ? hookDllPathX86 : hookDllPathX64;

    // SECURITY: Contain the DLL to the application directory and reject broadly
    // writable install locations before any remote load. Production builds fail
    // closed; development builds log and continue so local trees stay usable.
    if (!ValidateDllSecurity(dllPath)) {
        LogError("[SECURITY] DLL security validation failed for %s - refusing to inject", dllPath.c_str());
        return false;
    }

    // SECURITY: Verify DLL integrity before injection
    // This prevents injection of tampered DLLs and protects against DLL hijacking
    //
    // CE_PRODUCTION_BUILD is defined only for signed production releases.
    // Dev/CI builds do NOT define it and fall through to the warning path below.
    // To create a production build, pass --production to build.py.
#ifdef CE_PRODUCTION_BUILD
    // PRODUCTION BUILD: Require valid Authenticode signature
    if (!VerifyDLLSignature(dllPath, true)) {
        LogError(
            "[SECURITY] DLL signature verification failed for %s - refusing "
            "to inject",
            dllPath.c_str());
        LogError(
            "[SECURITY] In production builds, only properly signed DLLs can "
            "be injected");
        return false;
    }
    LogInfo("[SECURITY] DLL signature verified: %s", dllPath.c_str());
#else
    // DEVELOPMENT BUILD: Warn about unsigned DLL but allow injection.
    // SKIP_DLL_VERIFICATION=1 bypasses signature checks for local dev.
    // This env var is never read in production builds (CE_PRODUCTION_BUILD).
    const char* skipVerification = getenv("SKIP_DLL_VERIFICATION");
    if (skipVerification && strcmp(skipVerification, "1") == 0) {
        LogWarn("[SECURITY] Skipping DLL verification (SKIP_DLL_VERIFICATION=1)");
    } else if (!VerifyDLLSignature(dllPath, false)) {
        LogWarn("[SECURITY] DLL is not Authenticode-signed: %s", dllPath.c_str());
        LogWarn(
            "[SECURITY] This is expected for development builds. Set "
            "CE_PRODUCTION_BUILD to enforce signing.");
    } else {
        LogInfo("[SECURITY] DLL signature verified: %s", dllPath.c_str());
    }
#endif

    LogInfo("Using DLL: %s (WoW64: %d)", dllPath.c_str(), isWow64);

    if (!fs::exists(dllPath)) {
        LogError("Required DLL for %s injection missing: %s", isWow64 ? "x86" : "x64", dllPath.c_str());
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
        // Note: We assume target is 32-bit here because this logic is inside 'if
        // (isWow64)' But we should be careful. We can read NT Headers signature
        // first? Let's just read the signature + file header + optional header
        // structure. Since we know we are in isWow64 block, we expect 32-bit
        // headers.

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
        if (!ReadProcessMemory(hProc, (BYTE*)hModule + exportDir.AddressOfNames, nameRVAs.data(),
                               nameRVAs.size() * sizeof(DWORD), NULL))
            return nullptr;

        // Search for function name
        for (DWORD i = 0; i < exportDir.NumberOfNames; i++) {
            char buffer[256];
            if (ReadProcessMemory(hProc, (BYTE*)hModule + nameRVAs[i], buffer, sizeof(buffer), NULL)) {
                buffer[255] = '\0';  // Ensure null term
                if (strcmp(buffer, funcName) == 0) {
                    // Found name, get ordinal
                    WORD ordinal;
                    if (!ReadProcessMemory(hProc, (BYTE*)hModule + exportDir.AddressOfNameOrdinals + (i * sizeof(WORD)),
                                           &ordinal, sizeof(WORD), NULL))
                        return nullptr;

                    // Get Function RVA
                    DWORD funcRVA;
                    if (!ReadProcessMemory(hProc,
                                           (BYTE*)hModule + exportDir.AddressOfFunctions + (ordinal * sizeof(DWORD)),
                                           &funcRVA, sizeof(DWORD), NULL))
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
        pLoadLibrary = (LPVOID)GetProcAddress(GetModuleHandleA("kernel32.dll"), "LoadLibraryA");
    } else {
        // 32-bit target (WoW64)
        // We must wait for kernel32.dll to be loaded. It might take a moment during
        // startup.
        int maxRetries = 20;  // 2 seconds (20 * 100ms)

        for (int retry = 0; retry < maxRetries; retry++) {
            std::vector<HMODULE> hMods;
            if (ce::EnumerateProcessModulesEx(hProcess.get(), LIST_MODULES_32BIT, hMods)) {
                for (size_t i = 0; i < hMods.size(); i++) {
                    char szModName[MAX_PATH];
                    if (GetModuleFileNameExA(hProcess.get(), hMods[i], szModName, sizeof(szModName))) {
                        std::string modName = szModName;
                        // Case insensitive check
                        std::transform(modName.begin(), modName.end(), modName.begin(),
                                       [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });

                        if (modName.find("kernel32.dll") != std::string::npos) {
                            // Found kernel32!
                            pLoadLibrary = GetRemoteProcAddress(hProcess.get(), hMods[i], "LoadLibraryA");

                            if (pLoadLibrary)
                                LogInfo("Resolved LoadLibraryA in x86 process at 0x%p (Base: 0x%p)", pLoadLibrary,
                                        hMods[i]);
                            else
                                LogError(
                                    "Failed to resolve LoadLibraryA in x86 process via PE "
                                    "parsing");
                            goto found_kernel32;
                        }
                    }
                }
            }
            Sleep(100);  // Wait for WoW64 init
        }
        LogError("Timeout waiting for kernel32.dll in WoW64 process");

    found_kernel32:;
    }

    if (!pLoadLibrary) {
        LogError("Failed to resolve LoadLibrary for PID %lu", pid);
        return false;
    }

    // Allocate memory in remote process - use RAII VirtualAllocGuard
    ce::VirtualAllocGuard pRemotePath(
        hProcess.get(), VirtualAllocEx(hProcess.get(), NULL, dllPath.size() + 1, MEM_COMMIT, PAGE_READWRITE));
    if (!pRemotePath) {
        LogError("VirtualAllocEx failed for PID %lu", pid);
        return false;
    }

    if (!WriteProcessMemory(hProcess.get(), pRemotePath.get(), dllPath.c_str(), dllPath.size() + 1, NULL)) {
        LogError("WriteProcessMemory failed for PID %lu", pid);
        return false;
    }

    // Publish both target wakeups before LoadLibrary can start HookThread. A
    // signaled manual-reset event survives until the newly loaded hook/layer
    // consumes it, closing the narrow race where its first IPC mapping attempt
    // runs before the host has finished publishing all session objects.
    ce::HandleGuard reactivateEvent;
    ce::HandleGuard vulkanReactivateEvent;
    CreateTargetReactivationEvents(pid, true, true, reactivateEvent.addressof(),
                                   vulkanReactivateEvent.addressof());

    ce::HandleGuard hThread(
        CreateRemoteThread(hProcess.get(), NULL, 0, (LPTHREAD_START_ROUTINE)pLoadLibrary, pRemotePath.get(), 0, NULL));
    if (!hThread) {
        LogError("CreateRemoteThread failed for PID %lu", pid);
        return false;
    }

    const DWORD loadWait = WaitForSingleObject(hThread.get(), 5000);
    if (loadWait != WAIT_OBJECT_0) {
        InjectedProcess pending;
        pending.pid = pid;
        pending.name = processName;
        pending.hProcess = hProcess.release();
        pending.remoteMemory = pRemotePath.release();
        pending.injectionThread = hThread.release();
        pending.reactivateEvent = reactivateEvent.release();
        pending.vulkanReactivateEvent = vulkanReactivateEvent.release();
        injectedProcesses.push_back(pending);
        LogWarn(
            "Remote LoadLibrary is still pending for PID %lu (wait=%lu error=%lu); retaining its path buffer until "
            "completion or process exit",
            static_cast<unsigned long>(pid), static_cast<unsigned long>(loadWait), GetLastError());
        return true;
    }

    DWORD exitCode = 0;
    if (GetExitCodeThread(hThread.get(), &exitCode)) {
        if (exitCode == 0) {
            // LoadLibraryA returned NULL
            LogError(
                "LoadLibraryA failed in remote process (Exit Code: 0). DLL "
                "failed to load.");
            return false;
        } else {
            LogInfo("LoadLibraryA succeeded (Remote Handle: 0x%lX)", (unsigned long)exitCode);
        }
    } else {
        LogError("Failed to get thread exit code.");
    }

    // Verify DLL is actually loaded in remote process
    {
        DWORD filterFlag = isWow64 ? LIST_MODULES_32BIT : LIST_MODULES_64BIT;
        bool dllFound = false;
        std::vector<HMODULE> hMods;
        if (ce::EnumerateProcessModulesEx(hProcess.get(), filterFlag, hMods)) {
            for (size_t i = 0; i < hMods.size(); i++) {
                char szModName[MAX_PATH];
                if (GetModuleFileNameExA(hProcess.get(), hMods[i], szModName, sizeof(szModName))) {
                    if (strstr(szModName, "capture_hook_x64.dll") || strstr(szModName, "capture_hook_x86.dll")) {
                        dllFound = true;
                        break;
                    }
                }
            }
        }
        if (!dllFound) {
            LogWarn(
                "DLL injection verification failed - hook DLL not found in "
                "module list for PID %lu",
                (unsigned long)pid);
        }
    }

    // Success - track the injected process
    // Note: We need to keep a handle to monitor the process, so release from RAII
    InjectedProcess ip;
    ip.pid = pid;
    ip.name = processName;
    ip.hProcess = hProcess.release();  // Transfer ownership
    ip.remoteMemory = nullptr;         // No remote memory for CreateRemoteThread injection (freed by RAII)
    ip.injectionThread = nullptr;
    ip.reactivateEvent = reactivateEvent.release();
    ip.vulkanReactivateEvent = vulkanReactivateEvent.release();
    injectedProcesses.push_back(ip);

    LogInfo("Injected %s into %s (PID: %d)", isWow64 ? "x86" : "x64", processName.c_str(), pid);
    return true;
}

bool InjectionManager::InjectEarly(DWORD pid, const std::string& dllPath, HANDLE hMainThread) {
    LogInfo("[APC] Attempting early APC injection for PID %lu", pid);

    HANDLE hProcess = OpenProcess(
        PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_VM_OPERATION | PROCESS_CREATE_THREAD |
            SYNCHRONIZE,
        FALSE, pid);
    if (!hProcess) {
        LogError("[APC] OpenProcess failed for PID %d: %d", pid, GetLastError());
        return false;
    }

    BOOL isWow64Target = FALSE;
    IsWow64Process(hProcess, &isWow64Target);

    // SECURITY: Mirror Inject()'s integrity gates. Early APC injection runs
    // before import resolution and previously skipped every check, so a swapped
    // hook DLL would execute inside the game without even a warning log.
    // Production builds fail closed; development builds log advisories and
    // honor SKIP_DLL_VERIFICATION=1 like Inject().
    if (!ValidateDllSecurity(dllPath)) {
        LogError("[SECURITY] DLL security validation failed for %s - refusing early APC injection", dllPath.c_str());
        CloseHandle(hProcess);
        return false;
    }
#ifdef CE_PRODUCTION_BUILD
    if (!VerifyDLLSignature(dllPath, true)) {
        LogError("[SECURITY] DLL signature verification failed for %s - refusing early APC injection",
                 dllPath.c_str());
        CloseHandle(hProcess);
        return false;
    }
#else
    const char* skipVerification = getenv("SKIP_DLL_VERIFICATION");
    if (skipVerification && strcmp(skipVerification, "1") == 0) {
        LogWarn("[SECURITY] Skipping DLL verification (SKIP_DLL_VERIFICATION=1)");
    } else if (!VerifyDLLSignature(dllPath, false)) {
        LogWarn("[SECURITY] Early APC target DLL is not Authenticode-signed: %s", dllPath.c_str());
    }
#endif

    LPVOID pLoadLibraryA = nullptr;
    if (isWow64Target) {
        pLoadLibraryA = GetRemoteModuleProcAddress(hProcess, L"kernel32.dll", "LoadLibraryA");
        if (!pLoadLibraryA) {
            LogError("[APC] Failed to resolve LoadLibraryA in WoW64 process");
            CloseHandle(hProcess);
            return false;
        }
    } else {
        HMODULE hKernel32 = GetModuleHandleA("kernel32.dll");
        pLoadLibraryA = (LPVOID)GetProcAddress(hKernel32, "LoadLibraryA");
        if (!pLoadLibraryA) {
            LogError("[APC] Failed to get LoadLibraryA address");
            CloseHandle(hProcess);
            return false;
        }
    }

    SIZE_T pathSize = dllPath.size() + 1;
    LPVOID pRemotePath = VirtualAllocEx(hProcess, NULL, pathSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!pRemotePath) {
        LogError("[APC] VirtualAllocEx failed: %lu", GetLastError());
        CloseHandle(hProcess);
        return false;
    }

    if (!WriteProcessMemory(hProcess, pRemotePath, dllPath.c_str(), pathSize, NULL)) {
        LogError("[APC] WriteProcessMemory failed: %lu", GetLastError());
        VirtualFreeEx(hProcess, pRemotePath, 0, MEM_RELEASE);
        CloseHandle(hProcess);
        return false;
    }

    ce::HandleGuard reactivateEvent;
    ce::HandleGuard vulkanReactivateEvent;
    CreateTargetReactivationEvents(pid, true, true, reactivateEvent.addressof(),
                                   vulkanReactivateEvent.addressof());

    DWORD result = QueueUserAPC((PAPCFUNC)pLoadLibraryA, hMainThread, (ULONG_PTR)pRemotePath);
    if (!result) {
        LogError("[APC] QueueUserAPC failed: %lu", GetLastError());
        VirtualFreeEx(hProcess, pRemotePath, 0, MEM_RELEASE);
        CloseHandle(hProcess);
        return false;
    }

    LogInfo(
        "[APC] APC queued successfully for PID %d - DLL will load before "
        "import resolution",
        pid);

    InjectedProcess ip;
    ip.pid = pid;
    ip.name = dllPath;
    ip.hProcess = hProcess;
    ip.remoteMemory = pRemotePath;  // Store for later cleanup
    ip.injectionThread = nullptr;
    ip.reactivateEvent = reactivateEvent.release();
    ip.vulkanReactivateEvent = vulkanReactivateEvent.release();
    injectedProcesses.push_back(ip);

    return true;
}

void InjectionManager::EjectAll() {
    std::vector<DWORD> processIds;
    {
        std::lock_guard<std::mutex> lock(injectMutex);
        processIds.reserve(injectedProcesses.size());
        for (const auto& process : injectedProcesses)
            processIds.push_back(process.pid);
    }

    // Host shutdown is broadcast, so all targets deactivate concurrently. Use
    // one bounded acknowledgement deadline for the whole set rather than
    // multiplying shutdown latency by the number of injected processes.
    const ULONGLONG deadline = GetTickCount64() + 5000;
    for (DWORD pid : processIds)
        EjectWithDeadline(pid, deadline);

    std::lock_guard<std::mutex> lock(injectMutex);
    injectedProcesses.clear();
}

// CRITICAL FIX: Wait for all delayed injection threads with timeout
void InjectionManager::WaitForInjectionThreads(int timeoutMs) {
    std::list<std::thread> threadsToJoin;

    {
        std::lock_guard<std::mutex> lock(threadListMutex);
        threadsToJoin = std::move(delayedInjectionThreads);
        delayedInjectionThreads.clear();
    }

    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    for (auto& t : threadsToJoin) {
        if (!t.joinable()) {
            continue;
        }

        auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            LogWarn(
                "[Injection] Delayed injection thread exceeded the shutdown deadline; waiting for its bounded "
                "remote operation to finish so InjectionManager storage remains valid");
            t.join();
            continue;
        }

        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
        DWORD waitMs = static_cast<DWORD>(std::max<int64_t>(remaining, 1));
        HANDLE threadHandle = ce::Win32ThreadHandle(t);
        DWORD waitResult = WaitForSingleObject(threadHandle, waitMs);

        if (waitResult == WAIT_OBJECT_0) {
            t.join();
        } else {
            if (waitResult == WAIT_TIMEOUT) {
                LogWarn(
                    "[Injection] Delayed injection thread exceeded the shutdown deadline; waiting for its bounded "
                    "remote operation to finish so InjectionManager storage remains valid");
            } else {
                LogWarn("[Injection] WaitForSingleObject failed for delayed injection thread (error=%lu); joining "
                        "to preserve manager lifetime",
                        GetLastError());
            }
            t.join();
        }
    }
    LogInfo("[Injection] All delayed injection threads cleaned up");

}

// Check if any process is currently injected
bool InjectionManager::HasActiveInjections() const {
    std::lock_guard<std::mutex> lock(injectMutex);
    return !injectedProcesses.empty();
}

bool InjectionManager::HasPendingInjections() {
    std::lock_guard<std::mutex> lock(injectMutex);
    std::lock_guard<std::mutex> threadLock(threadListMutex);
    return !pendingInjections.empty() || !delayedInjectionThreads.empty();
}

void InjectionManager::Eject(DWORD pid) {
    EjectWithDeadline(pid, GetTickCount64() + 5000);
}

void InjectionManager::EjectWithDeadline(DWORD pid, ULONGLONG deadline) {
    std::lock_guard<std::mutex> lock(injectMutex);
    auto it = std::find_if(injectedProcesses.begin(), injectedProcesses.end(),
                           [&](const InjectedProcess& p) { return p.pid == pid; });
    HANDLE hProcess = (it != injectedProcesses.end()) ? it->hProcess : NULL;

    if (!hProcess) {
        hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_OPERATION | SYNCHRONIZE, FALSE, pid);
        if (!hProcess) {
            const DWORD error = GetLastError();
            if (it != injectedProcesses.end()) {
                if (it->injectionThread) {
                    CloseHandle(it->injectionThread);
                    it->injectionThread = nullptr;
                }
                CloseTargetReactivationEvents(&it->reactivateEvent, &it->vulkanReactivateEvent);
                if (it->hProcess) {
                    CloseHandle(it->hProcess);
                    it->hProcess = nullptr;
                }
            }
            LogWarn("[InjectLifecycle] Could not open PID %lu for dormant acknowledgement (error=%lu)",
                    static_cast<unsigned long>(pid), static_cast<unsigned long>(error));
            if (it != injectedProcesses.end())
                injectedProcesses.erase(it);
            return;
        }
    }

    const auto waitForDormantEvent = [&](const wchar_t* eventName, const char* owner) {
        HANDLE event = OpenEventW(SYNCHRONIZE, FALSE, eventName);
        if (!event)
            return false;
        const ULONGLONG now = GetTickCount64();
        const DWORD timeout = now < deadline ? static_cast<DWORD>(deadline - now) : 0;
        HANDLE waits[] = {event, hProcess};
        const DWORD wait = WaitForMultipleObjects(_countof(waits), waits, FALSE, timeout);
        if (wait == WAIT_OBJECT_0) {
            LogInfo("[InjectLifecycle] %s acknowledged dormant state for PID %lu", owner,
                    static_cast<unsigned long>(pid));
            CloseHandle(event);
            return true;
        } else if (wait != WAIT_OBJECT_0 + 1) {
            LogWarn("[InjectLifecycle] Timed out waiting for %s dormant acknowledgement for PID %lu (wait=%lu)",
                    owner, static_cast<unsigned long>(pid), static_cast<unsigned long>(wait));
        }
        CloseHandle(event);
        return false;
    };

    wchar_t injectDormantEventName[64] = {};
    wchar_t vulkanDormantEventName[64] = {};
    GenerateInjectDormantEventName(injectDormantEventName, _countof(injectDormantEventName), pid);
    GenerateVulkanDormantEventName(vulkanDormantEventName, _countof(vulkanDormantEventName), pid);
    const bool hookDormant = waitForDormantEvent(injectDormantEventName, "capture hook");
    waitForDormantEvent(vulkanDormantEventName, "Vulkan layer");

    // APC injection retains its path buffer until the queued LoadLibrary has consumed it.
    if (it != injectedProcesses.end() && it->remoteMemory) {
        if (hookDormant) {
            void* remoteMemory = it->remoteMemory;
            VirtualFreeEx(hProcess, remoteMemory, 0, MEM_RELEASE);
            it->remoteMemory = nullptr;
            LogInfo("[InjectLifecycle] Freed consumed injection path memory at %p for PID %lu", remoteMemory,
                    static_cast<unsigned long>(pid));
        } else {
            LogWarn(
                "[InjectLifecycle] Retaining injection path memory for PID %lu because remote LoadLibrary "
                "consumption was not acknowledged",
                static_cast<unsigned long>(pid));
        }
    }

    if (it != injectedProcesses.end() && it->injectionThread) {
        CloseHandle(it->injectionThread);
        it->injectionThread = nullptr;
    }
    if (it != injectedProcesses.end()) {
        CloseTargetReactivationEvents(&it->reactivateEvent, &it->vulkanReactivateEvent);
    }

    LogInfo(
        "[InjectLifecycle] PID %lu is pass-through; capture_hook remains resident to preserve game wrappers and "
        "foreign hook chains",
        static_cast<unsigned long>(pid));

    CloseHandle(hProcess);
    if (it != injectedProcesses.end())
        injectedProcesses.erase(it);
}
