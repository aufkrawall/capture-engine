#include "remix_hook.h"

#include "../common/hook_common.h"
#include "../common/module_export_resolver.h"
#include "../common/remix_frame_generation_policy.h"
#include "../wrappers/iat_hook.h"

#include <atomic>
#include <cstddef>
#include <cstring>
#include <mutex>

namespace RemixHook {

#ifdef _WIN64
namespace {

using RemixErrorCode = uint32_t;
using RemixSetConfigVariable = RemixErrorCode(WINAPI*)(const char* key, const char* value);
using RemixInitializeLibrary = RemixErrorCode(WINAPI*)(const void* info, void* outInterface);

inline constexpr RemixErrorCode kRemixSuccess = 0;
inline constexpr RemixErrorCode kRemixGeneralFailure = 1;

// The public Remix interface is append-only. SetConfigVariable has occupied
// slot 10 since its introduction; only this stable prefix is needed here.
struct RemixInterfacePrefix {
    void* functionsBeforeSetConfigVariable[10];
    RemixSetConfigVariable setConfigVariable;
};

static_assert(offsetof(RemixInterfacePrefix, setConfigVariable) == 10 * sizeof(void*));

std::once_flag g_dynamicRegistrationOnce;
std::atomic<HMODULE> g_remixModule{nullptr};
std::atomic<RemixSetConfigVariable> g_originalSetConfigVariable{nullptr};
std::atomic<uint32_t> g_lastAppliedGeneratedFrames{0};
std::atomic<uint32_t> g_lastObservedNgxGeneratedFrames{UINT32_MAX};

// IATHook's dynamic registry publishes the original into caller-owned storage
// immediately before it returns CE's detour. This follows the same stable
// storage contract as the other process-wide dynamic API hooks.
RemixInitializeLibrary g_originalInitializeLibrary = nullptr;
thread_local bool g_insideSetConfigVariable = false;

uint32_t ConfiguredGeneratedFrames() {
    return DLSSFGMultiplierToGeneratedFrames(GetActiveGraphicsConfig().parsed.dlssFGFactor);
}

bool IsRemixApiModule(const char* moduleBaseName, HMODULE module) {
    if (!module || !moduleBaseName || _stricmp(moduleBaseName, "d3d9.dll") != 0)
        return false;
    return ce::module_export::ResolveAddressDirect(module, "remixapi_InitializeLibrary") != nullptr;
}

RemixErrorCode ApplyScheduleOverride(uint32_t generatedFrames, const char* source,
                                     uint32_t observedGeneratedFrames = 0) {
    const char* value = ce::remix_fg::GeneratedFrameCountString(generatedFrames);
    RemixSetConfigVariable original = g_originalSetConfigVariable.load(std::memory_order_acquire);
    if (!value || !original || g_insideSetConfigVariable)
        return kRemixGeneralFailure;

    g_insideSetConfigVariable = true;
    const RemixErrorCode result = original(ce::remix_fg::kScheduleOption, value);
    g_insideSetConfigVariable = false;
    if (result == kRemixSuccess) {
        g_lastAppliedGeneratedFrames.store(generatedFrames, std::memory_order_release);
        if (observedGeneratedFrames) {
            HookLogImportant(
                "RTX Remix FG: applied upstream scheduler override %s=%s (%ux output, source=%s, "
                "observedGeneratedFrames=%u)",
                ce::remix_fg::kScheduleOption, value, generatedFrames + 1,
                source ? source : "unknown", observedGeneratedFrames);
        } else {
            HookLogImportant("RTX Remix FG: applied upstream scheduler override %s=%s (%ux output, source=%s)",
                             ce::remix_fg::kScheduleOption, value, generatedFrames + 1,
                             source ? source : "unknown");
        }
    } else {
        HookLogImportant(
            "RTX Remix FG: FAILED to apply upstream scheduler override %s=%s (result=%u source=%s)",
            ce::remix_fg::kScheduleOption, value, result, source ? source : "unknown");
    }
    return result;
}

RemixErrorCode WINAPI HookedSetConfigVariable(const char* key, const char* value) {
    RemixSetConfigVariable original = g_originalSetConfigVariable.load(std::memory_order_acquire);
    if (!original)
        return kRemixGeneralFailure;
    if (HookIsShuttingDown() || g_insideSetConfigVariable)
        return original(key, value);

    const uint32_t generatedFrames = ConfiguredGeneratedFrames();
    const bool overrideSchedule = key && value &&
        ce::remix_fg::ShouldOverrideConfigVariable(key, generatedFrames);
    const char* forwardedValue = value;
    if (overrideSchedule)
        forwardedValue = ce::remix_fg::GeneratedFrameCountString(generatedFrames);

    g_insideSetConfigVariable = true;
    const RemixErrorCode result = original(key, forwardedValue);
    g_insideSetConfigVariable = false;
    if (overrideSchedule && result == kRemixSuccess) {
        g_lastAppliedGeneratedFrames.store(generatedFrames, std::memory_order_release);
        if (strcmp(value, forwardedValue) != 0) {
            static std::atomic<uint32_t> changedValueLogs{0};
            const uint32_t logIndex = changedValueLogs.fetch_add(1, std::memory_order_relaxed);
            if (logIndex < 16 || (logIndex % 256) == 0) {
                HookLogImportant(
                    "RTX Remix FG: intercepted SetConfigVariable %s=%s -> %s (configured=%ux output, log=%u)",
                    key, value, forwardedValue, generatedFrames + 1, logIndex + 1);
            }
        }
    } else if (overrideSchedule && result != kRemixSuccess) {
        HookLogImportant("RTX Remix FG: SetConfigVariable override FAILED for %s=%s (result=%u)",
                         key, forwardedValue, result);
    }
    return result;
}

RemixErrorCode WINAPI HookedInitializeLibrary(const void* info, void* outInterface) {
    RemixInitializeLibrary original = g_originalInitializeLibrary;
    if (!original)
        return kRemixGeneralFailure;

    const RemixErrorCode result = original(info, outInterface);
    if (result != kRemixSuccess || !outInterface || HookIsShuttingDown())
        return result;

    auto* interfacePrefix = static_cast<RemixInterfacePrefix*>(outInterface);
    RemixSetConfigVariable returnedSetter = interfacePrefix->setConfigVariable;
    if (returnedSetter && returnedSetter != &HookedSetConfigVariable) {
        g_originalSetConfigVariable.store(returnedSetter, std::memory_order_release);
        interfacePrefix->setConfigVariable = &HookedSetConfigVariable;

        HMODULE owner = nullptr;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(original), &owner);
        if (owner)
            g_remixModule.store(owner, std::memory_order_release);

        const uint32_t generatedFrames = ConfiguredGeneratedFrames();
        HookLogImportant(
            "RTX Remix FG: captured public SetConfigVariable route and installed CE's resident pass-through "
            "wrapper (module=%p configuredGeneratedFrames=%u)",
            static_cast<void*>(owner), generatedFrames);
        if (generatedFrames > 0)
            ApplyScheduleOverride(generatedFrames, "remixapi_InitializeLibrary");
    }
    return result;
}

void InstallForModule(HMODULE module, const char* moduleNameOrPath) {
    if (!module || !IsRemixApiModule("d3d9.dll", module))
        return;
    if (g_remixModule.exchange(module, std::memory_order_acq_rel) == module)
        return;

    void* captured = nullptr;
    const bool patched = IATHook::PatchIATAllModules(
        "d3d9.dll", "remixapi_InitializeLibrary", reinterpret_cast<void*>(&HookedInitializeLibrary), &captured);
    if (captured && !g_originalInitializeLibrary)
        g_originalInitializeLibrary = reinterpret_cast<RemixInitializeLibrary>(captured);

    HookLogImportant(
        "RTX Remix FG: verified Remix API provider %s at %p; initializer interception armed "
        "(staticImportPatched=%d dynamicLookupArmed=1)",
        moduleNameOrPath && moduleNameOrPath[0] ? moduleNameOrPath : "d3d9.dll",
        static_cast<void*>(module), patched ? 1 : 0);
}

}  // namespace
#endif

