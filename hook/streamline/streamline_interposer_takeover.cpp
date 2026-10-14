#include "streamline_interposer_takeover.h"
#include "streamline_takeover_publication.h"
#include "streamline_takeover_core_scan.h"
#include "streamline_takeover_decision.h"
#include "streamline_takeover_imports.h"

#include <psapi.h>
#include <array>
#include <atomic>
#include <cstring>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "common/logging/log_meter.h"
#include "common/platform/module_enumeration.h"
#include "hook/hooking/iat_hook.h"
#include "hook/hooking/iat_import_table.h"
#include "hook/ngx/ngx_ota_policy.h"
#include "hook/ngx/ngx_ota_runtime.h"
#include "hook/overlay/overlay_compat.h"
#include "hook/overrides/published_graphics_config.h"
#include "hook/runtime/dll_utils.h"
#include "hook/runtime/graphics_runtime_module_policy.h"
#include "hook/runtime/hook_common.h"
#include "hook/runtime/runtime_override_paths.h"
#include "hook/streamline/streamline_api_generation.h"
#include "hook/streamline/streamline_bridge.h"
#include "hook/streamline/streamline_hook.h"
#include "hook/streamline/streamline_ota_preferences.h"

// The hook worker's wake event (main.cpp); rollback requests signal it.
extern HANDLE g_hCheckHooksEvent;

