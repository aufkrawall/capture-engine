#include "streamline_bridge_runtime.h"

#include <atomic>
#include <cstring>
#include <string>
#include <vector>

#include "../../common/config.h"
#include "../../common/module_enumeration.h"
#include "../common/dll_utils.h"
#include "../common/graphics_runtime_module_policy.h"
#include "../common/hook_common.h"
#include "streamline_bridge_policy.h"

#if defined(_M_X64) || defined(__x86_64__)
#include "sl.h"
#include "sl_core_types.h"
#endif

// `g_pLocalConfig` (AppConfig*) comes from hook_common.h.

namespace ce::streamline_bridge {
namespace {

namespace api = ce::streamline_api;

enum class LegacyQuiesceState : uint8_t {
    NotRequired,
    Pending,
    Running,
    Complete,
    Failed,
};

std::atomic<LegacyQuiesceState> g_legacyQuiesceState{LegacyQuiesceState::NotRequired};
thread_local bool g_thisThreadOwnsLegacyQuiesce = false;

std::string RuntimeFeaturePath(const std::string& runtimeDir, const char* baseName) {
    std::string path = runtimeDir;
    if (!path.empty() && path.back() != '\\' && path.back() != '/') {
        path += '\\';
    }
    path += baseName;
    return path;
}

bool QueryModulePath(HMODULE module, char* path, size_t pathSize) {
    if (!module || !path || pathSize == 0 || pathSize > MAX_PATH) {
        return false;
    }
    const DWORD length = GetModuleFileNameA(module, path, static_cast<DWORD>(pathSize));
    if (length == 0 || length >= pathSize) {
        path[0] = '\0';
        return false;
    }
    return true;
}

bool SameModuleImageIsStillLoaded(HMODULE module, const char* expectedPath) {
    HMODULE current = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCSTR>(module), &current) ||
        current != module) {
        return false;
    }
    char currentPath[MAX_PATH] = {};
    return QueryModulePath(current, currentPath, sizeof(currentPath)) &&
           ce::graphics_runtime::EqualsModulePathIgnoreCase(currentPath, expectedPath);
}

bool PreloadReplacementNgxFeatureModules(const std::string& runtimeDir) {
    std::vector<HMODULE> replacements;
    for (const char* baseName : {"nvngx_dlss.dll", "nvngx_dlssg.dll"}) {
        const std::string configuredPath = RuntimeFeaturePath(runtimeDir, baseName);
        HMODULE module = LoadLibraryA(configuredPath.c_str());
        const DWORD loadError = module ? ERROR_SUCCESS : GetLastError();
        char loadedPath[MAX_PATH] = {};
        SetLastError(ERROR_SUCCESS);
        const bool pathResolved = module && QueryModulePath(module, loadedPath, sizeof(loadedPath));
        const DWORD pathError = pathResolved ? ERROR_SUCCESS : GetLastError();
        if (!module || !pathResolved ||
            !ce::graphics_runtime::EqualsModulePathIgnoreCase(configuredPath.c_str(), loadedPath)) {
            HookLogImportant(
                "Streamline bridge: FAILED to preload configured replacement NGX image %s (module=%p path=%s "
                "error=%lu)",
                configuredPath.c_str(), reinterpret_cast<void*>(module),
                loadedPath[0] ? loadedPath : "(unresolved)", module ? pathError : loadError);
            for (HMODULE replacement : replacements) {
                FreeLibrary(replacement);
            }
            if (module) {
                FreeLibrary(module);
            }
            return false;
        }
        replacements.push_back(module);  // Deliberately pin the configured image for the bridge lifetime.
    }

    // A racing absolute load must not recreate the state this boundary exists
    // to prevent. Verify every physical SR/FG image, not GetModuleHandle's first
    // answer, before allowing the V2 interposer to initialize.
    std::vector<HMODULE> modules;
    if (!ce::EnumerateProcessModules(GetCurrentProcess(), modules)) {
        HookLogImportant(
            "Streamline bridge: FAILED to verify configured NGX replacement ownership after preload");
        for (HMODULE replacement : replacements) {
            FreeLibrary(replacement);
        }
        return false;
    }
    for (HMODULE module : modules) {
        char loadedPath[MAX_PATH] = {};
        if (!QueryModulePath(module, loadedPath, sizeof(loadedPath)) ||
            !ce::graphics_runtime::IsBridgeNgxFeatureModuleName(loadedPath)) {
            continue;
        }
        const std::string configuredPath = RuntimeFeaturePath(
            runtimeDir, ce::graphics_runtime::ModuleFileName(loadedPath));
        if (!ce::graphics_runtime::EqualsModulePathIgnoreCase(configuredPath.c_str(), loadedPath)) {
            HookLogImportant(
                "Streamline bridge: FAILED replacement ownership check: foreign NGX image %s became resident "
                "before 2.x initialization",
                loadedPath);
            for (HMODULE replacement : replacements) {
                FreeLibrary(replacement);
            }
            return false;
        }
    }

    HookLogImportant(
        "Streamline bridge: preloaded and pinned the configured NGX SR/FG replacements from %s before 2.x "
        "initialization",
        runtimeDir.c_str());
    return true;
}