void RegisterDynamicHooks() {
#ifdef _WIN64
    std::call_once(g_dynamicRegistrationOnce, [] {
        IATHook::RegisterDynamicHookFiltered(
            "remixapi_InitializeLibrary", reinterpret_cast<void*>(&HookedInitializeLibrary),
            reinterpret_cast<void**>(&g_originalInitializeLibrary), IsRemixApiModule);
        HookLogImportant("RTX Remix FG: registered filtered remixapi_InitializeLibrary dynamic hook");
    });
#endif
}

void Install() {
#ifdef _WIN64
    RegisterDynamicHooks();
    HMODULE module = GetModuleHandleW(L"d3d9.dll");
    if (module)
        InstallForModule(module, "loaded d3d9.dll");
    const uint32_t generatedFrames = ConfiguredGeneratedFrames();
    if (generatedFrames > 0 && g_originalSetConfigVariable.load(std::memory_order_acquire) &&
        g_lastAppliedGeneratedFrames.load(std::memory_order_acquire) != generatedFrames) {
        ApplyScheduleOverride(generatedFrames, "hook/config refresh");
    }
#endif
}

void OnModuleLoaded(HMODULE module, const char* moduleNameOrPath) {
#ifdef _WIN64
    if (!moduleNameOrPath)
        return;
    const char* baseName = strrchr(moduleNameOrPath, '\\');
    baseName = baseName ? baseName + 1 : moduleNameOrPath;
    const char* slash = strrchr(baseName, '/');
    baseName = slash ? slash + 1 : baseName;
    if (_stricmp(baseName, "d3d9.dll") == 0)
        InstallForModule(module, moduleNameOrPath);
#else
    (void)module;
    (void)moduleNameOrPath;
#endif
}

