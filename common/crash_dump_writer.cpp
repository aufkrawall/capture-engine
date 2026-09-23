// Minidump writing and the exception filters that drive it: the dump worker
// thread, the vectored/unhandled exception filters, and crash-handler
// installation. Shared state and helpers come from crash_handler_internal.h.

#include "crash_handler_internal.h"
#include "cpp_exception_message.h"
#include "crash_first_chance.h"

#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>

#include "crash_dump_policy.h"
#include "secure_dll_loading.h"

// Everything MiniDumpWriteDump needs, owned independently of the crashing thread.
//
// This used to hold the raw EXCEPTION_POINTERS the filter was handed. That structure and the
// EXCEPTION_RECORD and CONTEXT it points at live on the crashing thread's stack, and the
// filter only waits 5 s for the worker before returning EXCEPTION_CONTINUE_SEARCH. Five
// seconds is not a generous bound for this workload - the comment above the external-helper
// branch below records a 61.6 s in-process MiniDumpNormal with the Steam overlay loaded - so
// the timeout is a normal outcome. If an SEH frame then handles the exception and execution
// continues, that stack is reused while the worker is still reading it, and the dump is
// written from freed memory.
//
// Deep copies remove the coupling: both structures are fixed-size PODs. The instance is a
// single static slot rather than a heap allocation because `new` inside a crash filter can
// deadlock when the crash happened while the heap lock was held. Ownership of the slot is the
// g_DumpAttemptInProgress compare-exchange that already gates this path.
struct DumpParams {
    EXCEPTION_RECORD record{};
    CONTEXT context{};
    EXCEPTION_POINTERS pointers{};
    DWORD threadId = 0;

    void CaptureFrom(EXCEPTION_POINTERS* source, DWORD crashingThreadId) {
        if (source && source->ExceptionRecord)
            record = *source->ExceptionRecord;
        if (source && source->ContextRecord)
            context = *source->ContextRecord;
        // The copies are self-referential; the nested ExceptionRecord chain is deliberately
        // not followed, so clear it rather than leave a pointer into the crashed stack.
        record.ExceptionRecord = nullptr;
        pointers.ExceptionRecord = &record;
        pointers.ContextRecord = &context;
        threadId = crashingThreadId;
    }
};

static DumpParams g_DumpParamsSlot;

