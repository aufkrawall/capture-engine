#include "streamline_interposer_takeover.h"
#include "streamline_takeover_publication.h"
#include "streamline_takeover_core_scan.h"
#include "streamline_takeover_decision.h"

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

namespace ce::streamline_interposer_takeover {

#if defined(_M_X64) || defined(__x86_64__)

namespace {

using PfnGenericInterposerExport = uint64_t(WINAPI*)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);

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
char g_SupersededInterposerPath[MAX_PATH] = {};
char g_OverrideInterposerPath[MAX_PATH] = {};

std::array<std::atomic<void*>, kTakeoverExportCount> g_FallbackTargets{};
std::array<std::atomic<void*>, kTakeoverExportCount> g_ResolvedTargets{};
std::atomic<uint32_t> g_ThunkCallCount{0};
std::atomic<bool> g_TakeoverLoadAnnounced{false};

const char* ConfiguredStreamlineDirectory() {
    return (g_pLocalConfig && g_LocalConfigLoaded.load(std::memory_order_acquire))
               ? g_pLocalConfig->graphics.streamlineDllPath.c_str()
               : ce::published_config::EarlyStreamlineDllPath();
}

std::string CanonicalOverridePath(const char* overrideDirectory, const char* fileName) {
    std::string path = BuildOverridePath(overrideDirectory ? overrideDirectory : "", fileName ? fileName : "");
    char canonical[MAX_PATH] = {};
    const DWORD length = ::GetFullPathNameA(path.c_str(), MAX_PATH, canonical, nullptr);
    if (length > 0 && length < MAX_PATH) {
        path.assign(canonical);
    }
    return path;
}

bool ModuleImportsDll(HMODULE module, const char* sourceModule) {
    if (!module || !sourceModule || !sourceModule[0]) {
        return false;
    }
    MODULEINFO modInfo = {};
    if (!::GetModuleInformation(::GetCurrentProcess(), module, &modInfo, sizeof(modInfo)) ||
        modInfo.lpBaseOfDll != module || modInfo.SizeOfImage < sizeof(IMAGE_DOS_HEADER)) {
        return false;
    }
    const auto* base = reinterpret_cast<const BYTE*>(module);
    const size_t imageSize = modInfo.SizeOfImage;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0 ||
        !IATHook::detail::ImageRangeContains(imageSize, static_cast<uintptr_t>(dos->e_lfanew),
                                             sizeof(IMAGE_NT_HEADERS))) {
        return false;
    }
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->FileHeader.SizeOfOptionalHeader < sizeof(IMAGE_OPTIONAL_HEADER) ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR_MAGIC ||
        nt->OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_IMPORT) {
        return false;
    }
    const auto& importDir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (importDir.VirtualAddress == 0 || importDir.Size < sizeof(IMAGE_IMPORT_DESCRIPTOR) ||
        !IATHook::detail::ImageRangeContains(imageSize, importDir.VirtualAddress, importDir.Size)) {
        return false;
    }
    const size_t count = importDir.Size / sizeof(IMAGE_IMPORT_DESCRIPTOR);
    const auto* descriptors = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + importDir.VirtualAddress);
    for (size_t i = 0; i < count; ++i) {
        if (descriptors[i].Name == 0) {
            break;
        }
        if (IATHook::detail::MatchImportModuleName(base, imageSize, descriptors[i].Name, sourceModule,
                                                   &IATHook::detail::IsCommittedReadableMemory) ==
            IATHook::detail::ImageStringMatch::Match) {
            return true;
        }
    }
    return false;
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
        [](HMODULE module, char* path, DWORD capacity) { return ::GetModuleFileNameA(module, path, capacity); });
}

