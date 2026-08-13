#include "main_internal.h"

#include "../common/module_enumeration.h"
#include "common/dll_utils.h"
#include "common/third_party_load_policy.h"

#include <tlhelp32.h>

namespace {

// Trivial load the probe performs. Any LoadLibrary call blocks in the loader
// work-queue drain while another thread holds the loader lock, so the probe
// thread completing means every in-flight loader call has finished.
DWORD WINAPI LoaderQuiescenceProbe(LPVOID) {
    const HMODULE module = LoadLibraryW(L"version.dll");
    if (module) {
        FreeLibrary(module);
    }
    return 0;
}

// Tool DllMains spawn background threads that perform their own loader work
// (Special K's DLL enumerator frees a probe library, OptiScaler's nvapi and
// update threads delay-load webio/nvapi). Starting the next tool load while
// such a thread is mid-loader-call deadlocks: our load holds the loader lock,
// the next tool's DllMain re-enters the previous tool's loader hook, and that
// hook waits on a mutex the background thread holds while it waits for the
// loader lock (sessions 20260813_020236 / 20260813_021731 / 20260813_025615 /
// 20260813_031321). Waiting for a trivial LoadLibrary probe to finish is
// waiting for exactly that queue to drain, not a fixed delay.
bool RunLoaderQuiescenceProbe(DWORD timeoutMs) {
    const HANDLE thread = CreateThread(nullptr, 0, LoaderQuiescenceProbe, nullptr, 0, nullptr);
    if (!thread) {
        HookLog("ThirdParty preload: loader quiescence probe thread creation failed (error=%lu)",
                static_cast<unsigned long>(GetLastError()));
        return false;
    }
    const DWORD waitResult = WaitForSingleObject(thread, timeoutMs);
    CloseHandle(thread);
    if (waitResult != WAIT_OBJECT_0) {
        return false;
    }
    return true;
}

bool WaitForLoaderQuiescence() {
    return RunLoaderQuiescenceProbe(INFINITE);
}

// RAII suspension of every other thread in the process. The tools' background
// threads must not hold their loader-hook mutexes while CE's next tool
// DllMain runs under the loader lock (see third_party_load_policy.h).
class PeerThreadSuspension {
public:
    ~PeerThreadSuspension() { ResumeAll(); }
    PeerThreadSuspension(const PeerThreadSuspension&) = delete;
    PeerThreadSuspension& operator=(const PeerThreadSuspension&) = delete;
    PeerThreadSuspension() = default;

    bool Suspend() {
        const DWORD currentThreadId = GetCurrentThreadId();
        const DWORD processId = GetCurrentProcessId();
        for (int attempt = 0; attempt < 4; ++attempt) {
            const size_t previousCount = handles_.size();
            const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
            if (snapshot == INVALID_HANDLE_VALUE) {
                ResumeAll();
                return false;
            }
            THREADENTRY32 entry = {sizeof(entry)};
            bool enumerationOk = true;
            if (Thread32First(snapshot, &entry)) {
                do {
                    if (entry.th32OwnerProcessID != processId || entry.th32ThreadID == currentThreadId)
                        continue;
                    const bool alreadyTracked =
                        std::any_of(handles_.begin(), handles_.end(), [&](const std::pair<HANDLE, DWORD>& handle) {
                            return handle.second == entry.th32ThreadID;
                        });
                    if (alreadyTracked)
                        continue;
                    HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME, FALSE, entry.th32ThreadID);
                    if (!thread) {
                        if (GetLastError() == ERROR_INVALID_PARAMETER)
                            continue;  // Thread exited after the snapshot.
                        enumerationOk = false;
                        break;
                    }
                    handles_.push_back({thread, entry.th32ThreadID});
                } while (Thread32Next(snapshot, &entry));
            }
            CloseHandle(snapshot);
            if (!enumerationOk) {
                ResumeAll();
                return false;
            }
            for (size_t i = previousCount; i < handles_.size(); ++i) {
                if (SuspendThread(handles_[i].first) == static_cast<DWORD>(-1)) {
                    if (WaitForSingleObject(handles_[i].first, 0) == WAIT_OBJECT_0) {
                        CloseHandle(handles_[i].first);
                        handles_[i].first = nullptr;
                        continue;
                    }
                    ResumeAll();
                    return false;
                }
            }
            if (handles_.size() == previousCount) {
                return true;  // Stable thread set: every peer is suspended.
            }
        }
        ResumeAll();
        return false;  // Never reached a stable enumeration.
    }