namespace ce::streamline_interposer_takeover {

#if defined(_M_X64) || defined(__x86_64__)

namespace {

// Integer/pointer arguments only (see kTakeoverThunkArgumentCount). Forwarding
// unused trailing slots is harmless: the thunk reads its caller's stack frame
// and the callee ignores what it does not declare.
using PfnGenericInterposerExport = uint64_t(WINAPI*)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,
                                                     uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
static_assert(kTakeoverThunkArgumentCount == 12, "PfnGenericInterposerExport forwards twelve arguments");

constexpr bool SameName(const char* left, const char* right) {
    while (*left != '\0' && *left == *right) {
        ++left;
        ++right;
    }
    return *left == *right;
}

// The entry classification is positional: exactly the names before
// kFirstNonSlTakeoverExportIndex are sl* API exports, and index 0 is slInit.
constexpr bool TakeoverExportTableIsPartitioned() {
    if (!SameName(kTakeoverExportNames[kSlInitTakeoverExportIndex], "slInit")) {
        return false;
    }
    for (size_t i = 0; i < kTakeoverExportCount; ++i) {
        const bool slApi = kTakeoverExportNames[i][0] == 's' && kTakeoverExportNames[i][1] == 'l';
        if (slApi != (i < kFirstNonSlTakeoverExportIndex)) {
            return false;
        }
    }
    return true;
}
static_assert(TakeoverExportTableIsPartitioned(), "sl* exports must precede kFirstNonSlTakeoverExportIndex");

std::mutex g_TakeoverMutex; // Arming only; never held during runtime loading.
RuntimePublication<HMODULE> g_RuntimePublication;
TakeoverDecision g_TakeoverDecision;
TakeoverRollbackRequest g_ResidentRollback;
std::atomic<bool> g_TakeoverArmed{false};
std::atomic<bool> g_TakeoverActive{false};
std::atomic<bool> g_TakeoverFailed{false};
std::atomic<bool> g_TakeoverDeclinedFinal{false};
std::atomic<uint32_t> g_TakeoverLoadInProgressCount{0};
thread_local bool tl_TakeoverLoadInProgress = false;
thread_local bool tl_InEarlyArm = false;
thread_local bool tl_InEntryThunkActivation = false;

std::atomic<HMODULE> g_SupersededInterposerModule{nullptr};
// Provider identity is available during loader notifications. Export readiness
// belongs exclusively to RuntimePublication, whose final store follows preparation.
std::atomic<HMODULE> g_MappedCustomInterposerModule{nullptr};
// UTF-8, like every path the loader redirect compares (runtime_override_paths.h).
constexpr size_t kInterposerPathCapacity = MAX_PATH * 3;
char g_SupersededInterposerPath[kInterposerPathCapacity] = {};
char g_OverrideInterposerPath[kInterposerPathCapacity] = {};

std::array<std::atomic<void*>, kTakeoverExportCount> g_FallbackTargets{};
std::array<std::atomic<void*>, kTakeoverExportCount> g_ResolvedTargets{};
std::atomic<uint32_t> g_ThunkCallCount{0};
// The thunk whose call claimed the override, for the split-runtime diagnostic.
std::atomic<int32_t> g_ClaimingExportIndex{-1};
std::atomic<bool> g_TakeoverLoadAnnounced{false};

const char* ConfiguredStreamlineDirectory() {
    return ce::published_config::CurrentRuntimeOverridePaths().streamline;
}

uint32_t OverrideFileMajorVersion(const std::string& utf8Path, DllVersionReadPolicy policy) {
    return DllFileMajorVersionW(ce::runtime_override_path::Utf8ToWide(utf8Path).c_str(), policy);
}

std::vector<HMODULE> CollectInterposerImportingModules(const std::vector<HMODULE>* preEnumerated = nullptr) {
    std::vector<HMODULE> result;
    std::vector<HMODULE> modules;
    const std::vector<HMODULE>* list = preEnumerated;
    if (!list || list->empty()) {
        if (ce::EnumerateProcessModules(::GetCurrentProcess(), modules)) {
            list = &modules;
        }
    }
    if (list) {
        for (HMODULE mod : *list) {
            if (!mod || mod == g_hModule || mod == g_SupersededInterposerModule.load(std::memory_order_relaxed) ||
                mod == g_MappedCustomInterposerModule.load(std::memory_order_relaxed)) {
                continue;
            }
            if (ModuleImportsDll(mod, "sl.interposer.dll")) {
                result.push_back(mod);
            }
        }
    }
    if (result.empty()) {
        if (HMODULE mainExe = ::GetModuleHandleW(nullptr);
            mainExe && ModuleImportsDll(mainExe, "sl.interposer.dll")) {
            result.push_back(mainExe);
        }
    }
    return result;
}

void* TakeoverThunkAddress(size_t index);

CorePresence CurrentStreamlineCorePresence() {
    return ScanLoadedStreamlineCore(
        [](std::vector<HMODULE>& modules) { return ce::EnumerateProcessModules(::GetCurrentProcess(), modules); },
        [](HMODULE module) { return ce::runtime_override_path::ModulePath(module); });
}

void QueueResidentRollback(const char* reason) {
    g_TakeoverDeclinedFinal.store(true, std::memory_order_release);
    g_ResidentRollback.Request(reason);
    // Wake the worker now rather than at its next timeout: until it restores
    // the imports and hooks the resident interposer, CE observes no sl* call.
    if (g_hCheckHooksEvent) {
        ::SetEvent(g_hCheckHooksEvent);
    }
}

void FinishResidentRollback(const char* reason) {
    const HMODULE superseded = g_SupersededInterposerModule.exchange(nullptr, std::memory_order_acq_rel);
    g_MappedCustomInterposerModule.store(nullptr, std::memory_order_release);
    g_TakeoverArmed.store(false, std::memory_order_release);

    const std::vector<HMODULE> importingModules = CollectInterposerImportingModules();
    size_t restoredSlots = 0;
    for (HMODULE mod : importingModules) {
        for (size_t i = 0; i < kTakeoverExportCount; ++i) {
            if (void* fallback = g_FallbackTargets[i].load(std::memory_order_acquire)) {
                if (IATHook::RebindIATBaseline(mod, "sl.interposer.dll", kTakeoverExportNames[i],
                                               TakeoverThunkAddress(i), fallback)) {
                    ++restoredSlots;
                }
            }
        }
    }
    if (void* fallbackSlInit = g_FallbackTargets[0].load(std::memory_order_acquire)) {
        ce::streamline_ota::RetargetOriginalSlInit(fallbackSlInit);
    }
    if (superseded) {
        StreamlineHook::OnModuleLoaded(
            superseded, g_SupersededInterposerPath[0] ? g_SupersededInterposerPath : "sl.interposer.dll");
    }
    HookLogImportant(
        "Streamline interposer takeover rolled back (%s): resident %s (%p) already owns the runtime; "
        "restored %zu sl.interposer.dll import slot(s) across %zu module(s) and hooked resident interposer",
        reason ? reason : "unspecified", g_SupersededInterposerPath, superseded, restoredSlots,
        importingModules.size());
}

void AbandonArmedTakeover(const char* reason) {
    if (!g_TakeoverArmed.load(std::memory_order_acquire) || !g_TakeoverDecision.TrySelectResident()) {
        return;
    }
    QueueResidentRollback(reason);
}

#define CE_TAKEOVER_THUNK_PARAMS                                                                                   \
    uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7, uint64_t a8, \
        uint64_t a9, uint64_t a10, uint64_t a11
#define CE_TAKEOVER_THUNK_ARGS a0, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11

uint64_t DispatchTakeoverSlot(size_t index, CE_TAKEOVER_THUNK_PARAMS) {
    void* target = g_ResolvedTargets[index].load(std::memory_order_acquire);
    if (!target) {
        // Pending only: once resident routing is selected, the worker's import
        // restoration is queued and a per-call module scan would only cost time.
        if (!g_TakeoverActive.load(std::memory_order_acquire) && g_TakeoverArmed.load(std::memory_order_acquire) &&
            g_TakeoverDecision.IsPending()) {
            const CorePresence corePresence =
                IsTakeoverEntryExportIndex(index) ? CurrentStreamlineCorePresence() : CorePresence::Loaded;
            if (CanActivateTakeoverForExport(index, corePresence)) {
                struct ScopedEntryThunkActivation {
                    ScopedEntryThunkActivation() { tl_InEntryThunkActivation = true; }
                    ~ScopedEntryThunkActivation() { tl_InEntryThunkActivation = false; }
                } activationGuard;
                g_ClaimingExportIndex.store(static_cast<int32_t>(index), std::memory_order_relaxed);
                EnsureCustomInterposerReady();
            } else {
                AbandonArmedTakeover(IsTakeoverEntryExportIndex(index)
                                         ? "Streamline core already mapped before takeover entry"
                                         : kTakeoverExportNames[index]);
            }
        }
        // A peer entry thunk may have selected the override before its targets
        // are published. Join that load instead of rolling back or forwarding
        // this call to the resident interposer.
        if (g_TakeoverDecision.IsCustom()) {
            EnsureCustomInterposerReady();
        }
        target = g_ResolvedTargets[index].load(std::memory_order_acquire);
        if (!target && g_TakeoverDecision.IsCustom() && g_RuntimePublication.Query()) {
            // The override owns the runtime but lacks this export (a dynamic
            // lookup the import coverage check could not see). The resident
            // copy was never initialized, so forwarding there would split it.
            static ce::log_meter::ChangeGate missingExportGate;
            if (const auto verdict = missingExportGate.Observe(index)) {
                HookLogImportant("Streamline interposer takeover: override lacks %s; the call fails instead of "
                                 "reaching the uninitialized resident interposer (%llu repeat(s) since last line)",
                                 kTakeoverExportNames[index], static_cast<unsigned long long>(verdict.suppressed));
            }
            return static_cast<uint64_t>(E_FAIL);
        }
        if (!target) {
            target = g_FallbackTargets[index].load(std::memory_order_acquire);
        }
    }
    if (!target) {
        return static_cast<uint64_t>(E_FAIL);
    }
    if (g_ThunkCallCount.load(std::memory_order_relaxed) < 8) {
        const uint32_t callIndex = g_ThunkCallCount.fetch_add(1, std::memory_order_relaxed) + 1;
        if (callIndex <= 8) {
            HookLogImportant("Streamline interposer takeover: forwarded thunk call #%u (%s) -> %p", callIndex,
                             kTakeoverExportNames[index], target);
        }
    }
    return reinterpret_cast<PfnGenericInterposerExport>(target)(CE_TAKEOVER_THUNK_ARGS);
}

template <size_t Index>
uint64_t WINAPI InterposerTakeoverThunk(CE_TAKEOVER_THUNK_PARAMS) {
    return DispatchTakeoverSlot(Index, CE_TAKEOVER_THUNK_ARGS);
}

#undef CE_TAKEOVER_THUNK_ARGS
#undef CE_TAKEOVER_THUNK_PARAMS

template <size_t... Indices>
constexpr std::array<PfnGenericInterposerExport, sizeof...(Indices)> MakeTakeoverThunkTable(
    std::index_sequence<Indices...>) {
    return {&InterposerTakeoverThunk<Indices>...};
}

constexpr auto g_TakeoverThunkTable = MakeTakeoverThunkTable(std::make_index_sequence<kTakeoverExportCount>{});

void* TakeoverThunkAddress(size_t index) {
    return reinterpret_cast<void*>(g_TakeoverThunkTable[index]);
}

bool TryArmTakeover(const char* phase) {
    if (g_TakeoverArmed.load(std::memory_order_acquire)) {
        return true;
    }
    if (g_TakeoverDeclinedFinal.load(std::memory_order_acquire) || g_TakeoverFailed.load(std::memory_order_acquire)) {
        return false;
    }
    if (!CurrentProcessOwnsProcessLocalRuntimeOverrides() || ce::streamline_bridge::IsActive()) {
        return false;
    }

    const char* streamlineDir = ConfiguredStreamlineDirectory();
    if (!streamlineDir || !streamlineDir[0]) {
        return false;
    }

    const HMODULE resident = ::GetModuleHandleA("sl.interposer.dll");
    if (!resident) {
        return false;
    }

    const std::string residentPath = ce::runtime_override_path::ModulePath(resident);
    const std::string overridePath = CanonicalOverridePath(streamlineDir, "sl.interposer.dll");
    const bool foreignResident =
        ce::graphics_runtime::IsForeignStreamlineInterposer(residentPath.c_str(), overridePath.c_str());
    if (!foreignResident) {
        g_TakeoverDeclinedFinal.store(true, std::memory_order_release);
        return false;
    }

    std::vector<HMODULE> enumeratedModules;
    const CorePresence corePresence = ScanLoadedStreamlineCore(
        [&](std::vector<HMODULE>& modules) {
            if (!ce::EnumerateProcessModules(::GetCurrentProcess(), modules)) {
                return false;
            }
            enumeratedModules = modules;
            return true;
        },
        [](HMODULE module) { return ce::runtime_override_path::ModulePath(module); });
    const bool anyCoreLoaded = corePresence != CorePresence::Absent;
    const bool overrideExists = !overridePath.empty() && overridePath.size() < kInterposerPathCapacity &&
                                ce::runtime_override_path::Exists(overridePath);
    uint32_t residentMajor = ModuleFileMajorVersion(resident);
    if (residentMajor == 0 && !tl_InEarlyArm && !residentPath.empty()) {
        residentMajor = OverrideFileMajorVersion(residentPath, DllVersionReadPolicy::AllowLoaderFallback);
    }
    const auto versionPolicy = tl_InEarlyArm ? DllVersionReadPolicy::DirectPeOnly
                                           : DllVersionReadPolicy::AllowLoaderFallback;
    const uint32_t overrideMajor = overrideExists ? OverrideFileMajorVersion(overridePath, versionPolicy) : 0u;

    if (!ce::graphics_runtime::CanTakeOverForeignStreamlineInterposer(
            true, foreignResident, anyCoreLoaded, overrideExists, residentMajor, overrideMajor)) {
        g_TakeoverDeclinedFinal.store(true, std::memory_order_release);
        HookLogImportant(
            "Streamline interposer takeover declined (%s): resident=%s (v%u.x) override=%s (exists=%d v%u.x) "
            "coreAlreadyLoaded=%d coreScan=%s",
            phase ? phase : "unspecified", residentPath.empty() ? "unresolved" : residentPath.c_str(), residentMajor,
            overridePath.c_str(), overrideExists ? 1 : 0, overrideMajor, anyCoreLoaded ? 1 : 0,
            corePresence == CorePresence::Unresolved ? "unresolved" : "complete");
        return false;
    }

    // Every import must be able to move to the override at once; see
    // streamline_takeover_import_coverage.h. Plain file I/O, so DllMain-safe.
    const std::vector<HMODULE> importingModules = CollectInterposerImportingModules(&enumeratedModules);
    const ImportCoverage coverage = MeasureImportCoverage(importingModules, overridePath);
    const ImportCoverageVerdict coverageVerdict = DecideImportCoverage(coverage);
    if (coverageVerdict != ImportCoverageVerdict::Complete) {
        g_TakeoverDeclinedFinal.store(true, std::memory_order_release);
        HookLogImportant(
            "Streamline interposer takeover declined (%s): %s - %zu module(s) import %zu named (%zu thunked), "
            "%zu ordinal and %zu unnamed sl.interposer.dll export(s); override %s lacks %zu (first: %s). The game "
            "keeps its own interposer rather than splitting one runtime across two",
            phase ? phase : "unspecified", ImportCoverageVerdictName(coverageVerdict), importingModules.size(),
            coverage.named, coverage.thunked, coverage.ordinal, coverage.unnamed, overridePath.c_str(),
            coverage.missingFromOverride, coverage.firstMissing.empty() ? "none" : coverage.firstMissing.c_str());
        return false;
    }

    std::lock_guard<std::mutex> lock(g_TakeoverMutex);
    if (g_TakeoverArmed.load(std::memory_order_acquire)) {
        return true;
    }

    strncpy_s(g_SupersededInterposerPath, sizeof(g_SupersededInterposerPath), residentPath.c_str(), _TRUNCATE);
    strncpy_s(g_OverrideInterposerPath, sizeof(g_OverrideInterposerPath), overridePath.c_str(), _TRUNCATE);
    g_SupersededInterposerModule.store(resident, std::memory_order_release);

    for (size_t i = 0; i < kTakeoverExportCount; ++i) {
        void* proc = reinterpret_cast<void*>(::GetProcAddress(resident, kTakeoverExportNames[i]));
        g_FallbackTargets[i].store(proc, std::memory_order_release);
    }

    g_TakeoverArmed.store(true, std::memory_order_release);

    size_t patchedSlots = 0;
    for (HMODULE mod : importingModules) {
        for (size_t i = 0; i < kTakeoverExportCount; ++i) {
            void* orig = nullptr;
            if (IATHook::PatchIAT(mod, "sl.interposer.dll", kTakeoverExportNames[i], TakeoverThunkAddress(i), &orig)) {
                if (orig && !g_FallbackTargets[i].load(std::memory_order_relaxed)) {
                    g_FallbackTargets[i].store(orig, std::memory_order_release);
                }
                ++patchedSlots;
            }
        }
    }

    HookLogImportant(
        "Streamline interposer takeover armed (%s): gated %zu sl.interposer.dll import slot(s) across %zu module(s) "
        "from %s -> %s (Streamline %u.x); %zu further named import(s) move at activation",
        phase ? phase : "unspecified", patchedSlots, importingModules.size(), g_SupersededInterposerPath,
        g_OverrideInterposerPath, residentMajor, coverage.named - coverage.thunked);
    return true;
}

HMODULE ResolveCustomInterposerForModuleQuery() {
    // GetModuleHandle/GetProcAddress are legal from DLL callbacks. They must
    // neither start a load nor wait for a thread that may need the loader lock.
    return g_RuntimePublication.Query();
}

}  // namespace

HMODULE PublishedCustomInterposerForModuleQuery() {
    return ResolveCustomInterposerForModuleQuery();
}

void ArmEarlyFromDllMain() {
    struct ScopedEarlyArm {
        ScopedEarlyArm() { tl_InEarlyArm = true; }
        ~ScopedEarlyArm() { tl_InEarlyArm = false; }
    } earlyGuard;
    TryArmTakeover("DllMain");
}

void ServicePendingRollback() {
    g_ResidentRollback.Service(&FinishResidentRollback);
}

bool EnsureCustomInterposerReady() {
    if (g_TakeoverActive.load(std::memory_order_acquire)) {
        return true;
    }
    if (tl_InEarlyArm || tl_TakeoverLoadInProgress || g_TakeoverFailed.load(std::memory_order_acquire) ||
        g_TakeoverDeclinedFinal.load(std::memory_order_acquire)) {
        return false;
    }
    if (!g_TakeoverArmed.load(std::memory_order_acquire) && !TryArmTakeover("HookThread")) {
        return false;
    }
    if (!tl_InEntryThunkActivation && !g_TakeoverDecision.IsCustom()) {
        return false;
    }

    if (!g_OverrideInterposerPath[0]) {
        return false;
    }

    const std::wstring widePath = ce::runtime_override_path::Utf8ToWide(g_OverrideInterposerPath);
    if (widePath.empty()) {
        return false;
    }
    if (!g_TakeoverDecision.TryClaimCustom(tl_InEntryThunkActivation)) {
        return false;
    }

    struct ScopedTakeoverLoadGuard {
        ScopedTakeoverLoadGuard() {
            tl_TakeoverLoadInProgress = true;
            g_TakeoverLoadInProgressCount.fetch_add(1, std::memory_order_acq_rel);
        }
        ~ScopedTakeoverLoadGuard() {
            g_TakeoverLoadInProgressCount.fetch_sub(1, std::memory_order_acq_rel);
            tl_TakeoverLoadInProgress = false;
        }
    } loadGuard;

    const HMODULE superseded = g_SupersededInterposerModule.load(std::memory_order_acquire);
    std::array<void*, kTakeoverExportCount> preparedTargets{};
    const HMODULE customModule = g_RuntimePublication.LoadAndPublish(
        [&] {
            if (!g_TakeoverLoadAnnounced.exchange(true, std::memory_order_acq_rel)) {
                HookLogImportant("Streamline interposer takeover: loading %s; module queries remain nonblocking",
                                 g_OverrideInterposerPath);
            }
            const HMODULE loaded = LoadRuntimeDllViaOriginal(widePath.c_str(), g_OverrideInterposerPath);
            return loaded != superseded ? loaded : nullptr;
        },
        [&](HMODULE loaded) {
            HMODULE pinned = nullptr;
            if (!::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                                      reinterpret_cast<LPCWSTR>(loaded), &pinned)) {
                return false;
            }
            for (size_t i = 0; i < kTakeoverExportCount; ++i) {
                preparedTargets[i] = reinterpret_cast<void*>(::GetProcAddress(loaded, kTakeoverExportNames[i]));
            }
            return true;
        },
        [&](HMODULE loaded) {
            // Publication only: these stores and slInit retargeting make no
            // loader calls. Competing preparations never overwrite this set.
            g_MappedCustomInterposerModule.store(loaded, std::memory_order_release);
            for (size_t i = 0; i < kTakeoverExportCount; ++i) {
                g_ResolvedTargets[i].store(preparedTargets[i], std::memory_order_release);
            }
            if (preparedTargets[0]) {
                ce::streamline_ota::RetargetOriginalSlInit(preparedTargets[0]);
            }
        });
    if (!customModule || customModule == superseded) {
        if (g_TakeoverFailed.exchange(true, std::memory_order_acq_rel)) {
            return false;
        }
        g_TakeoverDecision.SelectResidentAfterLoadFailure();
        g_MappedCustomInterposerModule.store(nullptr, std::memory_order_release);
        QueueResidentRollback("custom interposer load failed");
        HookLogImportant(
            "Streamline interposer takeover FAILED to load %s (module=%p superseded=%p) - selected resident "
            "sl.interposer.dll; import restoration queued for hook worker (%s)",
            g_OverrideInterposerPath, customModule, superseded, g_SupersededInterposerPath);
        return false;
    }