// Worker thread to write minidump safely away from the crashed stack
DWORD WINAPI DumpWorker(LPVOID lpParam) {
    DumpParams* params = static_cast<DumpParams*>(lpParam);

    // The worker, not the filter, owns g_DumpAttemptInProgress from here on. The filter used
    // to clear it as soon as its 5 s wait expired, which let a second crash start a second
    // worker while the first was still writing - two workers sharing the dump slot and the
    // dump directory. Releasing it here instead means the flag stays set exactly as long as a
    // worker is actually running, whether or not the filter is still waiting for it.
    struct AttemptReleaser {
        ~AttemptReleaser() { g_DumpAttemptInProgress.store(false, std::memory_order_release); }
    } attemptReleaser;

    ActivateCrashTrace();
    TraceCrash("DumpWorker started");

    // Read dump directory under mutex
    std::string dumpDir;
    {
        ExceptionSafeLock dirLock(g_DumpDirMutex, g_DumpDirMutexOwnerThread);
        dumpDir = CrashDumpDirectoryStorage();
    }

    SYSTEMTIME st;
    GetLocalTime(&st);
    char buf[128];
    snprintf(buf, sizeof(buf), "%04u%02u%02u_%02u%02u%02u_%03u_pid%lu_tid%lu", st.wYear, st.wMonth, st.wDay, st.wHour,
             st.wMinute, st.wSecond, st.wMilliseconds, GetCurrentProcessId(), params->threadId);

    char dumpFileName[MAX_PATH];
    snprintf(dumpFileName, sizeof(dumpFileName), "crash_%s.dmp", buf);

    char dumpPath[MAX_PATH];
    snprintf(dumpPath, sizeof(dumpPath), "%s\\%s", dumpDir.c_str(), dumpFileName);

    const std::string tempDumpFileName = ce::crash_dump_policy::BuildInProgressDumpFileName(dumpFileName);
    char tempDumpPath[MAX_PATH];
    snprintf(tempDumpPath, sizeof(tempDumpPath), "%s\\%s", dumpDir.c_str(), tempDumpFileName.c_str());

    // An in-process dump makes dbghelp read every loaded module's version
    // resource with all other threads suspended. Through a foreign overlay's
    // loader/version hooks that walk takes minutes, which the user experiences
    // as the game freezing (session 20260817_052857: 61.6 s per MiniDumpNormal
    // with the Steam overlay loaded). The external helper writes the same dump
    // from outside without suspending anything.
    const bool foreignOverlayLoaded = IsForeignOverlayLoadedForCrashDump();
    if (ce::crash_dump_policy::ShouldPreferExternalCrashDumpHelper(foreignOverlayLoaded,
                                                                   HasExternalCrashDumpCapture())) {
        TraceCrash("Foreign overlay loaded - capturing crash dump with the external helper first");
        ExternalDumpException exception;
        exception.pointers = &params->pointers;
        exception.threadId = params->threadId;
        if (CaptureCrashDumpWithExternalHelper(dumpFileName, false, &exception)) {
            TraceCrash("External helper captured the crash dump");
            g_DumpSuccessfullyWritten.store(true, std::memory_order_release);
            return 0;
        }
        TraceCrash("External helper crash dump failed");
    }

    if (!ce::crash_dump_policy::ShouldUseInProcessMiniDumpFallbackAfterExternalHelperFailure(foreignOverlayLoaded)) {
        TraceCrash("Skipping in-process crash dump - dbghelp module enumeration can hang against foreign overlay "
                   "hooks and would suspend every thread meanwhile");
        return 0;
    }

    TraceCrash("Creating in-progress dump file...");
    TraceCrash(tempDumpPath);
    TraceCrash("Final dump path after successful write:");
    TraceCrash(dumpPath);

    // Ensure directory exists with proper error checking
    if (CreateDirectoryA(dumpDir.c_str(), NULL)) {
        TraceCrash("Created dump directory");
    } else {
        DWORD dirErr = GetLastError();
        if (dirErr == ERROR_ALREADY_EXISTS) {
            TraceCrash("Dump directory already exists");
        } else {
            char dirErrMsg[128];
            snprintf(dirErrMsg, sizeof(dirErrMsg), "Failed to create dump directory (err=%lu)", dirErr);
            TraceCrash(dirErrMsg);
        }
    }

    DeleteFileA(tempDumpPath);
    HANDLE hFile = CreateFileA(tempDumpPath, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, NULL);

    if (hFile == INVALID_HANDLE_VALUE) {
        DWORD createErr = GetLastError();
        char createErrMsg[160];
        snprintf(createErrMsg, sizeof(createErrMsg), "Failed to create dump file at configured path (err=%lu)",
                 createErr);
        TraceCrash(createErrMsg);
    }

    if (hFile != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION mdei;
        mdei.ThreadId = params->threadId;
        mdei.ExceptionPointers = &params->pointers;
        mdei.ClientPointers = FALSE;

        struct DumpAttempt {
            MINIDUMP_TYPE type;
            bool withExceptionInfo;
            const char* label;
        };

        const DumpAttempt attempts[] = {
            {ce::crash_dump_policy::kMinimalDumpType, true, "minimal-primary"},
            {ce::crash_dump_policy::kMinimalDumpType, false, "minimal-no-exception"},
            {ce::crash_dump_policy::kCompatibilityCrashDumpType, true, "compat-after-minimal"},
            {ce::crash_dump_policy::kRichCrashDumpType, true, "rich-after-minimal"},
        };
        const size_t attemptCount = sizeof(attempts) / sizeof(attempts[0]);

        TraceCrash("Calling MiniDumpWriteDump from worker thread...");
        TraceCrash("CrashHandler: using minimal-first crash dump attempts");

        BOOL rv = FALSE;
        DWORD err = ERROR_SUCCESS;
        if (g_pMiniDumpWriteDump) {
            for (size_t i = 0; i < attemptCount; ++i) {
                char attemptMsg[192];
                snprintf(attemptMsg, sizeof(attemptMsg), "MiniDumpWriteDump attempt %llu/%llu (%s)",
                         (unsigned long long)(i + 1), (unsigned long long)attemptCount, attempts[i].label);
                TraceCrash(attemptMsg);

                PMINIDUMP_EXCEPTION_INFORMATION pExc = attempts[i].withExceptionInfo ? &mdei : nullptr;
                rv = g_pMiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), hFile, attempts[i].type, pExc, 0,
                                          0);
                if (rv) {
                    break;
                }

                err = GetLastError();
                snprintf(attemptMsg, sizeof(attemptMsg), "MiniDumpWriteDump attempt failed (%s): %lu (0x%08lX)",
                         attempts[i].label, err, err);
                TraceCrash(attemptMsg);

                if (i + 1 < attemptCount) {
                    SetFilePointer(hFile, 0, NULL, FILE_BEGIN);
                    SetEndOfFile(hFile);
                }
            }
        } else {
            TraceCrash("g_pMiniDumpWriteDump is NULL in worker!");
            err = ERROR_PROC_NOT_FOUND;
        }

        bool removeTempDump = false;
        if (rv) {
            FlushFileBuffers(hFile);
            LARGE_INTEGER dumpSize = {};
            if (!GetFileSizeEx(hFile, &dumpSize) || dumpSize.QuadPart <= 0) {
                TraceCrash("MiniDumpWriteDump returned success but dump file is empty");
                rv = FALSE;
                err = ERROR_WRITE_FAULT;
                removeTempDump = true;
            } else {
                TraceCrash("MiniDumpWriteDump wrote non-empty in-progress dump");
            }
        }

        if (!rv) {
            TraceCrash("MiniDumpWriteDump Failed");
            char msg[256];
            snprintf(msg, sizeof(msg), "[CrashHandler] MiniDumpWriteDump failed: %lu (0x%08lX)\n", err, err);
            OutputDebugStringA(msg);

            LARGE_INTEGER dumpSize = {};
            if (GetFileSizeEx(hFile, &dumpSize) && dumpSize.QuadPart == 0) {
                removeTempDump = true;
            }

            char errPath[MAX_PATH];
            snprintf(errPath, sizeof(errPath), "%s\\crash_error.txt", dumpDir.c_str());
            FILE* f = fopen(errPath, "w");
            if (f) {
                fprintf(f, "MiniDumpWriteDump failed. Error: %lu (0x%08lX)\nDump Path: %s\n", err, err, dumpPath);
                fclose(f);
            }
        }
        CloseHandle(hFile);
        if (rv) {
            bool preservedTempDump = false;
            if (PromoteInProgressDumpFile(tempDumpPath, dumpPath, "DumpWorker", &preservedTempDump)) {
                TraceCrash("MiniDumpWriteDump Success");
                g_DumpSuccessfullyWritten.store(true, std::memory_order_release);
                char msg[256];
                snprintf(msg, sizeof(msg), "[CrashHandler] Minidump created at: %s\n", dumpPath);
                OutputDebugStringA(msg);
            } else {
                err = GetLastError();
                rv = FALSE;
                if (preservedTempDump) {
                    TraceCrash("MiniDumpWriteDump preserved non-empty in-progress dump after promotion failure");
                    g_DumpSuccessfullyWritten.store(true, std::memory_order_release);
                } else {
                    char renameErrMsg[160];
                    snprintf(renameErrMsg, sizeof(renameErrMsg), "Failed to promote in-progress dump file (err=%lu)",
                             err);
                    TraceCrash(renameErrMsg);
                }
            }
        } else if (removeTempDump) {
            DeleteFileA(tempDumpPath);
            TraceCrash("Deleted empty failed in-progress dump file");
        }
    } else {
        TraceCrash("Failed to create dump file");
    }

    return 0;
}

