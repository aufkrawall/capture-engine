#include "dump_helper.h"

#include <windows.h>

#include <shellapi.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <string>

#include "common/crash/crash_dump_policy.h"
#include "common/crash/crash_handler.h"
#include "dump_helper_fault_neighborhood.h"
#include "dump_helper_wow64_stacks.h"

namespace {

std::string WideToUtf8(const wchar_t* value) {
    if (!value || value[0] == L'\0') {
        return "";
    }

    const int required = WideCharToMultiByte(CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
    if (required <= 1) {
        return "";
    }

    std::string converted(static_cast<size_t>(required - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value, -1, converted.data(), required, nullptr, nullptr);
    return converted;
}

bool ParseDumpHelperPid(const wchar_t* value, DWORD* outPid) {
    if (!value || !outPid || value[0] == L'\0') {
        return false;
    }

    wchar_t* end = nullptr;
    const unsigned long parsed = wcstoul(value, &end, 10);
    if (!end || *end != L'\0' || parsed == 0 || parsed > 0xFFFFFFFFul) {
        return false;
    }

    *outPid = static_cast<DWORD>(parsed);
    return true;
}

bool TryGetWideArgumentValue(int argc, wchar_t** argv, const wchar_t* prefix, const wchar_t** outValue) {
    if (!argv || !prefix || !outValue) {
        return false;
    }

    const size_t prefixLen = wcslen(prefix);
    for (int i = 1; i < argc; ++i) {
        if (wcsncmp(argv[i], prefix, prefixLen) == 0) {
            *outValue = argv[i] + prefixLen;
            return true;
        }
    }

    return false;
}

bool HasWideArgument(int argc, wchar_t** argv, const wchar_t* argument) {
    if (!argv || !argument) {
        return false;
    }

    for (int i = 1; i < argc; ++i) {
        if (wcscmp(argv[i], argument) == 0) {
            return true;
        }
    }

    return false;
}

// One memory-callback slot serves both range sources: the fault neighborhood
// first (its windows are the ones that say why the target faulted), then the
// WoW64 32-bit stacks. Source order is per dump attempt; the zero-length answer
// ends the attempt and re-arms the first source for the next one.
struct DumpHelperCallbackChain {
    Wow64StackCollector* wow64 = nullptr;
    FaultNeighborhoodCollector* fault = nullptr;
    bool faultExhausted = false;

    MINIDUMP_CALLBACK_INFORMATION CallbackInformation() {
        MINIDUMP_CALLBACK_INFORMATION information = {};
        information.CallbackRoutine = &DumpHelperCallbackChain::Routine;
        information.CallbackParam = this;
        return information;
    }

    static BOOL CALLBACK Routine(PVOID param, const PMINIDUMP_CALLBACK_INPUT input,
                                 PMINIDUMP_CALLBACK_OUTPUT output) {
        auto* chain = static_cast<DumpHelperCallbackChain*>(param);
        if (!chain || !input || !output) {
            return TRUE;
        }
        if (input->CallbackType == MemoryCallback) {
            if (chain->fault && !chain->faultExhausted && chain->fault->ServeMemoryRange(output)) {
                return TRUE;
            }
            chain->faultExhausted = true;
            if (chain->wow64) {
                Wow64StackCollector::MinidumpCallback(chain->wow64, input, output);
                if (output->MemorySize != 0) {
                    return TRUE;
                }
            }
            output->MemoryBase = 0;
            output->MemorySize = 0;
            chain->faultExhausted = false;
            return TRUE;
        }
        if (chain->wow64) {
            Wow64StackCollector::MinidumpCallback(chain->wow64, input, output);
        }
        return TRUE;
    }
};

}  // namespace

bool IsDumpHelperCommandLine(const char* commandLine) {
    return commandLine && strstr(commandLine, "--dump-helper") != nullptr;
}

int RunDumpHelperFromCommandLine() {
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) {
        return 2;
    }

    const wchar_t* pidArg = nullptr;
    const wchar_t* dirArg = nullptr;
    const wchar_t* hintArg = nullptr;
    DWORD targetPid = 0;
    const bool parsed = HasWideArgument(argc, argv, L"--dump-helper") &&
                        TryGetWideArgumentValue(argc, argv, L"--dump-helper-pid=", &pidArg) &&
                        TryGetWideArgumentValue(argc, argv, L"--dump-helper-dir=", &dirArg) &&
                        ParseDumpHelperPid(pidArg, &targetPid);
    TryGetWideArgumentValue(argc, argv, L"--dump-helper-hint=", &hintArg);
    const wchar_t* scopeArg = nullptr;
    TryGetWideArgumentValue(argc, argv, L"--dump-helper-scope=", &scopeArg);
    // Optional: the target's own EXCEPTION_POINTERS and faulting thread, so the
    // dump's exception stream names the fault instead of the helper launch.
    const wchar_t* exceptionArg = nullptr;
    const wchar_t* exceptionTidArg = nullptr;
    unsigned long long exceptionPointersAddress = 0;
    DWORD exceptionThreadId = 0;
    if (TryGetWideArgumentValue(argc, argv, L"--dump-helper-exception=", &exceptionArg) &&
        TryGetWideArgumentValue(argc, argv, L"--dump-helper-tid=", &exceptionTidArg) &&
        ParseDumpHelperPid(exceptionTidArg, &exceptionThreadId)) {
        wchar_t* end = nullptr;
        exceptionPointersAddress = wcstoull(exceptionArg, &end, 16);
        if (!end || *end != L'\0')
            exceptionPointersAddress = 0;
    }
    // Only the caller knows whether the process's memory is worth recording. A
    // freeze the application explains itself - its own modal dialog on the
    // render thread - asks for stacks, an application assertion for the fatal
    // assert scope; everything else gets the rich dump.
    const auto scope = ce::crash_dump_policy::ParseExternalDumpScopeArgument(scopeArg);

    const std::string dumpDir = WideToUtf8(dirArg);
    const std::string dumpHint = WideToUtf8(hintArg && hintArg[0] ? hintArg : L"fatal_exit_external_helper.dmp");
    LocalFree(reinterpret_cast<HLOCAL>(argv));

    if (!parsed || dumpDir.empty() || dumpHint.empty()) {
        OutputDebugStringA("[DumpHelper] Invalid dump helper command line\n");
        return 2;
    }

    std::error_code ec;
    std::filesystem::create_directories(dumpDir, ec);
    if (ec) {
        OutputDebugStringA("[DumpHelper] Failed to create dump directory\n");
        return 3;
    }

    SetCrashDumpDirectory(dumpDir);
    InstallCrashHandler();

    HANDLE targetProcess =
        OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | PROCESS_DUP_HANDLE | SYNCHRONIZE, FALSE, targetPid);
    if (!targetProcess) {
        OutputDebugStringA("[DumpHelper] Failed to open target process\n");
        return 4;
    }