    if (!g_RuntimePublication.ClaimBinding()) {
        return true;
    }
    const std::vector<HMODULE> importingModules = CollectInterposerImportingModules();

    size_t reboundSlots = 0;
    for (HMODULE mod : importingModules) {
        for (size_t i = 0; i < kTakeoverExportCount; ++i) {
            void* target = g_ResolvedTargets[i].load(std::memory_order_acquire);
            if (!target) {
                continue;
            }
            if (IATHook::RebindIATBaseline(mod, "sl.interposer.dll", kTakeoverExportNames[i], TakeoverThunkAddress(i),
                                           target)) {
                ++reboundSlots;
            }
        }
    }
    // The imports no thunk gates (most of the Vulkan proxy, D3D11 helpers)
    // still point at the dormant interposer; move them in the same pass.
    DirectRebindResult direct;
    for (HMODULE mod : importingModules) {
        const DirectRebindResult moduleResult = RebindUnthunkedImports(mod, superseded, customModule);
        direct.rebound += moduleResult.rebound;
        if (moduleResult.failed != 0 && direct.failed == 0) {
            direct.firstFailed = moduleResult.firstFailed;
        }
        direct.failed += moduleResult.failed;
    }

    PatchLoadLibraryIatForLateLoadedModule(customModule, g_OverrideInterposerPath);
    PatchModuleHandleIatForModule(customModule);
    PatchProcessCreationIatForLateLoadedModule(customModule, g_OverrideInterposerPath);