LONG WINAPI CrashHandlerExceptionFilter(EXCEPTION_POINTERS* pExceptionPointers) {
    DWORD code = pExceptionPointers->ExceptionRecord->ExceptionCode;

    // Track VEH call count for detecting runaway exception storms
    int callCount = g_VEHCallCount.fetch_add(1, std::memory_order_acq_rel);

    const LONG executionFaultResult = DispatchCrashExecutionFaultHandler(pExceptionPointers);
    if (executionFaultResult == EXCEPTION_CONTINUE_EXECUTION) {
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    // If the UnhandledExceptionFilter has asked us to force a dump, do it
    // regardless of exception code.
    const bool forceDump = g_ForceUnhandledDump.load(std::memory_order_acquire);

    // Nothing above this line may allocate, lock or write a file: it runs for
    // every exception the host raises, including the thousands a managed or
    // JIT runtime handles itself. See ClassifyFirstChanceException.
    const auto action =
        ce::crash_dump_policy::ClassifyFirstChanceException(code, forceDump, IsDebuggerPresent() != FALSE);
    switch (action) {
        case ce::crash_dump_policy::FirstChanceAction::kIgnore:
            return EXCEPTION_CONTINUE_SEARCH;
        case ce::crash_dump_policy::FirstChanceAction::kRecordFault:
            ce::crash_first_chance::RecordFault(pExceptionPointers);
            return EXCEPTION_CONTINUE_SEARCH;
        case ce::crash_dump_policy::FirstChanceAction::kQuickAssertDump:
        case ce::crash_dump_policy::FirstChanceAction::kDumpNow:
            break;
    }

    // Unhandled C++ exception: log the thrown object (message) before dumping;
    // the minimal-first minidump does not capture it.
    if (code == 0xE06D7363 || code == 0x20474343) {
        ce::crash_diagnostics::LogCppExceptionDiagnostics(pExceptionPointers->ExceptionRecord);
    }

    ActivateCrashTrace();

    if (code == EXCEPTION_BREAKPOINT && !forceDump) {
        TraceCrash("Breakpoint exception is dump-worthy because no debugger is attached");
    }
    if (forceDump) {
        const auto stats = ce::crash_first_chance::GetStatistics();
        char statsMsg[192];
        snprintf(statsMsg, sizeof(statsMsg),
                 "First-chance faults before this unhandled exception: recorded=%llu resolvedByContinue=%llu "
                 "dropped=%llu",
                 static_cast<unsigned long long>(stats.recorded),
                 static_cast<unsigned long long>(stats.resolvedByContinue),
                 static_cast<unsigned long long>(stats.dropped));
        TraceCrash(statsMsg);
    }

    // Log the exception for debugging
    char codeStr[128];
    snprintf(codeStr, sizeof(codeStr), "VEH Exception: 0x%08lX at 0x%p (PID:%lu TID:%lu, call#%d)", code,
             pExceptionPointers->ExceptionRecord->ExceptionAddress, GetCurrentProcessId(), GetCurrentThreadId(),
             callCount);
    TraceCrash(codeStr);

    // UE5 ensure() assertion (0x4000): continuable, but UE5 may call
    // TerminateProcess shortly after. Write a FAST MiniDumpNormal for
    // diagnostics (<50 ms, ~100 KB) then let UE5's handler continue.
    if (action == ce::crash_dump_policy::FirstChanceAction::kQuickAssertDump) {
        // Read the dump directory with try_lock: the crashed thread may own the mutex.
        std::string dumpDir;
        {
            std::unique_lock<std::mutex> dirLock(g_DumpDirMutex, std::try_to_lock);
            dumpDir = dirLock.owns_lock() ? CrashDumpDirectoryStorage() : std::string(".\\logs");
        }
        {
            HMODULE hMod = NULL;
            GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCSTR)pExceptionPointers->ExceptionRecord->ExceptionAddress, &hMod);
            char modName[MAX_PATH] = "unknown";
            if (hMod)
                GetModuleFileNameA(hMod, modName, MAX_PATH);
            char* baseName = strrchr(modName, '\\');
            baseName = baseName ? baseName + 1 : modName;
            char loc[512];
            snprintf(loc, sizeof(loc), "UE5 ensure() in %s at 0x%p (offset 0x%llX)", baseName,
                     pExceptionPointers->ExceptionRecord->ExceptionAddress,
                     hMod ? (unsigned long long)((uintptr_t)pExceptionPointers->ExceptionRecord->ExceptionAddress -
                                                 (uintptr_t)hMod)
                          : 0ULL);
            TraceCrash(loc);
        }

        // Quick inline dump: richer than MiniDumpNormal, but still synchronous and
        // lightweight enough for assert/terminate paths.
        if (g_pMiniDumpWriteDump) {
            SYSTEMTIME st;
            GetLocalTime(&st);
            char dumpPath[MAX_PATH];
            snprintf(dumpPath, sizeof(dumpPath), "%s\\assert_%04u%02u%02u_%02u%02u%02u_%03u_pid%lu.dmp",
                     dumpDir.c_str(), st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                     GetCurrentProcessId());

            HANDLE hFile = CreateFileA(dumpPath, GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                                       FILE_ATTRIBUTE_NORMAL, NULL);
            if (hFile != INVALID_HANDLE_VALUE) {
                MINIDUMP_EXCEPTION_INFORMATION mdei;
                mdei.ThreadId = GetCurrentThreadId();
                mdei.ExceptionPointers = pExceptionPointers;
                mdei.ClientPointers = FALSE;

                if (g_pMiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), hFile,
                                         ce::crash_dump_policy::kQuickAssertDumpType, &mdei, NULL, NULL)) {
                    TraceCrash("Quick assert dump written");
                    char msg[256];
                    snprintf(msg, sizeof(msg), "Assert dump: %s", dumpPath);
                    TraceCrash(msg);
                }
                CloseHandle(hFile);
            }
        }
        return EXCEPTION_CONTINUE_SEARCH;
    }

    // ANY exception that reaches here is considered potentially fatal.
    // Write a dump for ALL of them. This is the critical change: instead of
    // whitelisting "known crashes", we blacklist "known benign" and dump
    // everything else. This ensures we capture 0xC0000409, COM exceptions,
    // and any other crash codes that might not be in our known list.

    TraceCrash("CRASH DETECTED - Handling exception");

    // Prevent duplicate dumps (VEH + UEF both fire for the same crash), but only
    // suppress once a non-empty dump was actually written. A crashed/failed dump
    // worker must not permanently block the top-level retry path.
    if (g_DumpSuccessfullyWritten.load(std::memory_order_acquire)) {
        TraceCrash("Dump already successfully written by previous handler, skipping");
        return EXCEPTION_CONTINUE_SEARCH;
    }

    bool expectedAttempt = false;
    if (!g_DumpAttemptInProgress.compare_exchange_strong(expectedAttempt, true, std::memory_order_acq_rel)) {
        TraceCrash("Dump attempt already in progress, skipping duplicate handler");
        return EXCEPTION_CONTINUE_SEARCH;
    }

    // Ensure trace log goes to the correct dir
    TraceCrash("CrashHandlerExceptionFilter entered");

    char bufCode[64];
    snprintf(bufCode, sizeof(bufCode), "Exception Code: 0x%08lX", code);
    TraceCrash(bufCode);

    // Log detailed exception info for access violations
    if (code == EXCEPTION_ACCESS_VIOLATION && pExceptionPointers->ExceptionRecord->NumberParameters >= 2) {
        ULONG_PTR accessType = pExceptionPointers->ExceptionRecord->ExceptionInformation[0];
        ULONG_PTR faultAddr = pExceptionPointers->ExceptionRecord->ExceptionInformation[1];
        char avDetail[256];
        snprintf(avDetail, sizeof(avDetail), "Access Violation: %s address 0x%p",
                 accessType == 0 ? "READ from" : (accessType == 1 ? "WRITE to" : "DEP at"), (void*)faultAddr);
        TraceCrash(avDetail);

        // If fault address looks like a vtable slot (code address + small offset),
        // log the relationship using register values
        CONTEXT* ctx = pExceptionPointers->ContextRecord;
#ifdef _WIN64
        if (ctx->Rax != 0 && faultAddr >= ctx->Rax && faultAddr < ctx->Rax + 0x1000) {
            char vtableInfo[256];
            snprintf(vtableInfo, sizeof(vtableInfo), "VTable hint: RAX=0x%016llX, slot at offset +0x%llX (slot %llu)",
                     (unsigned long long)ctx->Rax, (unsigned long long)(faultAddr - ctx->Rax),
                     (unsigned long long)((faultAddr - ctx->Rax) / sizeof(void*)));
            TraceCrash(vtableInfo);
        }
#else
        if (ctx->Eax != 0 && faultAddr >= ctx->Eax && faultAddr < ctx->Eax + 0x1000) {
            char vtableInfo[256];
            snprintf(vtableInfo, sizeof(vtableInfo), "VTable hint: EAX=0x%08lX, slot at offset +0x%lX (slot %lu)",
                     (unsigned long)ctx->Eax, (unsigned long)(faultAddr - ctx->Eax),
                     (unsigned long)((faultAddr - ctx->Eax) / sizeof(void*)));
            TraceCrash(vtableInfo);
        }
        if (ctx->Ecx != 0 && faultAddr >= ctx->Ecx && faultAddr < ctx->Ecx + 0x1000) {
            char vtableInfo[256];
            snprintf(vtableInfo, sizeof(vtableInfo), "VTable hint: ECX=0x%08lX, slot at offset +0x%lX (slot %lu)",
                     (unsigned long)ctx->Ecx, (unsigned long)(faultAddr - ctx->Ecx),
                     (unsigned long)((faultAddr - ctx->Ecx) / sizeof(void*)));
            TraceCrash(vtableInfo);
        }
#endif
    }

    // DEP crash at RIP=0 with non-zero RAX indicates a function pointer
    // dispatch through NULL (vtable entry or trampoline target was zero).
    // This is the signature of a race condition during inline hook installation
    // where a concurrent thread reads a partially-written JMP instruction.
    if (code == EXCEPTION_ACCESS_VIOLATION && pExceptionPointers->ExceptionRecord->NumberParameters >= 2) {
        ULONG_PTR accessType = pExceptionPointers->ExceptionRecord->ExceptionInformation[0];
        ULONG_PTR faultAddr = pExceptionPointers->ExceptionRecord->ExceptionInformation[1];
        if (faultAddr == 0 && accessType == 2) {
            CONTEXT* ctx = pExceptionPointers->ContextRecord;
#ifdef _WIN64
            char rip0Info[512];
            snprintf(rip0Info, sizeof(rip0Info),
                     "RIP=0 DEP crash at vcall through NULL: "
                     "RAX=0x%016llX RCX=0x%016llX RDX=0x%016llX R8=0x%016llX R9=0x%016llX "
                     "RSP=0x%016llX - possible trampoline race condition",
                     (unsigned long long)ctx->Rax, (unsigned long long)ctx->Rcx, (unsigned long long)ctx->Rdx,
                     (unsigned long long)ctx->R8, (unsigned long long)ctx->R9, (unsigned long long)ctx->Rsp);
#else
            char rip0Info[256];
            snprintf(rip0Info, sizeof(rip0Info),
                     "EIP=0 DEP crash at vcall through NULL: "
                     "EAX=0x%08lX ECX=0x%08lX EDX=0x%08lX ESP=0x%08lX",
                     (unsigned long)ctx->Eax, (unsigned long)ctx->Ecx, (unsigned long)ctx->Edx,
                     (unsigned long)ctx->Esp);
#endif
            TraceCrash(rip0Info);
        }
    }

    // Log crash address with module info
    {
        HMODULE hCrashMod = NULL;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)pExceptionPointers->ExceptionRecord->ExceptionAddress, &hCrashMod);
        char modName[MAX_PATH] = "unknown";
        if (hCrashMod)
            GetModuleFileNameA(hCrashMod, modName, MAX_PATH);

        char* modBaseName = strrchr(modName, '\\');
        modBaseName = modBaseName ? modBaseName + 1 : modName;

        char crashLoc[512];
        snprintf(crashLoc, sizeof(crashLoc), "Crash in module: %s at 0x%p (base=0x%p, offset=0x%llX)", modBaseName,
                 pExceptionPointers->ExceptionRecord->ExceptionAddress, (void*)hCrashMod,
                 hCrashMod ? (unsigned long long)((uintptr_t)pExceptionPointers->ExceptionRecord->ExceptionAddress -
                                                  (uintptr_t)hCrashMod)
                           : 0ULL);
        TraceCrash(crashLoc);
    }

    // Log key registers for post-mortem analysis
    {
        CONTEXT* ctx = pExceptionPointers->ContextRecord;
        char regBuf[512];
#ifdef _WIN64
        snprintf(regBuf, sizeof(regBuf),
                 "Registers: RIP=0x%016llX RSP=0x%016llX RBP=0x%016llX "
                 "RAX=0x%016llX RCX=0x%016llX RDX=0x%016llX R8=0x%016llX R9=0x%016llX",
                 (unsigned long long)ctx->Rip, (unsigned long long)ctx->Rsp, (unsigned long long)ctx->Rbp,
                 (unsigned long long)ctx->Rax, (unsigned long long)ctx->Rcx, (unsigned long long)ctx->Rdx,
                 (unsigned long long)ctx->R8, (unsigned long long)ctx->R9);
#else
        snprintf(regBuf, sizeof(regBuf),
                 "Registers: EIP=0x%08X ESP=0x%08X EBP=0x%08X "
                 "EAX=0x%08X ECX=0x%08X EDX=0x%08X",
                 ctx->Eip, ctx->Esp, ctx->Ebp, ctx->Eax, ctx->Ecx, ctx->Edx);
#endif
        TraceCrash(regBuf);
    }

    // Log instruction bytes at crash site for offline disassembly
    {
        unsigned char instrBytes[16] = {};
        SIZE_T bytesRead = 0;
        if (ReadProcessMemory(GetCurrentProcess(), pExceptionPointers->ExceptionRecord->ExceptionAddress, instrBytes,
                              sizeof(instrBytes), &bytesRead) &&
            bytesRead > 0) {
            char instrBuf[128] = "Crash instr:";
            char hex[8];
            for (SIZE_T i = 0; i < bytesRead && i < 16; i++) {
                snprintf(hex, sizeof(hex), " %02X", instrBytes[i]);
                strcat(instrBuf, hex);
            }
            TraceCrash(instrBuf);
        }
    }

    TraceCrash("CrashHandler: safe pre-dump diagnostics complete");
    OutputDebugStringA("[CrashHandler] CRASH DETECTED! Spawning worker for minidump...\n");

    if (!g_pMiniDumpWriteDump) {
        TraceCrash("g_pMiniDumpWriteDump is NULL - cannot write dump!");
        g_DumpAttemptInProgress.store(false, std::memory_order_release);
        return EXCEPTION_CONTINUE_SEARCH;
    }

    // Release what the rest of the desktop waits on before the dump suspends this
    // process's threads.
    NotifyCrashPreDump();

    // Copy the exception state out of the crashing thread's stack before handing it to a
    // thread that can outlive this filter. See the DumpParams comment: the 5 s wait below
    // expires routinely on large dumps, and the stack is reused as soon as the exception is
    // handled elsewhere. The static slot is owned by the g_DumpAttemptInProgress exchange
    // above, and released by the worker rather than here.
    DumpParams* params = &g_DumpParamsSlot;
    params->CaptureFrom(pExceptionPointers, GetCurrentThreadId());

    // Spawn thread to handle dump writing (crucial for Stack Overflow exceptions)
    HANDLE hThread = CreateThread(NULL, 0, DumpWorker, params, 0, NULL);

    if (hThread) {
        TraceCrash("Worker thread spawned, waiting (5s timeout)...");
        DWORD waitResult = WaitForSingleObject(hThread, 5000);
        if (waitResult == WAIT_TIMEOUT) {
            // The worker keeps running and still owns the slot and the attempt flag. Its dump
            // is written from the copies above, so returning here cannot corrupt it.
            TraceCrash("Worker thread timed out after 5s - returning while it finishes");
        } else {
            TraceCrash("Worker thread finished.");
        }
        CloseHandle(hThread);
    } else {
        TraceCrash("Failed to create worker thread! Attempting inline dump...");
        DumpWorker(params);  // Fallback to inline if thread creation fails
    }

    TraceCrash("Handler finished - Returning EXCEPTION_CONTINUE_SEARCH");
    return EXCEPTION_CONTINUE_SEARCH;
}