    void ResumeAll() {
        for (auto& entry : handles_) {
            if (entry.first) {
                ResumeThread(entry.first);
                CloseHandle(entry.first);
            }
        }
        handles_.clear();
    }

private:
    std::vector<std::pair<HANDLE, DWORD>> handles_;
};

// True when a renamed copy of `tool` (a graphics proxy base name from the
// shared candidate list) is already mapped. One bounded module walk plus
// export/version probes, mirroring the identity scan in
// main_overlay_detect.cpp so the preloader and overlay detection never
// disagree about which tools are present. Init-time only, never on a hot path.
bool IsRenamedThirdPartyProxyLoadedForTool(ce::third_party_load::Tool tool) {
    std::vector<HMODULE> modules;
    if (!ce::EnumerateProcessModules(GetCurrentProcess(), modules)) {
        return false;
    }
    for (HMODULE module : modules) {
        HMODULE retained = nullptr;
        if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCSTR>(module),
                                &retained)) {
            continue;
        }
        char path[MAX_PATH] = {};
        if (GetModuleFileNameA(retained, path, MAX_PATH)) {
            const char* baseName = ce::graphics_runtime::ModuleFileName(path);
            bool matches = false;
            if (ce::overlay_compat::IsThirdPartyGraphicsProxyCandidateName(baseName)) {
                switch (tool) {
                    case ce::third_party_load::Tool::kReShade:
                        matches = GetProcAddress(retained, "ReShadeVersion") != nullptr ||
                                  GetProcAddress(retained, "ReShadeRegisterAddon") != nullptr ||
                                  GetProcAddress(retained, "ReShadeUnregisterAddon") != nullptr ||
                                  DllVersionStringContains(path, "ReShade");
                        break;
                    case ce::third_party_load::Tool::kSpecialK:
                        matches = GetProcAddress(retained, "SK_GetDLL") != nullptr ||
                                  GetProcAddress(retained, "SK_Inject_GetRecord") != nullptr ||
                                  DllVersionStringContains(path, "Special K");
                        break;
                    case ce::third_party_load::Tool::kOptiScaler:
                        // OptiScaler has no stable export marker; the version
                        // resource is the same evidence the identity scan uses.
                        matches = DllVersionStringContains(path, "OptiScaler");
                        break;
                    default:
                        break;
                }
            }
            FreeLibrary(retained);
            if (matches) {
                return true;
            }
            continue;
        }
        FreeLibrary(retained);
    }
    return false;
}

bool IsToolAlreadyLoaded(ce::third_party_load::Tool tool) {
    const ce::third_party_load::ToolKnownNames known = ce::third_party_load::KnownBaseNamesForTool(tool);
    for (size_t i = 0; i < known.count; ++i) {
        if (GetModuleHandleA(known.names[i])) {
            return true;
        }
    }
    return IsRenamedThirdPartyProxyLoadedForTool(tool);
}

}  // namespace