    g_TakeoverActive.store(true, std::memory_order_release);

    HookLogImportant(
        "Streamline interposer takeover ACTIVE: loaded and pinned %s (%p), superseding dormant %s (%p) and rebound "
        "%zu thunked + %zu direct sl.interposer.dll import slot(s) across %zu module(s)",
        g_OverrideInterposerPath, customModule, g_SupersededInterposerPath, superseded, reboundSlots, direct.rebound,
        importingModules.size());
    if (direct.failed != 0) {
        HookLogImportant(
            "Streamline interposer takeover: %zu direct import slot(s) could not move to the override (first: %s); "
            "those calls still reach the dormant interposer",
            direct.failed, direct.firstFailed.c_str());
    }
    return true;
}

bool IsTakeoverArmed() {
    return g_TakeoverArmed.load(std::memory_order_acquire) && !g_TakeoverDecision.IsResident();
}

bool IsTakeoverActive() {
    return g_TakeoverActive.load(std::memory_order_acquire);
}

bool ShouldAllowTakeoverInterposerLoad(const char* candidatePath) {
    if (!candidatePath || !candidatePath[0]) {
        return false;
    }
    if (!g_TakeoverDecision.AllowsCustomLoad(g_TakeoverArmed.load(std::memory_order_acquire),
                                            g_TakeoverLoadInProgressCount.load(std::memory_order_acquire) != 0)) {
        return false;
    }
    return g_OverrideInterposerPath[0] != '\0' &&
           ce::graphics_runtime::EqualsModulePathIgnoreCase(candidatePath, g_OverrideInterposerPath);
}

