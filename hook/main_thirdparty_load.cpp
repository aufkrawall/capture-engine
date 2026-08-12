#include "main_internal.h"

#include "../common/module_enumeration.h"
#include "common/dll_utils.h"
#include "common/third_party_load_policy.h"

namespace {

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

    for (const auto& entry : tools) {
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