std::wstring Widen(const char* text) {
    if (!text || !*text) {
        return std::wstring();
    }
    const int len = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
    std::wstring out(len > 0 ? static_cast<size_t>(len) : 0, L'\0');
    if (len > 0) {
        MultiByteToWideChar(CP_UTF8, 0, text, -1, out.data(), len);
    }
    while (!out.empty() && out.back() == L'\0') {
        out.pop_back();
    }
    return out;
}

// Streamline's own log, into CE's session directory, at trace log level only.
//
// The bridge drives somebody else's runtime through an ABI CE reconstructed, so when it goes
// wrong CE's log can say what CE did but not what Streamline made of it. Session
// `20260821_161620` is the case for this: the process died on a C++ exception whose throw
// site had been fully unwound by the time the terminate handler ran, and nothing in CE's own
// diagnostics could distinguish a Streamline that had refused something from a game failing
// for its own reasons. `sl.log` is NVIDIA's account of plugin loading, device binding and
// feature init, and it costs nothing when CE is not in trace mode.
//
// Returned by value and held by the caller for the duration of `slInit`, which is the only
// call that reads it.
std::wstring StreamlineLogDirectory(uint32_t* outLogLevel) {
    // sl::LogLevel: eOff = 0, eDefault = 1, eVerbose = 2.
    *outLogLevel = 0;
    if (!g_pLocalConfig || !IsTraceLoggingEnabled(g_pLocalConfig->logLevel)) {
        return std::wstring();
    }
    char dir[MAX_PATH] = {};
    if (!GetSessionLogsDirectory(dir, sizeof(dir))) {
        return std::wstring();
    }
    *outLogLevel = 2;
    return Widen(dir);
}

// Confirms the module CE just loaded really speaks 2.x, from its exports rather than only
// its file version. The version resource says which generation NVIDIA stamped; the export
// markers say which API the image actually presents, and the bridge is about to bind to
// that API.
bool ExportsLookLike2x(HMODULE module) {
    bool anyV2 = false;
    for (size_t i = 0; i < api::kV2OnlyExportCount; ++i) {
        if (GetProcAddress(module, api::kV2OnlyExports[i])) {
            anyV2 = true;
            break;
        }
    }
    bool anyV1 = false;
    for (size_t i = 0; i < api::kV1OnlyExportCount; ++i) {
        if (GetProcAddress(module, api::kV1OnlyExports[i])) {
            anyV1 = true;
            break;
        }
    }
    return api::Classify(anyV2, anyV1) == api::Generation::V2;
}