void NoteTakeoverInterposerMapped(HMODULE module, const char* resolvedPath) {
    if (!module || !resolvedPath || !resolvedPath[0] || !IsTakeoverArmed()) {
        return;
    }
    if (module == g_SupersededInterposerModule.load(std::memory_order_acquire)) {
        return;
    }
    if (g_OverrideInterposerPath[0] != '\0' &&
        ce::graphics_runtime::EqualsModulePathIgnoreCase(resolvedPath, g_OverrideInterposerPath)) {
        g_MappedCustomInterposerModule.store(module, std::memory_order_release);
        return;
    }
    if (!g_TakeoverActive.load(std::memory_order_acquire) &&
        g_TakeoverLoadInProgressCount.load(std::memory_order_acquire) == 0 &&
        IsStreamlineCoreModulePath(resolvedPath)) {
        AbandonArmedTakeover("Streamline core mapped before takeover entry");
    }
    // A core from the dormant interposer's own folder after the override was
    // claimed means the game's resident slInit was already running when the
    // claiming thunk fired (it entered before the imports were gated): one
    // runtime is now split. Not recoverable here, but it must be visible.
    if (g_TakeoverDecision.IsCustom() && IsStreamlineCoreModulePath(resolvedPath) &&
        g_SupersededInterposerPath[0] != '\0') {
        const char* residentName = ce::graphics_runtime::ModuleFileName(g_SupersededInterposerPath);
        const char* coreName = ce::graphics_runtime::ModuleFileName(resolvedPath);
        const size_t residentDir = static_cast<size_t>(residentName - g_SupersededInterposerPath);
        if (static_cast<size_t>(coreName - resolvedPath) == residentDir &&
            _strnicmp(resolvedPath, g_SupersededInterposerPath, residentDir) == 0) {
            const int32_t claimed = g_ClaimingExportIndex.load(std::memory_order_relaxed);
            HookLogImportant(
                "Streamline interposer takeover: SPLIT RUNTIME - the dormant interposer mapped its own core %s after "
                "the override was claimed by %s; the game's resident slInit was already running when the imports "
                "were gated",
                resolvedPath,
                claimed >= 0 && static_cast<size_t>(claimed) < kTakeoverExportCount ? kTakeoverExportNames[claimed]
                                                                                     : "a worker join");
        }
    }
}

