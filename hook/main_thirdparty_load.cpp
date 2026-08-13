#include "main_internal.h"

#include "../common/module_enumeration.h"
#include "common/dll_utils.h"
#include "common/third_party_load_policy.h"

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
// loader lock (sessions 20260813_020236 / 20260813_021731). Waiting for a
// trivial LoadLibrary probe to finish is waiting for exactly that queue to
// drain, not a fixed delay.
bool WaitForLoaderQuiescence() {
    const HANDLE thread = CreateThread(nullptr, 0, LoaderQuiescenceProbe, nullptr, 0, nullptr);
    if (!thread) {
        HookLog("ThirdParty preload: loader quiescence probe thread creation failed (error=%lu)",
                static_cast<unsigned long>(GetLastError()));
        return false;
    }
    const DWORD waitResult = WaitForSingleObject(thread, INFINITE);
    CloseHandle(thread);
    if (waitResult != WAIT_OBJECT_0) {
        HookLog("ThirdParty preload: loader quiescence wait failed (result=%lu)", waitResult);
        return false;
    }
    return true;
}

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
        {ce::third_party_load::Tool::kReShade, &thirdParty.reshadeDllPath},
        {ce::third_party_load::Tool::kOptiScaler, &thirdParty.optiscalerDllPath},
        // Special K loads LAST: its thread-creation hook deadlocks the loader
        // if a tool whose DllMain creates threads (OptiScaler) loads while
        // Special K's enumerator threads are mid-loader-call, and the
        // quiescence wait cannot exclude Special K's recurring enumerator
        // cycles. Loading Special K after the wait drains the other tools'
        // startup loader work instead (see third_party_load_policy.h).
        {ce::third_party_load::Tool::kSpecialK, &thirdParty.specialkDllPath},
    };

    for (size_t toolIndex = 0; toolIndex < sizeof(tools) / sizeof(tools[0]); ++toolIndex) {
        const auto& entry = tools[toolIndex];
        if (ce::third_party_load::ShouldWaitForLoaderQuiescenceBeforeToolLoad(toolIndex)) {
            WaitForLoaderQuiescence();
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