static bool g_CrashHandlerInstalled = false;

static LPTOP_LEVEL_EXCEPTION_FILTER g_OldUnhandledFilter = NULL;

LONG WINAPI UnhandledExceptionFilterCallback(EXCEPTION_POINTERS* pExceptionPointers) {
    DWORD code = pExceptionPointers->ExceptionRecord->ExceptionCode;

    ActivateCrashTrace();
    char codeStr[128];
    snprintf(codeStr, sizeof(codeStr), "UnhandledExceptionFilter: 0x%08lX at 0x%p (TID:%lu)", code,
             pExceptionPointers->ExceptionRecord->ExceptionAddress, GetCurrentThreadId());
    TraceCrash(codeStr);

    // Unhandled exception filter is the LAST line of defense. ALWAYS dump,
    // regardless of exception code. If we reached this filter, the exception
    // was not handled by anyone else and the process is about to terminate.
    TraceCrash("Unhandled exception reached top-level filter - FORCING dump");
    g_ForceUnhandledDump.store(true, std::memory_order_release);
    LONG result = CrashHandlerExceptionFilter(pExceptionPointers);
    g_ForceUnhandledDump.store(false, std::memory_order_release);

    if (g_OldUnhandledFilter && g_OldUnhandledFilter != UnhandledExceptionFilterCallback) {
        LONG prevResult = g_OldUnhandledFilter(pExceptionPointers);
        if (prevResult != EXCEPTION_CONTINUE_SEARCH) {
            return prevResult;
        }
    }
    if (result == EXCEPTION_CONTINUE_EXECUTION) {
        return result;
    }

    TraceCrash("Unhandled exception consumed by crash handler");
    return EXCEPTION_EXECUTE_HANDLER;
}