HMODULE ActiveInterposerOr(HMODULE fallback) {
    if (g_TakeoverDecision.IsResident())
        return fallback;
    HMODULE custom = g_MappedCustomInterposerModule.load(std::memory_order_acquire);
    return custom ? custom : fallback;
}

HMODULE ResolveModuleByNameForRedirect(const char* baseName) {
    if (!baseName || !baseName[0]) {
        return nullptr;
    }
    if (!g_TakeoverDecision.IsResident() && ce::graphics_runtime::IsStreamlineInterposerModuleQuery(baseName)) {
        if (HMODULE custom = g_MappedCustomInterposerModule.load(std::memory_order_acquire)) {
            return custom;
        }
    }
    return ::GetModuleHandleA(baseName);
}

HMODULE GetCustomInterposerModule() {
    return g_TakeoverDecision.IsResident() ? nullptr : g_MappedCustomInterposerModule.load(std::memory_order_acquire);
}

HMODULE GetSupersededInterposerModule() {
    return g_SupersededInterposerModule.load(std::memory_order_acquire);
}

bool IsSupersededInterposerModule(HMODULE module) {
    if (!module || !IsTakeoverArmed()) {
        return false;
    }
    const HMODULE superseded = g_SupersededInterposerModule.load(std::memory_order_acquire);
    const HMODULE custom = g_MappedCustomInterposerModule.load(std::memory_order_acquire);
    return superseded != nullptr && module == superseded && module != custom;
}

