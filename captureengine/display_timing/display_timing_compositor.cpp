#include "display_timing_compositor.h"

#include <windows.h>
#include <tlhelp32.h>

#include <cwchar>

uint32_t FindCompositorProcessId(uint32_t processId) {
    DWORD targetSession = 0;
    const bool targetSessionKnown = ProcessIdToSessionId(processId, &targetSession) != FALSE;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return 0;
    std::vector<CompositorCandidate> candidates;
    PROCESSENTRY32W entry = {};
    entry.dwSize = sizeof(entry);
    for (BOOL more = Process32FirstW(snapshot, &entry); more; more = Process32NextW(snapshot, &entry)) {
        if (_wcsicmp(entry.szExeFile, L"dwm.exe") != 0)
            continue;
        DWORD session = 0;
        const bool sessionKnown = ProcessIdToSessionId(entry.th32ProcessID, &session) != FALSE;
        candidates.push_back(
            {static_cast<uint32_t>(entry.th32ProcessID), static_cast<uint32_t>(session), sessionKnown});
    }
    CloseHandle(snapshot);
    return SelectCompositorProcess(candidates, static_cast<uint32_t>(targetSession), targetSessionKnown);
}