    // This helper is x64. Against a 32-bit target every thread stack the dump
    // records is the WoW64 syscall thunk, which is why this crash and the two
    // sessions in commit 339eccf0 could not be walked past the faulting
    // instruction. The collector hands dbghelp the 32-bit stacks as extra
    // memory regions; a 64-bit target needs none of it and gets the dump it
    // always got.
    ActivateCrashTrace();
    Wow64StackCollector wow64Stacks(targetProcess, targetPid);
    FaultNeighborhoodCollector faultNeighborhood(targetProcess, exceptionPointersAddress);
    DumpHelperCallbackChain chain;
    chain.wow64 = wow64Stacks.Active() ? &wow64Stacks : nullptr;
    chain.fault = faultNeighborhood.Active() ? &faultNeighborhood : nullptr;
    MINIDUMP_CALLBACK_INFORMATION callbackInformation = {};
    PMINIDUMP_CALLBACK_INFORMATION callbackParam = nullptr;
    if (chain.wow64 || chain.fault) {
        callbackInformation = chain.CallbackInformation();
        callbackParam = &callbackInformation;
    }

    const MINIDUMP_TYPE dumpType = ce::crash_dump_policy::ExternalHelperDumpType(scope);
    MINIDUMP_EXCEPTION_INFORMATION exceptionInformation = {};
    PMINIDUMP_EXCEPTION_INFORMATION exceptionParam = nullptr;
    if (exceptionPointersAddress != 0 && exceptionThreadId != 0) {
        exceptionInformation.ThreadId = exceptionThreadId;
        exceptionInformation.ExceptionPointers =
            reinterpret_cast<PEXCEPTION_POINTERS>(static_cast<uintptr_t>(exceptionPointersAddress));
        exceptionInformation.ClientPointers = TRUE;
        exceptionParam = &exceptionInformation;
        TraceCrash("DumpHelper: Recording the target's exception context (ClientPointers)");
    }
    // WriteSupplementalCrashDump retries without the exception stream if dbghelp
    // cannot read it, so a bad pointer costs the stream, never the dump.
    const bool wroteDump = WriteSupplementalCrashDump(dumpHint.c_str(), targetProcess, targetPid, dumpType,
                                                      exceptionParam, nullptr, callbackParam);
    if (scope == ce::crash_dump_policy::ExternalDumpScope::kStacks) {
        TraceCrash("DumpHelper: Stack-only scope requested - thread stacks, thread info and modules only");
    } else if (scope == ce::crash_dump_policy::ExternalDumpScope::kFatalAssert) {
        TraceCrash("DumpHelper: Fatal-assert scope requested - stacks, handles, stack-referenced memory and the "
                   "memory map; no module data segments");
    }

    if (wow64Stacks.Active()) {
        char message[192];
        snprintf(message, sizeof(message),
                 "DumpHelper: WoW64 target - added %llu 32-bit thread stack range(s), %llu bytes",
                 static_cast<unsigned long long>(wow64Stacks.RangeCount()), wow64Stacks.RangeBytes());
        TraceCrash(message);
    }
    if (faultNeighborhood.Active()) {
        char message[256];
        snprintf(message, sizeof(message),
                 "DumpHelper: Fault neighborhood - added %llu range(s), %llu bytes (code windows: %llu, "
                 "reference windows: %llu, register windows: %llu)",
                 static_cast<unsigned long long>(faultNeighborhood.RangeCount()), faultNeighborhood.RangeBytes(),
                 static_cast<unsigned long long>(faultNeighborhood.CodeWindowCount()),
                 static_cast<unsigned long long>(faultNeighborhood.ReferenceWindowCount()),
                 static_cast<unsigned long long>(faultNeighborhood.RegisterWindowCount()));
        TraceCrash(message);
    }
    CloseHandle(targetProcess);

    TraceCrash(wroteDump ? "DumpHelper: External pre-termination dump captured"
                         : "DumpHelper: External pre-termination dump failed");
    OutputDebugStringA(wroteDump ? "[DumpHelper] External pre-termination dump captured\n"
                                 : "[DumpHelper] External pre-termination dump failed\n");
    return wroteDump ? 0 : 5;
}