void OnModuleUnloaded(const void* moduleBase, size_t, const char*) {
#ifdef _WIN64
    HMODULE expected = static_cast<HMODULE>(const_cast<void*>(moduleBase));
    if (!expected || !g_remixModule.compare_exchange_strong(expected, nullptr, std::memory_order_acq_rel))
        return;
    g_originalInitializeLibrary = nullptr;
    g_originalSetConfigVariable.store(nullptr, std::memory_order_release);
    g_lastAppliedGeneratedFrames.store(0, std::memory_order_release);
    g_lastObservedNgxGeneratedFrames.store(UINT32_MAX, std::memory_order_release);
#else
    (void)moduleBase;
#endif
}

void ReassertFrameGenerationScheduleFromNgx(const char* parameterName, uint32_t observedGeneratedFrames,
                                            uint32_t configuredGeneratedFrames) {
#ifdef _WIN64
    if (!parameterName)
        return;
    if (!ce::remix_fg::IsNgxGeneratedFrameParameter(parameterName))
        return;
    const uint32_t lastApplied = g_lastAppliedGeneratedFrames.load(std::memory_order_acquire);
    const uint32_t previousObserved =
        g_lastObservedNgxGeneratedFrames.exchange(observedGeneratedFrames, std::memory_order_acq_rel);
    if (!ce::remix_fg::ShouldReassertFromNgx(parameterName, observedGeneratedFrames,
                                             configuredGeneratedFrames, lastApplied, previousObserved))
        return;
    if (!g_originalSetConfigVariable.load(std::memory_order_acquire)) {
        static std::atomic<uint32_t> missingRouteLogs{0};
        const uint32_t logIndex = missingRouteLogs.fetch_add(1, std::memory_order_relaxed);
        if (logIndex < 8 || (logIndex % 512) == 0) {
            HookLogImportant(
                "RTX Remix FG: NGX exposed scheduler mismatch %s=%u (configured=%u), but the public "
                "SetConfigVariable route has not been captured yet (log=%u)",
                parameterName, observedGeneratedFrames, configuredGeneratedFrames, logIndex + 1);
        }
        return;
    }
    ApplyScheduleOverride(configuredGeneratedFrames, "NGX MultiFrameCount mismatch", observedGeneratedFrames);
#else
    (void)parameterName;
    (void)observedGeneratedFrames;
    (void)configuredGeneratedFrames;
#endif
}

}  // namespace RemixHook