// Brings the 2.x runtime up with the preferences the bridge pins. Returns false without
// leaving anything taken over, so a failure here is simply "no bridge".
//
// This used to hand-mirror `sl::Preferences`, and that mirror was wrong on its first write:
// `BaseStructure` puts `next` at offset 0 and `structType` at 8, the reverse of how the
// declaration reads, which would have initialised the runtime with a garbage struct type and
// failed nowhere near its cause. It was then corrected and verified field by field - which is
// work that has to be redone every time the staged SDK moves, against a header CE now has on
// its include path anyway. The same change that let the translation use the real
// `sl::Constants` and `sl::DLSSOptions` makes the mirror pure risk for no benefit, so it is
// gone: `featuresToLoad`, `renderAPI` and the rest are now the SDK's own fields by
// construction rather than by a re-verified claim about somebody else's ABI.
#if defined(_M_X64) || defined(__x86_64__)
bool InitializeV2Runtime(HMODULE v2Interposer, const std::string& runtimeDir) {
    auto slInit = reinterpret_cast<PFun_slInit*>(GetProcAddress(v2Interposer, "slInit"));
    if (!slInit) {
        HookLogImportant("Streamline bridge: the 2.x runtime exports no slInit - not bridging");
        return false;
    }

    const std::wstring widePath = Widen(runtimeDir.c_str());
    const wchar_t* paths[] = {widePath.c_str()};
    const sl::Feature features[] = {sl::kFeatureDLSS, sl::kFeatureDLSS_G, sl::kFeatureReflex, sl::kFeaturePCL};

    // NGX requires either a valid NVIDIA application ID or a stable project identity. CE
    // usually activates after the game's own slInit, so the game's application ID is not
    // observable; derive a stable project identity from the host executable instead. The
    // strings only have to live through slInit - Streamline copies them into its own state.
    char hostPath[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, hostPath, MAX_PATH);
    uint32_t hostMajor = 0, hostMinor = 0, hostPatch = 0;
    const bool haveHostVersion =
        DllFileVersionParts(hostPath, &hostMajor, &hostMinor, &hostPatch);
    const std::string engineVersion = haveHostVersion
                                          ? StreamlineEngineVersion(hostMajor, hostMinor, hostPatch, 0)
                                          : std::string("unknown");
    const std::string projectId = StreamlineProjectId(hostPath);

    uint32_t logLevel = 0;
    const std::wstring logDir = StreamlineLogDirectory(&logLevel);

    sl::Preferences prefs{};
    prefs.pathsToPlugins = paths;
    prefs.numPathsToPlugins = 1;
    prefs.flags = static_cast<sl::PreferenceFlags>(BridgePreferenceFlags());
    prefs.featuresToLoad = features;
    prefs.numFeaturesToLoad = static_cast<uint32_t>(sizeof(features) / sizeof(features[0]));
    prefs.logLevel = static_cast<sl::LogLevel>(logLevel);
    prefs.renderAPI = sl::RenderAPI::eD3D12;
    if (hostPath[0] != '\0') {
        prefs.engineVersion = engineVersion.c_str();
        prefs.projectId = projectId.c_str();
    }
    if (!logDir.empty()) {
        prefs.pathToLogsAndData = logDir.c_str();
    }

    // Declare the SDK version this runtime actually is, not one CE was built against.
    uint32_t major = 0, minor = 0, patch = 0;
    const std::string interposerPath = runtimeDir + "\\sl.interposer.dll";
    if (!DllFileVersionParts(interposerPath.c_str(), &major, &minor, &patch) || major != 2) {
        HookLogImportant("Streamline bridge: cannot read a 2.x version from %s - not bridging",
                         interposerPath.c_str());
        return false;
    }
    const sl::Result result = slInit(prefs, StreamlineSdkVersion(major, minor, patch));
    if (result != sl::Result::eOk) {
        HookLogImportant(
            "Streamline bridge: Streamline %u.%u.%u refused to initialise (sl::Result=%d) with plugins from %s - "
            "not bridging, the game keeps its own Streamline",
            major, minor, patch, static_cast<int>(result), runtimeDir.c_str());
        return false;
    }
    HookLogImportant(
        "Streamline bridge: Streamline %u.%u.%u initialised with plugins pinned to %s (OTA off, DXGI factory "
        "proxy on, sl.log %s)",
        major, minor, patch, runtimeDir.c_str(),
        logDir.empty() ? "off - raise log_level to trace for Streamline's own account"
                       : "verbose in the session log directory");
    return true;
}
#else
// Streamline has no 32-bit runtime, and the bridge never activates here.
bool InitializeV2Runtime(HMODULE, const std::string&) { return false; }
#endif

}  // namespace

api::Generation ProcessGeneration() {
    HMODULE interposer = GetModuleHandleA("sl.interposer.dll");
    if (!interposer) {
        return api::Generation::Unknown;
    }
    char path[MAX_PATH] = {};
    if (GetModuleFileNameA(interposer, path, MAX_PATH) == 0) {
        return api::Generation::Unknown;
    }
    return api::GenerationFromMajorVersion(DllFileMajorVersion(path));
}

std::vector<LegacyNgxFeatureModule> CaptureLegacyNgxFeatureModules() {
    std::vector<HMODULE> modules;
    if (!ce::EnumerateProcessModules(GetCurrentProcess(), modules)) {
        HookLogImportant(
            "Streamline bridge: could not inventory the legacy NGX feature images before 1.x shutdown");
        return {};
    }

    std::vector<LegacyNgxFeatureModule> featureModules;
    for (HMODULE module : modules) {
        char path[MAX_PATH] = {};
        if (QueryModulePath(module, path, sizeof(path)) &&
            ce::graphics_runtime::IsBridgeNgxFeatureModuleName(path)) {
            featureModules.push_back({module, path});
        }
    }
    return featureModules;
}