bool IsOwnedStreamlineInterposerModule(const char* sourceModule, HMODULE ownerModule) {
    if (!sourceModule || !ownerModule || !ce::graphics_runtime::IsStreamlineInterposerModuleQuery(sourceModule)) {
        return false;
    }
    return ownerModule == g_SupersededInterposerModule.load(std::memory_order_acquire) ||
           ownerModule == g_MappedCustomInterposerModule.load(std::memory_order_acquire);
}

HMODULE RedirectInterposerModuleForGetProcAddress(HMODULE requestedModule) {
    if (!requestedModule || !IsTakeoverArmed()) {
        return requestedModule;
    }
    if (requestedModule != g_SupersededInterposerModule.load(std::memory_order_acquire)) {
        return requestedModule;
    }
    if (HMODULE custom = ResolveCustomInterposerForModuleQuery()) {
        return custom;
    }
    return requestedModule;
}

FARPROC ResolvePendingTakeoverExport(HMODULE requestedModule, LPCSTR name) {
    if (!IsSupersededInterposerModule(requestedModule)) {
        return nullptr;
    }
    // Publication may race the earlier HMODULE redirect; recheck here rather
    // than letting that lookup escape to the superseded image.
    if (HMODULE ready = g_RuntimePublication.Query()) {
        return ::GetProcAddress(ready, name);
    }
    if (reinterpret_cast<uintptr_t>(name) < 0x10000) {
        return nullptr;
    }
    for (size_t i = 0; i < kTakeoverExportCount; ++i) {
        if (std::strcmp(name, kTakeoverExportNames[i]) == 0 &&
            g_FallbackTargets[i].load(std::memory_order_acquire)) {
            return reinterpret_cast<FARPROC>(TakeoverThunkAddress(i));
        }
    }
    return nullptr;
}