void QueueResidentRollback(const char* reason) {
    g_TakeoverDeclinedFinal.store(true, std::memory_order_release);
    g_ResidentRollback.Request(reason);
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

uint64_t DispatchTakeoverSlot(size_t index, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4,
                              uint64_t a5) {
    void* target = g_ResolvedTargets[index].load(std::memory_order_acquire);
    if (!target) {
        if (!g_TakeoverActive.load(std::memory_order_acquire) && g_TakeoverArmed.load(std::memory_order_acquire) &&
            !g_TakeoverDecision.IsCustom()) {
            const CorePresence corePresence =
                IsTakeoverEntryExportIndex(index) ? CurrentStreamlineCorePresence() : CorePresence::Loaded;
            if (CanActivateTakeoverForExport(index, corePresence)) {
                struct ScopedEntryThunkActivation {
                    ScopedEntryThunkActivation() { tl_InEntryThunkActivation = true; }
                    ~ScopedEntryThunkActivation() { tl_InEntryThunkActivation = false; }
                } activationGuard;
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
        if (!target) {
            target = g_FallbackTargets[index].load(std::memory_order_acquire);
        }
    }
    if (!target) {
        return static_cast<uint64_t>(E_FAIL);
    }
    const uint32_t callIndex = g_ThunkCallCount.fetch_add(1, std::memory_order_relaxed) + 1;
    if (callIndex <= 8) {
        HookLogImportant("Streamline interposer takeover: forwarded early call #%u (%s) -> %p", callIndex,
                         kTakeoverExportNames[index], target);
    }
    return reinterpret_cast<PfnGenericInterposerExport>(target)(a0, a1, a2, a3, a4, a5);
}

template <size_t Index>
uint64_t WINAPI InterposerTakeoverThunk(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    return DispatchTakeoverSlot(Index, a0, a1, a2, a3, a4, a5);
}

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

    char residentPath[MAX_PATH] = {};
    const DWORD residentLen = ::GetModuleFileNameA(resident, residentPath, MAX_PATH);
    if (residentLen == 0 || residentLen >= MAX_PATH) {
        residentPath[0] = '\0';
    }

    const std::string overridePath = CanonicalOverridePath(streamlineDir, "sl.interposer.dll");
    const bool foreignResident =
        ce::graphics_runtime::IsForeignStreamlineInterposer(residentPath, overridePath.c_str());
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
        [](HMODULE module, char* path, DWORD capacity) { return ::GetModuleFileNameA(module, path, capacity); });
    const bool anyCoreLoaded = corePresence != CorePresence::Absent;
    const bool overrideExists =
        !overridePath.empty() && ::GetFileAttributesA(overridePath.c_str()) != INVALID_FILE_ATTRIBUTES;
    uint32_t residentMajor = ModuleFileMajorVersion(resident);
    if (residentMajor == 0 && !tl_InEarlyArm && residentPath[0]) {
        residentMajor = DllFileMajorVersion(residentPath);
    }
    const uint32_t overrideMajor = overrideExists ? DllFileMajorVersion(overridePath.c_str()) : 0u;

    if (!ce::graphics_runtime::CanTakeOverForeignStreamlineInterposer(
            true, foreignResident, anyCoreLoaded, overrideExists, residentMajor, overrideMajor)) {
        g_TakeoverDeclinedFinal.store(true, std::memory_order_release);
        HookLogImportant(
            "Streamline interposer takeover declined (%s): resident=%s (v%u.x) override=%s (exists=%d v%u.x) "
            "coreAlreadyLoaded=%d coreScan=%s",
            phase ? phase : "unspecified", residentPath[0] ? residentPath : "unresolved", residentMajor,
            overridePath.c_str(), overrideExists ? 1 : 0, overrideMajor, anyCoreLoaded ? 1 : 0,
            corePresence == CorePresence::Unresolved ? "unresolved" : "complete");
        return false;
    }

    std::lock_guard<std::mutex> lock(g_TakeoverMutex);
    if (g_TakeoverArmed.load(std::memory_order_acquire)) {
        return true;
    }

    strncpy_s(g_SupersededInterposerPath, sizeof(g_SupersededInterposerPath), residentPath, _TRUNCATE);
    strncpy_s(g_OverrideInterposerPath, sizeof(g_OverrideInterposerPath), overridePath.c_str(), _TRUNCATE);
    g_SupersededInterposerModule.store(resident, std::memory_order_release);

    for (size_t i = 0; i < kTakeoverExportCount; ++i) {
        void* proc = reinterpret_cast<void*>(::GetProcAddress(resident, kTakeoverExportNames[i]));
        g_FallbackTargets[i].store(proc, std::memory_order_release);
    }

    g_TakeoverArmed.store(true, std::memory_order_release);

    const std::vector<HMODULE> importingModules = CollectInterposerImportingModules(&enumeratedModules);
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
        "from %s -> %s (Streamline %u.x)",
        phase ? phase : "unspecified", patchedSlots, importingModules.size(), g_SupersededInterposerPath,
        g_OverrideInterposerPath, residentMajor);
    return true;
}

