#include "module_replacement.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

#include "common/logging/log_meter.h"
#include "common/platform/loaded_pe_image.h"
#include "common/platform/module_enumeration.h"
#include "hook/hooking/iat_hook.h"
#include "hook/overlay/overlay_compat_detail/module_address_cache.h"
#include "hook/runtime/hook_common.h"
#include "hook/runtime/module_replacement_internal.h"
#include "hook/runtime/module_replacement_policy.h"
#include "hook/runtime/third_party_load_policy.h"

namespace ce::module_replacement {

namespace {

constexpr size_t kMaxBaseName = 64;
constexpr DWORD kPathCapacity = 1024;

struct Replacement {
    char baseName[kMaxBaseName] = {};  // ASCII: every runtime this applies to has an ASCII name
    HMODULE module = nullptr;
    ImageRange range;
    std::wstring path;
    HMODULE superseded = nullptr;
    ImageRange supersededRange;
    std::wstring supersededPath;
    // The system-directory spelling of a SysWOW64 image ("<GetSystemDirectory>\dxgi.dll" is what
    // a 32-bit caller builds), empty otherwise.
    std::wstring supersededAlias;
};

// Published once and never freed: loader-lock readers may hold it for the process lifetime.
struct Snapshot {
    std::vector<Replacement> replacements;
    std::vector<HMODULE> customModules;  // every configured custom DLL, replacement or not
};

std::atomic<const Snapshot*> g_snapshot{nullptr};

// Importers seen before the loader had bound their slots (a loader notification can precede
// binding); serviced once a load has returned. A leaf lock: nothing is called while holding it.
std::mutex g_pendingLock;
std::vector<HMODULE> g_pendingModules;
std::atomic<bool> g_pendingNonEmpty{false};

void QueuePending(HMODULE module) {
    std::lock_guard<std::mutex> lock(g_pendingLock);
    for (HMODULE queued : g_pendingModules) {
        if (queued == module) {
            return;
        }
    }
    g_pendingModules.push_back(module);
    g_pendingNonEmpty.store(true, std::memory_order_release);
}

std::vector<HMODULE> TakePending() {
    std::lock_guard<std::mutex> lock(g_pendingLock);
    g_pendingNonEmpty.store(false, std::memory_order_release);
    return std::exchange(g_pendingModules, {});
}

const Snapshot* Current() {
    return g_snapshot.load(std::memory_order_acquire);
}

ImageRange RangeOf(HMODULE module) {
    const auto image = ce::pe::LoadedImage::Open(module);
    return image ? ImageRange{reinterpret_cast<uintptr_t>(image->base), image->size} : ImageRange{};
}

std::wstring ModulePathW(HMODULE module) {
    std::wstring path(kPathCapacity, L'\0');
    const DWORD length = GetModuleFileNameW(module, path.data(), kPathCapacity);
    if (length == 0 || length >= kPathCapacity) {
        return {};
    }
    path.resize(length);
    return path;
}

std::string Utf8(const std::wstring& wide) {
    if (wide.empty()) {
        return {};
    }
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (bytes <= 1) {
        return {};
    }
    std::string text(static_cast<size_t>(bytes - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, text.data(), bytes, nullptr, nullptr);
    return text;
}

std::wstring DirectoryOf(const std::wstring& path) {
    const size_t separator = path.find_last_of(L"\\/");
    return separator == std::wstring::npos ? std::wstring() : path.substr(0, separator);
}

std::wstring SystemDirectoryAlias(const std::wstring& supersededPath, const wchar_t* baseName) {
    wchar_t wow64[MAX_PATH] = {};
    wchar_t system[MAX_PATH] = {};
    const UINT wow64Length = GetSystemWow64DirectoryW(wow64, MAX_PATH);
    const UINT systemLength = GetSystemDirectoryW(system, MAX_PATH);
    if (wow64Length == 0 || wow64Length >= MAX_PATH || systemLength == 0 || systemLength >= MAX_PATH ||
        !ce::third_party_load::AreModulePathsEquivalent(DirectoryOf(supersededPath).c_str(), wow64)) {
        return {};
    }
    return std::wstring(system) + L"\\" + baseName;
}

bool CopyAsciiBaseName(const wchar_t* wide, char (&out)[kMaxBaseName]) {
    size_t i = 0;
    for (; wide[i]; ++i) {
        if (i + 1 >= kMaxBaseName || wide[i] > 0x7E || wide[i] < 0x20) {
            return false;
        }
        out[i] = static_cast<char>(wide[i]);
    }
    out[i] = '\0';
    return i != 0;
}

bool IsCustomModule(const Snapshot& snapshot, HMODULE module) {
    for (HMODULE custom : snapshot.customModules) {
        if (custom == module) {
            return true;
        }
    }
    return false;
}

RequesterScope ScopeOfCaller(const Snapshot& snapshot, const void* callerAddress) {
    HMODULE caller = nullptr;
    char path[MAX_PATH] = {};
    if (!callerAddress || !ce::overlay_compat::module_address_cache::Lookup(
                              callerAddress, &caller, path, sizeof(path), nullptr)) {
        return ScopeFor(Requester::kApplication, false);
    }
    bool nativeRuntime = false;
    for (const Replacement& replacement : snapshot.replacements) {
        nativeRuntime = nativeRuntime || replacement.superseded == caller ||
                        IsRuntimeComponentOf(replacement.baseName, PathBaseName(path));
    }
    return ScopeFor(ClassifyRequesterPath(path), IsCustomModule(snapshot, caller), nativeRuntime);
}

bool NamesSupersededImage(const Replacement& replacement, const wchar_t* request) {
    wchar_t full[kPathCapacity];
    const DWORD length = GetFullPathNameW(request, kPathCapacity, full, nullptr);
    const wchar_t* normalized = (length != 0 && length < kPathCapacity) ? full : request;
    return ce::third_party_load::AreModulePathsEquivalent(normalized, replacement.supersededPath.c_str()) ||
           (!replacement.supersededAlias.empty() &&
            ce::third_party_load::AreModulePathsEquivalent(normalized, replacement.supersededAlias.c_str()));
}

// The replacement a request from `callerAddress` resolves to: a bare name for every application
// requester, the superseded image's own path only at full scope (see RequesterScope).
const Replacement* ResolveRequest(const wchar_t* request, const void* callerAddress) {
    const Snapshot* snapshot = Current();
    if (!snapshot || !request || !*request) {
        return nullptr;
    }
    const wchar_t* base = PathBaseName(request);
    for (const Replacement& replacement : snapshot->replacements) {
        if (!BareNameMatchesBaseName(base, replacement.baseName)) {
            continue;
        }
        const RequesterScope scope = ScopeOfCaller(*snapshot, callerAddress);
        if (scope == RequesterScope::kNone) {
            return nullptr;
        }
        if (base == request) {
            return &replacement;
        }
        return scope == RequesterScope::kFull && NamesSupersededImage(replacement, request) ? &replacement : nullptr;
    }
    return nullptr;
}

void LogRedirect(const char* kind, const Replacement& replacement, const void* callerAddress) {
    static std::atomic<uint32_t> s_redirects{0};
    const uint32_t index = s_redirects.fetch_add(1, std::memory_order_relaxed);
    if (index < 16 || (index % 1000) == 0) {
        HMODULE caller = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<LPCWSTR>(callerAddress), &caller);
        const std::wstring callerPath = caller ? ModulePathW(caller) : std::wstring();
        HookLog("ModuleReplacement: %s for %s from %s -> replacement %p (redirect #%u)", kind, replacement.baseName,
                callerPath.empty() ? "unattributed code" : Utf8(PathBaseName(callerPath.c_str())).c_str(),
                static_cast<void*>(replacement.module), index + 1);
    }
}

struct RebindTally {
    size_t modules = 0;
    size_t slots = 0;
    size_t keptForeign = 0;
    size_t missing = 0;
    size_t unnamed = 0;
    size_t notYetBound = 0;
    size_t failedTransactions = 0;
    const char* firstMissing = nullptr;  // an import name inside a mapped image
};

// Moves every planned slot in one transaction; nothing to move counts as done.
bool CommitPlan(const ImportRebindPlan& plan) {
    if (plan.changes.empty()) {
        return true;
    }
    std::vector<IATHook::BaselineChange> changes;
    changes.reserve(plan.changes.size());
    for (const SlotChange& change : plan.changes) {
        changes.push_back({change.entry, change.expected, change.replacement});
    }
    return IATHook::RebindIATBaselinesTransaction(changes);
}

size_t RebindModuleImports(const Snapshot& snapshot, HMODULE module, RebindTally* tally) {
    const std::wstring path = module ? ModulePathW(module) : std::wstring();
    if (path.empty()) {
        return 0;
    }
    const wchar_t* base = PathBaseName(path.c_str());
    ImporterFacts facts;
    facts.requester = ClassifyRequesterPath(path.c_str());
    facts.isConfiguredCustomDll = IsCustomModule(snapshot, module);
    for (const Replacement& replacement : snapshot.replacements) {
        facts.isSuperseded = facts.isSuperseded || replacement.superseded == module;
        facts.belongsToReplacedRuntime = facts.belongsToReplacedRuntime ||
                                         BareNameMatchesBaseName(base, replacement.baseName) ||
                                         IsRuntimeComponentOf(replacement.baseName, base);
    }
    if (!ShouldRebindImporter(facts)) {
        return 0;
    }
    const auto image = ce::pe::LoadedImage::Open(module);
    if (!image || image->headers->OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_IMPORT) {
        return 0;
    }
    const auto& directory = image->headers->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (directory.VirtualAddress == 0 || directory.Size < sizeof(IMAGE_IMPORT_DESCRIPTOR) ||
        !image->Contains(directory.VirtualAddress, directory.Size)) {
        return 0;
    }
    size_t rebound = 0;
    for (const Replacement& replacement : snapshot.replacements) {
        const auto plan = [&] {
            return PlanImportRebind(
                image->base, image->size, directory.VirtualAddress, directory.Size, replacement.baseName,
                replacement.supersededRange, replacement.range,
                [&](const char* name) { return reinterpret_cast<void*>(GetProcAddress(replacement.module, name)); },
                &IATHook::detail::IsCommittedReadableMemory);
        };
        // The adoption sweep and a loader notification on another thread can rebind one module at
        // once; the loser's transaction finds its slots moved, and its second plan sees why.
        ImportRebindPlan current = plan();
        bool committed = CommitPlan(current);
        if (!committed) {
            current = plan();
            committed = CommitPlan(current);
        }
        if (committed) {
            rebound += current.changes.size();
        } else {
            ++tally->failedTransactions;
        }
        tally->keptForeign += current.keptForeign;
        tally->unnamed += current.unnamed;
        tally->notYetBound += current.notYetBound;
        tally->missing += current.missingExport;
        if (!tally->firstMissing && current.firstMissing) {
            tally->firstMissing = current.firstMissing;
        }
    }
    if (rebound != 0) {
        ++tally->modules;
        tally->slots += rebound;
    }
    return rebound;
}

RebindTally SweepLoadedModules(const Snapshot& snapshot) {
    RebindTally tally;
    std::vector<HMODULE> modules;
    if (ce::EnumerateProcessModules(GetCurrentProcess(), modules)) {
        for (HMODULE module : modules) {
            RebindModuleImports(snapshot, module, &tally);
        }
    }
    return tally;
}

void LogLateRebind(HMODULE module, size_t rebound, const RebindTally& tally) {
    static ce::log_meter::ChangeGate s_gate;
    const auto verdict = s_gate.Observe(ce::log_meter::FieldKey(module, rebound, tally.missing, tally.notYetBound));
    if (!verdict) {
        return;
    }
    const std::wstring path = ModulePathW(module);
    HookLogImportant("ModuleReplacement: rebound %zu import slot(s) of late-loaded %s (kept foreign=%zu missing "
                     "exports=%zu first=%s unnamed=%zu not yet bound=%zu failed transactions=%zu)%s",
                     rebound, Utf8(PathBaseName(path.c_str())).c_str(), tally.keptForeign, tally.missing,
                     tally.firstMissing ? tally.firstMissing : "-", tally.unnamed, tally.notYetBound,
                     tally.failedTransactions, ce::log_meter::SuppressedNote(verdict.suppressed).c_str());
}

}  // namespace

void AdoptCustomModules(const std::vector<CustomModule>& custom) {
    static std::atomic<bool> s_adopted{false};
    if (custom.empty() || s_adopted.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    auto snapshot = std::make_unique<Snapshot>();
    for (const CustomModule& entry : custom) {
        if (entry.module) {
            snapshot->customModules.push_back(entry.module);
        }
    }
    for (const CustomModule& entry : custom) {
        if (!entry.module) {
            continue;
        }
        const wchar_t* baseName = PathBaseName(entry.path.c_str());
        // What a by-name lookup answers right now. When that is the custom DLL itself, it was the
        // first image of its name and already resolves like an application-local copy.
        const HMODULE resident = GetModuleHandleW(baseName);
        if (!resident || resident == entry.module) {
            continue;
        }
        Replacement replacement;
        const std::string pathUtf8 = Utf8(entry.path);
        if (!CopyAsciiBaseName(baseName, replacement.baseName)) {
            HookLogImportant("ModuleReplacement: %s shares its name with a resident module but the name is not plain "
                             "ASCII; not replacing it",
                             pathUtf8.c_str());
            continue;
        }
        bool duplicate = false;
        for (const Replacement& existing : snapshot->replacements) {
            duplicate = duplicate || BareNameMatchesBaseName(baseName, existing.baseName);
        }
        if (duplicate) {
            HookLogImportant("ModuleReplacement: %s names %s a second time in custom_dll_paths; the first entry keeps "
                             "the replacement",
                             pathUtf8.c_str(), replacement.baseName);
            continue;
        }
        replacement.module = entry.module;
        replacement.range = RangeOf(entry.module);
        replacement.path = entry.path;
        replacement.superseded = resident;
        replacement.supersededRange = RangeOf(resident);
        replacement.supersededPath = ModulePathW(resident);
        replacement.supersededAlias = SystemDirectoryAlias(replacement.supersededPath, baseName);
        if (!replacement.range.base || !replacement.supersededRange.base || replacement.supersededPath.empty()) {
            HookLogImportant("ModuleReplacement: %s could not read its own or the resident %s image; not replacing it",
                             pathUtf8.c_str(), replacement.baseName);
            continue;
        }
        snapshot->replacements.push_back(std::move(replacement));
    }
    if (snapshot->replacements.empty()) {
        return;
    }

    // CE's wrappers forward to originals captured from the resident image in DllMain. They move
    // first, so a call that reaches a wrapper through an already rebound slot never lands on the
    // superseded runtime.
    for (const Replacement& replacement : snapshot->replacements) {
        const IATHook::OriginalRetargetResult retarget =
            IATHook::RetargetOriginals(replacement.superseded, replacement.module);
        HookLogImportant("ModuleReplacement: CE forwarding originals for %s moved to the replacement (dynamic=%zu "
                         "tracked slots=%zu missing=%zu first=%s)",
                         replacement.baseName, retarget.dynamicHooks, retarget.trackedSlots, retarget.missing,
                         retarget.firstMissing.empty() ? "-" : retarget.firstMissing.c_str());
    }

    const Snapshot* published = snapshot.release();
    g_snapshot.store(published, std::memory_order_release);
    const RebindTally tally = SweepLoadedModules(*published);
    for (const Replacement& replacement : published->replacements) {
        HookLogImportant("ModuleReplacement: %s now resolves to %s for application code instead of %s (Windows "
                         "modules and CE keep the system image)",
                         replacement.baseName, Utf8(replacement.path).c_str(),
                         Utf8(replacement.supersededPath).c_str());
    }
    HookLogImportant("ModuleReplacement: initial import sweep rebound %zu slot(s) in %zu module(s) (kept foreign=%zu "
                     "missing exports=%zu first=%s unnamed=%zu not yet bound=%zu failed transactions=%zu)",
                     tally.slots, tally.modules, tally.keptForeign, tally.missing,
                     tally.firstMissing ? tally.firstMissing : "-", tally.unnamed, tally.notYetBound,
                     tally.failedTransactions);
    // Addresses resolved before CE was injected - a protected executable's own import table, a
    // cached GetProcAddress result - still lead to the superseded image's entry points.
    for (const Replacement& replacement : published->replacements) {
        const detail::EntryForwardTally forward =
            detail::InstallEntryForwarders(replacement.baseName, replacement.superseded, replacement.module);
        HookLogImportant("ModuleReplacement: %zu entry point(s) of the superseded %s forward application calls to the "
                         "replacement (left to another owner=%zu native aliases=%zu missing in replacement=%zu "
                         "first=%s failed=%zu)",
                         forward.installed, replacement.baseName, forward.leftToForeignOwner,
                         forward.nativeAliases, forward.missingInReplacement,
                         forward.firstMissing ? forward.firstMissing : "-",
                         forward.failed);
    }
}

namespace detail {

bool ForwardsEntryCallFrom(const void* callerAddress) {
    const Snapshot* snapshot = Current();
    // Adoption changes the application's runtime for its remaining lifetime. A dormant CE host
    // must not switch cached entries back to native while rebound imports still use the replacement.
    return snapshot && !IsProcessTerminating() &&
           ShouldForwardEntryCall(ScopeOfCaller(*snapshot, callerAddress));
}

}  // namespace detail

bool IsActive() {
    return Current() != nullptr;
}

void RebindLoadedModule(HMODULE module) {
    const Snapshot* snapshot = Current();
    if (!snapshot || !module) {
        return;
    }
    RebindTally tally;
    const size_t rebound = RebindModuleImports(*snapshot, module, &tally);
    if (tally.notYetBound != 0) {
        QueuePending(module);
    }
    if (rebound != 0 || tally.missing != 0) {
        LogLateRebind(module, rebound, tally);
    }
}

void ServicePendingRebinds() {
    if (!Current() || !g_pendingNonEmpty.load(std::memory_order_acquire)) {
        return;
    }
    // A slot still unbound now belongs to a load in flight on another thread; it queues again.
    for (HMODULE module : TakePending()) {
        RebindLoadedModule(module);
    }
}

void NoteLoadReturned(HMODULE module) {
    if (!Current()) {
        return;
    }
    // The notification may have preceded import binding for this module or a dependency mapped
    // with it; once the load has returned their slots are bound.
    RebindLoadedModule(module);
    ServicePendingRebinds();
}

HMODULE ReplacementForModuleQuery(const wchar_t* nameOrPath, const void* callerAddress) {
    const Replacement* replacement = ResolveRequest(nameOrPath, callerAddress);
    if (!replacement) {
        return nullptr;
    }
    LogRedirect("module query", *replacement, callerAddress);
    return replacement->module;
}

HMODULE ReplacementForModuleQuery(const char* nameOrPath, const void* callerAddress) {
    if (!Current() || !nameOrPath || !*nameOrPath) {
        return nullptr;
    }
    // Module queries are ANSI-code-page strings; the cheap name filter in ResolveRequest runs
    // after the conversion, so only active replacements pay for it.
    wchar_t wide[kPathCapacity];
    if (MultiByteToWideChar(CP_ACP, 0, nameOrPath, -1, wide, kPathCapacity) <= 0) {
        return nullptr;
    }
    return ReplacementForModuleQuery(wide, callerAddress);
}

const wchar_t* ReplacementPathForLoad(const wchar_t* request, const void* callerAddress) {
    const Replacement* replacement = ResolveRequest(request, callerAddress);
    if (!replacement) {
        return nullptr;
    }
    LogRedirect("LoadLibrary", *replacement, callerAddress);
    return replacement->path.c_str();
}

HMODULE ModuleForProcLookup(HMODULE requested, const void* callerAddress) {
    const Snapshot* snapshot = Current();
    if (!snapshot || !requested) {
        return requested;
    }
    for (const Replacement& replacement : snapshot->replacements) {
        if (replacement.superseded != requested) {
            continue;
        }
        if (ScopeOfCaller(*snapshot, callerAddress) != RequesterScope::kFull) {
            return requested;
        }
        LogRedirect("GetProcAddress on the superseded image", replacement, callerAddress);
        return replacement.module;
    }
    return requested;
}

HMODULE ApplicationModuleByName(const char* baseName) {
    if (const Snapshot* snapshot = Current()) {
        for (const Replacement& replacement : snapshot->replacements) {
            if (BareNameMatchesBaseName(baseName, replacement.baseName)) {
                return replacement.module;
            }
        }
    }
    return baseName ? GetModuleHandleA(baseName) : nullptr;
}

}  // namespace ce::module_replacement