void PatchInterposerIatForLateLoadedModule(HMODULE module, const char* moduleNameOrPath) {
    if (!module || !IsTakeoverArmed()) {
        return;
    }
    if (moduleNameOrPath &&
        (strstr(moduleNameOrPath, "capture_hook") != nullptr ||
         strstr(moduleNameOrPath, "d3d12_wrappers") != nullptr ||
         ce::overlay_compat::IsThirdPartyOverlayModulePath(moduleNameOrPath) ||
         ce::overlay_compat::IsStreamlineFrameGenerationModulePath(moduleNameOrPath))) {
        return;
    }
    if (!ModuleImportsDll(module, "sl.interposer.dll")) {
        return;
    }
    const bool active = g_TakeoverActive.load(std::memory_order_acquire);
    if (active) {
        // Whichever interposer the loader bound this module to, every name
        // must end on the override, like the modules bound at activation.
        const DirectRebindResult direct = RebindUnthunkedImports(
            module, g_SupersededInterposerModule.load(std::memory_order_acquire), g_RuntimePublication.Query());
        if (direct.failed != 0) {
            HookLogImportant("Streamline interposer takeover: late module %s keeps %zu import(s) on the dormant "
                             "interposer (first: %s)",
                             moduleNameOrPath ? moduleNameOrPath : "unnamed", direct.failed,
                             direct.firstFailed.c_str());
        }
    }
    for (size_t i = 0; i < kTakeoverExportCount; ++i) {
        if (active) {
            if (void* target = g_ResolvedTargets[i].load(std::memory_order_acquire)) {
                IATHook::RebindIATBaseline(module, "sl.interposer.dll", kTakeoverExportNames[i],
                                           TakeoverThunkAddress(i), target);
            }
        } else {
            void* dummy = nullptr;
            IATHook::PatchIAT(module, "sl.interposer.dll", kTakeoverExportNames[i], TakeoverThunkAddress(i), &dummy);
        }
    }
}

#else

void ArmEarlyFromDllMain() {}
void ServicePendingRollback() {}
bool EnsureCustomInterposerReady() { return false; }
bool IsTakeoverArmed() { return false; }
bool IsTakeoverActive() { return false; }
bool ShouldAllowTakeoverInterposerLoad(const char*) { return false; }
void NoteTakeoverInterposerMapped(HMODULE, const char*) {}
HMODULE ActiveInterposerOr(HMODULE fallback) { return fallback; }
HMODULE ResolveModuleByNameForRedirect(const char* baseName) { return baseName ? ::GetModuleHandleA(baseName) : nullptr; }
HMODULE GetCustomInterposerModule() { return nullptr; }
HMODULE GetSupersededInterposerModule() { return nullptr; }
bool IsSupersededInterposerModule(HMODULE) { return false; }
bool IsOwnedStreamlineInterposerModule(const char*, HMODULE) { return false; }
HMODULE RedirectInterposerModuleForGetProcAddress(HMODULE requestedModule) { return requestedModule; }
FARPROC ResolvePendingTakeoverExport(HMODULE, LPCSTR) { return nullptr; }
HMODULE PublishedCustomInterposerForModuleQuery() { return nullptr; }
void PatchInterposerIatForLateLoadedModule(HMODULE, const char*) {}
#endif

}  // namespace ce::streamline_interposer_takeover