bool ShouldRedirectModuleHandleQuery(const char* nameOrPath) {
    if (!IsTakeoverArmed() || !nameOrPath || !nameOrPath[0]) {
        return false;
    }
    return ce::graphics_runtime::IsStreamlineInterposerModuleQuery(nameOrPath);
}

bool ShouldRedirectModuleHandleQuery(const wchar_t* nameOrPath) {
    if (!IsTakeoverArmed() || !nameOrPath || !nameOrPath[0]) {
        return false;
    }
    return ce::graphics_runtime::IsStreamlineInterposerModuleQuery(nameOrPath);
}

HMODULE ResolveCustomInterposerForModuleQuery() {
    // GetModuleHandle/GetProcAddress are legal from DLL callbacks. They must
    // neither start a load nor wait for a thread that may need the loader lock.
    return g_RuntimePublication.Query();
}

}  // namespace

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

    const int wideLen = ::MultiByteToWideChar(CP_UTF8, 0, g_OverrideInterposerPath, -1, nullptr, 0);
    if (wideLen <= 0) {
        return false;
    }
    std::wstring widePath(static_cast<size_t>(wideLen), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, g_OverrideInterposerPath, -1, widePath.data(), wideLen);
    if (!widePath.empty() && widePath.back() == L'\0') {
        widePath.pop_back();
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

    PatchLoadLibraryIatForLateLoadedModule(customModule, g_OverrideInterposerPath);
    PatchModuleHandleIatForModule(customModule);
    PatchProcessCreationIatForLateLoadedModule(customModule, g_OverrideInterposerPath);

    g_TakeoverActive.store(true, std::memory_order_release);

    HookLogImportant(
        "Streamline interposer takeover ACTIVE: loaded and pinned %s (%p), superseding dormant %s (%p) and rebound "
        "%zu sl.interposer.dll import slot(s) across %zu module(s)",
        g_OverrideInterposerPath, customModule, g_SupersededInterposerPath, superseded, reboundSlots,
        importingModules.size());
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

void InstallModuleHandleHooksForAllModules() {
    void* dummy = nullptr;
    IATHook::PatchIATAllModules("kernel32.dll", "GetModuleHandleA", reinterpret_cast<void*>(&HookedGetModuleHandleA),
                                &dummy);
    IATHook::PatchIATAllModules("kernel32.dll", "GetModuleHandleW", reinterpret_cast<void*>(&HookedGetModuleHandleW),
                                &dummy);
    IATHook::PatchIATAllModules("kernel32.dll", "GetModuleHandleExA",
                                reinterpret_cast<void*>(&HookedGetModuleHandleExA), &dummy);
    IATHook::PatchIATAllModules("kernel32.dll", "GetModuleHandleExW",
                                reinterpret_cast<void*>(&HookedGetModuleHandleExW), &dummy);
}

void PatchModuleHandleIatForModule(HMODULE module) {
    if (!module) {
        return;
    }
    void* dummy = nullptr;
    IATHook::PatchIAT(module, "kernel32.dll", "GetModuleHandleA", reinterpret_cast<void*>(&HookedGetModuleHandleA),
                      &dummy);
    IATHook::PatchIAT(module, "kernel32.dll", "GetModuleHandleW", reinterpret_cast<void*>(&HookedGetModuleHandleW),
                      &dummy);
    IATHook::PatchIAT(module, "kernel32.dll", "GetModuleHandleExA", reinterpret_cast<void*>(&HookedGetModuleHandleExA),
                      &dummy);
    IATHook::PatchIAT(module, "kernel32.dll", "GetModuleHandleExW", reinterpret_cast<void*>(&HookedGetModuleHandleExW),
                      &dummy);
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

HMODULE WINAPI HookedGetModuleHandleA(LPCSTR lpModuleName) {
    if (!IsProcessTerminating() && !HookIsShuttingDown() && ShouldRedirectModuleHandleQuery(lpModuleName)) {
        if (HMODULE custom = ResolveCustomInterposerForModuleQuery()) {
            return custom;
        }
    }
    return ::GetModuleHandleA(lpModuleName);
}

HMODULE WINAPI HookedGetModuleHandleW(LPCWSTR lpModuleName) {
    if (!IsProcessTerminating() && !HookIsShuttingDown() && ShouldRedirectModuleHandleQuery(lpModuleName)) {
        if (HMODULE custom = ResolveCustomInterposerForModuleQuery()) {
            return custom;
        }
    }
    return ::GetModuleHandleW(lpModuleName);
}

BOOL WINAPI HookedGetModuleHandleExA(DWORD dwFlags, LPCSTR lpModuleName, HMODULE* phModule) {
    if (!IsProcessTerminating() && !HookIsShuttingDown() &&
        (dwFlags & GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS) == 0 && ShouldRedirectModuleHandleQuery(lpModuleName)) {
        if (HMODULE custom = ResolveCustomInterposerForModuleQuery()) {
            return ::GetModuleHandleExW(dwFlags | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                                        reinterpret_cast<LPCWSTR>(custom), phModule);
        }
    }
    return ::GetModuleHandleExA(dwFlags, lpModuleName, phModule);
}

BOOL WINAPI HookedGetModuleHandleExW(DWORD dwFlags, LPCWSTR lpModuleName, HMODULE* phModule) {
    if (!IsProcessTerminating() && !HookIsShuttingDown() &&
        (dwFlags & GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS) == 0 && ShouldRedirectModuleHandleQuery(lpModuleName)) {
        if (HMODULE custom = ResolveCustomInterposerForModuleQuery()) {
            return ::GetModuleHandleExW(dwFlags | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                                        reinterpret_cast<LPCWSTR>(custom), phModule);
        }
    }
    return ::GetModuleHandleExW(dwFlags, lpModuleName, phModule);
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
void InstallModuleHandleHooksForAllModules() {}
void PatchModuleHandleIatForModule(HMODULE) {}
void PatchInterposerIatForLateLoadedModule(HMODULE, const char*) {}
HMODULE WINAPI HookedGetModuleHandleA(LPCSTR lpModuleName) { return ::GetModuleHandleA(lpModuleName); }
HMODULE WINAPI HookedGetModuleHandleW(LPCWSTR lpModuleName) { return ::GetModuleHandleW(lpModuleName); }
BOOL WINAPI HookedGetModuleHandleExA(DWORD dwFlags, LPCSTR lpModuleName, HMODULE* phModule) {
    return ::GetModuleHandleExA(dwFlags, lpModuleName, phModule);
}
BOOL WINAPI HookedGetModuleHandleExW(DWORD dwFlags, LPCWSTR lpModuleName, HMODULE* phModule) {
    return ::GetModuleHandleExW(dwFlags, lpModuleName, phModule);
}

#endif

}  // namespace ce::streamline_interposer_takeover
