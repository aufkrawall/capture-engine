// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall
#include <windows.h>
#include <string>

int main() {
    wchar_t path[32768]{};
    if (!GetModuleFileNameW(nullptr, path, 32768)) return 1;
    std::wstring executable(path);
    executable.resize(executable.find_last_of(L"\\/") + 1);
    executable += L"static_import_probe.exe";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION child{};
    if (!CreateProcessW(executable.c_str(), nullptr, nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                         nullptr, nullptr, &startup, &child)) return 2;
    DWORD result = 99;
    if (WaitForSingleObject(child.hProcess, 5000) == WAIT_OBJECT_0) GetExitCodeProcess(child.hProcess, &result);
    else {
        TerminateProcess(child.hProcess, result);
        WaitForSingleObject(child.hProcess, 5000);
    }
    CloseHandle(child.hThread);
    CloseHandle(child.hProcess);
    return static_cast<int>(result);
}
