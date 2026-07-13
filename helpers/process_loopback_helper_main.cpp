#include <windows.h>

#include <cstdint>
#include <cstdlib>
#include <filesystem>

namespace {

using WorkerEntry = int (*)(uint64_t, uint64_t, uint64_t, uint64_t, uint32_t, const wchar_t*, int, int, uint32_t);

uint64_t ParseUnsigned(const wchar_t* value) {
    return value ? _wcstoui64(value, nullptr, 10) : 0;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc != 10) {
        return ERROR_BAD_ARGUMENTS;
    }
    wchar_t executablePath[MAX_PATH]{};
    const DWORD chars = GetModuleFileNameW(nullptr, executablePath, MAX_PATH);
    if (chars == 0 || chars >= MAX_PATH) {
        return static_cast<int>(GetLastError());
    }
    const std::filesystem::path executableDir = std::filesystem::path(executablePath).parent_path();
    SetDllDirectoryW((executableDir / L"ffmpeg").c_str());
    HMODULE mediaEngine = LoadLibraryW((executableDir / L"mediaengine.dll").c_str());
    if (!mediaEngine) {
        return static_cast<int>(GetLastError());
    }
    auto entry = reinterpret_cast<WorkerEntry>(GetProcAddress(mediaEngine, "MediaEngine_RunProcessLoopbackWorker"));
    if (!entry) {
        const int error = static_cast<int>(GetLastError());
        FreeLibrary(mediaEngine);
        return error;
    }
    const int result = entry(ParseUnsigned(argv[1]), ParseUnsigned(argv[2]), ParseUnsigned(argv[3]),
                             ParseUnsigned(argv[4]), static_cast<uint32_t>(ParseUnsigned(argv[5])), argv[9],
                             static_cast<int>(ParseUnsigned(argv[6])), static_cast<int>(ParseUnsigned(argv[7])),
                             static_cast<uint32_t>(ParseUnsigned(argv[8])));
    FreeLibrary(mediaEngine);
    return result;
}
