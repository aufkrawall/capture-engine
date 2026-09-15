#include "dump_helper.h"

#include <windows.h>

#include <shellapi.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <string>

#include "../common/crash_dump_policy.h"
#include "../common/crash_handler.h"
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
    MINIDUMP_CALLBACK_INFORMATION callbackInformation = {};
    PMINIDUMP_CALLBACK_INFORMATION callbackParam = nullptr;
    if (wow64Stacks.Active()) {
        callbackInformation = wow64Stacks.CallbackInformation();
        callbackParam = &callbackInformation;
    }

    const bool wroteDump =
        WriteSupplementalCrashDump(dumpHint.c_str(), targetProcess, targetPid,
                                   ce::crash_dump_policy::kRichCrashDumpType, nullptr, nullptr, callbackParam);

    if (wow64Stacks.Active()) {
        char message[192];
        snprintf(message, sizeof(message),
                 "DumpHelper: WoW64 target - added %llu 32-bit thread stack range(s), %llu bytes",
                 static_cast<unsigned long long>(wow64Stacks.RangeCount()), wow64Stacks.RangeBytes());
        TraceCrash(message);
    }
    CloseHandle(targetProcess);

    TraceCrash(wroteDump ? "DumpHelper: External pre-termination dump captured"
                         : "DumpHelper: External pre-termination dump failed");
    OutputDebugStringA(wroteDump ? "[DumpHelper] External pre-termination dump captured\n"
                                 : "[DumpHelper] External pre-termination dump failed\n");
    return wroteDump ? 0 : 5;
}