void PreloadConfiguredThirdPartyDlls() {
    if (!g_pLocalConfig) {
        return;
    }
    static std::atomic<bool> s_preloaded{false};
    if (s_preloaded.exchange(true, std::memory_order_acq_rel)) {
        return;
    }

    const auto& thirdParty = g_pLocalConfig->thirdParty;
    if (!ce::third_party_load::HasAnyThirdPartyLoadConfigured(thirdParty.specialkDllPath.c_str(),
                                                              thirdParty.reshadeDllPath.c_str(),
                                                              thirdParty.optiscalerDllPath.c_str())) {
        return;
    }

    static constexpr bool kIs64BitProcess = sizeof(void*) == 8;

    const struct {
        ce::third_party_load::Tool tool;
        const std::string* configuredPath;
    } tools[] = {
        {ce::third_party_load::Tool::kSpecialK, &thirdParty.specialkDllPath},
        {ce::third_party_load::Tool::kReShade, &thirdParty.reshadeDllPath},
        {ce::third_party_load::Tool::kOptiScaler, &thirdParty.optiscalerDllPath},
    };

    for (size_t toolIndex = 0; toolIndex < sizeof(tools) / sizeof(tools[0]); ++toolIndex) {
        const auto& entry = tools[toolIndex];
        PeerThreadSuspension suspension;
        if (ce::third_party_load::ShouldWaitForLoaderQuiescenceBeforeToolLoad(toolIndex)) {
            WaitForLoaderQuiescence();
        }
        if (ce::third_party_load::ShouldSuspendPeerThreadsForToolLoad(toolIndex)) {
            bool peersSuspended = false;
            for (int attempt = 0; attempt < 4 && !peersSuspended; ++attempt) {
                if (!suspension.Suspend()) {
                    break;
                }
                if (RunLoaderQuiescenceProbe(2000)) {
                    peersSuspended = true;
                    break;
                }
                // A peer was suspended while inside the loader or its hook;
                // resume and try the clean sequence again.
                suspension.ResumeAll();
                HookLog("ThirdParty preload: peer thread was inside the loader during suspension; retrying (%d/%d)",
                        attempt + 1, 4);
            }
            if (!peersSuspended) {
                static std::atomic<bool> s_suspendGiveUpLogged{false};
                if (!s_suspendGiveUpLogged.exchange(true, std::memory_order_acq_rel)) {
                    HookLogImportant(
                        "ThirdParty preload: could not suspend peer threads cleanly; loading the remaining tools "
                        "without peer suspension (loader-deadlock protection degraded)");
                }
            }
        }
        const std::string resolved =
            ce::third_party_load::ResolveThirdPartyDllPath(entry.tool, *entry.configuredPath, kIs64BitProcess);
        if (resolved.empty()) {
            continue;
        }

        const char* toolName = ce::third_party_load::ToolName(entry.tool);
        if (IsToolAlreadyLoaded(entry.tool)) {
            HookLog("ThirdParty preload: %s already loaded; keeping the existing copy", toolName);
            continue;
        }
        if (GetFileAttributesA(resolved.c_str()) == INVALID_FILE_ATTRIBUTES) {
            HookLog("ThirdParty preload: %s not found at %s - skipping", toolName, resolved.c_str());
            continue;
        }

        const int wideLen = MultiByteToWideChar(CP_UTF8, 0, resolved.c_str(), -1, nullptr, 0);
        if (wideLen <= 0) {
            HookLog("ThirdParty preload: %s path is not valid UTF-8 - skipping", toolName);
            continue;
        }
        std::wstring wide(static_cast<size_t>(wideLen), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, resolved.c_str(), -1, wide.data(), wideLen);
        if (!wide.empty() && wide.back() == L'\0') {
            wide.pop_back();
        }

        const HMODULE hMod = LoadRuntimeDllViaOriginal(wide.c_str(), resolved.c_str());
        if (hMod) {
            HookLogImportant("ThirdParty preload: %s loaded from %s (module=%p)", toolName,
                             resolved.c_str(), reinterpret_cast<void*>(hMod));
        } else {
            const DWORD error = GetLastError();
            HookLogImportant("ThirdParty preload: %s FAILED to load from %s (error=%lu%s)", toolName,
                             resolved.c_str(), static_cast<unsigned long>(error),
                             error == ERROR_BAD_EXE_FORMAT ? ", wrong architecture for this process" : "");
        }
    }
}