bool RetireLegacyNgxFeatureModules(const std::vector<LegacyNgxFeatureModule>& legacyModules,
                                  const std::string& runtimeDir) {
    bool allRetired = true;
    for (const LegacyNgxFeatureModule& legacy : legacyModules) {
        if (!SameModuleImageIsStillLoaded(legacy.module, legacy.path.c_str())) {
            continue;  // The legacy shutdown already unloaded it.
        }

        const char* loadedPath = legacy.path.c_str();
        const char* baseName = ce::graphics_runtime::ModuleFileName(loadedPath);
        const std::string configuredPath = RuntimeFeaturePath(runtimeDir, baseName);
        if (!ce::graphics_runtime::ShouldRetireLegacyBridgeNgxFeatureModule(
                true, configuredPath.c_str(), loadedPath)) {
            continue;
        }
        if (GetFileAttributesA(configuredPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
            HookLogImportant(
                "Streamline bridge: cannot retire legacy NGX image %s because its configured replacement %s "
                "does not exist",
                loadedPath, configuredPath.c_str());
            allRetired = false;
            continue;
        }

        // Release only the legacy runtime's surviving feature-image reference.
        // Draining an opaque loader count would risk stealing a reference from
        // an independent integration in the same process. Never touch an image
        // that was not captured while 1.x was live.
        const bool released = FreeLibrary(legacy.module) != FALSE;

        if (SameModuleImageIsStillLoaded(legacy.module, loadedPath)) {
            HookLogImportant(
                "Streamline bridge: FAILED to retire legacy NGX image %s after its post-shutdown loader-reference "
                "release (FreeLibrary=%s); "
                "initializing %s would leave two feature generations resident",
                loadedPath, released ? "true" : "false", configuredPath.c_str());
            allRetired = false;
            continue;
        }
        HookLogImportant(
            "Streamline bridge: retired legacy NGX image %s after 1.x shutdown; 2.x will load %s",
            loadedPath, configuredPath.c_str());
    }
    return allRetired;
}

void RequireLegacyRuntimeQuiesce() {
    LegacyQuiesceState expected = LegacyQuiesceState::NotRequired;
    g_legacyQuiesceState.compare_exchange_strong(
        expected, LegacyQuiesceState::Pending, std::memory_order_release,
        std::memory_order_relaxed);
}

bool LegacyRuntimeQuiesceAllowsV2Initialization() {
    const LegacyQuiesceState state = g_legacyQuiesceState.load(std::memory_order_acquire);
    return state == LegacyQuiesceState::NotRequired || state == LegacyQuiesceState::Complete;
}

bool QuiesceLegacyRuntimeForBridgeCall(LegacyStreamlineShutdown shutdown,
                                       const std::string& runtimeDir) {
    for (;;) {
        LegacyQuiesceState state = g_legacyQuiesceState.load(std::memory_order_acquire);
        if (state == LegacyQuiesceState::NotRequired || state == LegacyQuiesceState::Complete) {
            return true;
        }
        if (state == LegacyQuiesceState::Failed) {
            return false;
        }
        if (state == LegacyQuiesceState::Running) {
            // A legacy shutdown can re-enter a hooked graphics path on its own
            // thread. That nested call must keep using 1.x; waiting here would
            // deadlock the shutdown, while starting 2.x recreated the mixed-NGX
            // state observed in session 20260822_185158.
            if (g_thisThreadOwnsLegacyQuiesce) {
                static std::atomic<bool> logged{false};
                if (!logged.exchange(true, std::memory_order_relaxed)) {
                    HookLogImportant(
                        "Streamline bridge: legacy shutdown re-entered a bridged path; keeping that nested call "
                        "on 1.x until teardown completes");
                }
                return false;
            }
            g_legacyQuiesceState.wait(LegacyQuiesceState::Running, std::memory_order_acquire);
            continue;
        }

        if (!g_legacyQuiesceState.compare_exchange_weak(
                state, LegacyQuiesceState::Running, std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            continue;
        }

        g_thisThreadOwnsLegacyQuiesce = true;
        bool shutdownOk = false;
        bool ngxRetired = false;
        bool replacementsReady = false;
        if (!shutdown) {
            HookLogImportant(
                "Streamline bridge: the game's 1.x runtime is up but its slShutdown slot was never captured; "
                "refusing to initialize 2.x beside it");
        } else {
            const std::vector<LegacyNgxFeatureModule> legacyNgxModules = CaptureLegacyNgxFeatureModules();
            shutdownOk = shutdown();
            ngxRetired = shutdownOk && RetireLegacyNgxFeatureModules(legacyNgxModules, runtimeDir);
            replacementsReady = ngxRetired && PreloadReplacementNgxFeatureModules(runtimeDir);
            HookLogImportant(
                "Streamline bridge: shut the game's own 1.x Streamline runtime down (returned %s); legacy NGX "
                "feature images retired=%s; configured replacements ready=%s; 2.x initialization permitted=%s",
                shutdownOk ? "true" : "false", ngxRetired ? "true" : "false",
                replacementsReady ? "true" : "false",
                shutdownOk && ngxRetired && replacementsReady ? "true" : "false");
            LogStreamlineModuleInventory("after quiescing the 1.x runtime and before initializing 2.x");
        }

        const bool complete = shutdownOk && ngxRetired && replacementsReady;
        g_thisThreadOwnsLegacyQuiesce = false;
        g_legacyQuiesceState.store(complete ? LegacyQuiesceState::Complete : LegacyQuiesceState::Failed,
                                   std::memory_order_release);
        g_legacyQuiesceState.notify_all();
        return complete;
    }
}

// Every Streamline module a 1.x game can have resident, so one log line settles which
// runtime is actually in the process.
//
// This is the measurement the first two bridged sessions were missing. "The game already
// drove its own Streamline" named a decision without naming the state behind it, so nothing
// in the log distinguished a game one call into `slInit` from one that had already bound
// every feature plugin - and those want opposite answers. Printing the inventory at the
// decision point, and again after the 1.x runtime is shut down, makes both checkable.
void LogStreamlineModuleInventory(const char* when) {
    static const char* const kNames[] = {"sl.interposer.dll", "sl.common.dll", "sl.dlss.dll",
                                         "sl.dlss_g.dll",     "sl.reflex.dll", "sl.pcl.dll",
                                         "sl.nis.dll"};
    for (const char* name : kNames) {
        HMODULE module = GetModuleHandleA(name);
        if (!module) {
            continue;
        }
        char path[MAX_PATH] = {};
        if (GetModuleFileNameA(module, path, MAX_PATH) == 0) {
            continue;
        }
        uint32_t major = 0, minor = 0, patch = 0;
        DllFileVersionParts(path, &major, &minor, &patch);
        HookLogImportant("Streamline bridge inventory (%s): %s %u.%u.%u resident from %s", when, name, major, minor,
                         patch, path);
    }
    std::vector<HMODULE> modules;
    if (ce::EnumerateProcessModules(GetCurrentProcess(), modules)) {
        for (HMODULE module : modules) {
            char path[MAX_PATH] = {};
            if (!QueryModulePath(module, path, sizeof(path)) ||
                !ce::graphics_runtime::IsBridgeNgxFeatureModuleName(path)) {
                continue;
            }
            uint32_t major = 0, minor = 0, patch = 0;
            DllFileVersionParts(path, &major, &minor, &patch);
            HookLogImportant("Streamline bridge inventory (%s): %s %u.%u.%u resident from %s (module=%p)",
                             when, ce::graphics_runtime::ModuleFileName(path), major, minor, patch, path,
                             reinterpret_cast<void*>(module));
        }
    }
    if (strncmp(when, "after quiescing the 1.x runtime", strlen("after quiescing the 1.x runtime")) == 0 &&
        GetModuleHandleA("sl.interposer.dll")) {
        HookLogImportant(
            "Streamline bridge: the statically imported 1.x sl.interposer.dll remains mapped because the OS loader "
            "holds the executable's import reference; its plugins are gone and every live import slot reaches CE");
    }
}

HMODULE LoadAndInitializeV2Runtime(const std::string& runtimeDir) {
    const std::string interposerPath = runtimeDir + "\\sl.interposer.dll";

    HMODULE v2Interposer = LoadLibraryA(interposerPath.c_str());
    if (!v2Interposer) {
        HookLogImportant(
            "Streamline bridge: failed to load %s (error %lu) - every bridged call falls back to the game's own "
            "1.x runtime",
            interposerPath.c_str(), GetLastError());
        return nullptr;
    }
    if (!ExportsLookLike2x(v2Interposer)) {
        HookLogImportant(
            "Streamline bridge: %s does not present the 2.x export surface despite its file version - every "
            "bridged call falls back to the game's own 1.x runtime",
            interposerPath.c_str());
        return nullptr;
    }
    if (!InitializeV2Runtime(v2Interposer, runtimeDir)) {
        return nullptr;
    }
    return v2Interposer;
}

}  // namespace ce::streamline_bridge