void InstallCrashHandler() {
    // Prevent double-installation
    if (g_CrashHandlerInstalled) {
        TraceCrash("Crash handler already installed");
        return;
    }
    g_CrashHandlerInstalled = true;

    TraceCrash("Installing crash handler...");

    // Pre-load DbgHelp.dll to ensure it's available during a crash (avoid loader
    // lock issues). Load from System32 to prevent DLL hijacking.
    if (!g_hDbgHelp) {
        g_hDbgHelp = ce::security::LoadSystemLibrary(L"dbghelp.dll");
        if (g_hDbgHelp) {
            g_pMiniDumpWriteDump = (MINIDUMPWRITEDUMP)GetProcAddress(g_hDbgHelp, "MiniDumpWriteDump");
            TraceCrash(g_pMiniDumpWriteDump ? "DbgHelp loaded successfully" : "Failed to get MiniDumpWriteDump");
        } else {
            TraceCrash("Failed to load DbgHelp.dll");
        }
    }

    // Stay visible to WER, with its fault-report UI suppressed. WER is the only
    // thing that records a __fastfail termination (0xC0000409), which reaches
    // none of the handlers installed below - so this process must NOT opt out of
    // it via SEM_NOGPFAULTERRORBOX; see RegisterWithWER for why.
    RegisterWithWER();

    // Install Vectored Exception Handler (catches exceptions before SEH)
    PVOID vehHandle = AddVectoredExceptionHandler(1, CrashHandlerExceptionFilter);
    if (vehHandle) {
        TraceCrash("VEH handler installed");
    } else {
        TraceCrash("Failed to install VEH handler");
    }

    // No second, last-position registration: a handler ahead of CE that
    // returns EXCEPTION_CONTINUE_SEARCH passes the exception on to this one
    // anyway, and one that resumes execution has handled it. The duplicate only
    // ran every exception through this filter twice.
    //
    // The continue handler clears a recorded first-chance fault once any
    // handler resumes execution after it.
    ce::crash_first_chance::Install();

    // Also install Unhandled Exception Filter as backup
    // (some games might install their own handlers that preempt VEH)
    g_OldUnhandledFilter = SetUnhandledExceptionFilter(UnhandledExceptionFilterCallback);
    TraceCrash("Unhandled exception filter installed");

    OutputDebugStringA("[CrashHandler] Crash handler installed (VEH + VEH-continue + UnhandledFilter).\n");
}
