#include "present_origin.h"

#include "dxgi_shared_internal.h"
#include "common/logging/log_meter.h"
#include "common/platform/module_enumeration.h"
#include "hook/fg/fg_presenter.h"
#include "hook/present/present_stage_cost.h"
#include "hook/runtime/hook_clock.h"
#include "hook/runtime/runtime_override_paths.h"
#include "hook/streamline/streamline_hook.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace DXGIShared {
namespace {

using ce::present_originator::FrameKind;
using ce::present_originator::Originator;

// CE's own module, resolved from a CE code address so this unit carries no link dependency on the injected DLL's
// entry point.
HMODULE CaptureEngineModuleHandle() {
    static HMODULE s_module = [] {
        HMODULE module = nullptr;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(&CaptureEngineModuleHandle), &module);
        return module;
    }();
    return s_module;
}

const char* SystemDirectory() {
    static char s_directory[MAX_PATH] = {};
    static std::once_flag s_once;
    std::call_once(s_once, [] {
        const UINT length = GetSystemDirectoryA(s_directory, MAX_PATH);
        if (length == 0 || length >= MAX_PATH) {
            s_directory[0] = '\0';
        }
    });
    return s_directory;
}

void RecordWalkedModule(PresentOrigin* out, const char* modulePath) {
    const char* baseName = ce::overlay_compat::ExtractModuleBaseName(modulePath);
    if (!baseName || !baseName[0]) {
        return;
    }
    if (out->moduleCount > 0 &&
        ce::overlay_compat::detail::EqualsInsensitive(out->modules[out->moduleCount - 1], baseName)) {
        return;
    }
    if (out->moduleCount >= PresentOrigin::kMaxModules) {
        return;
    }
    strncpy_s(out->modules[out->moduleCount], PresentOrigin::kModuleNameLength, baseName, _TRUNCATE);
    ++out->moduleCount;
}

void SetOriginator(PresentOrigin* out, const char* modulePath, Originator originator) {
    out->originator = originator;
    strncpy_s(out->module, sizeof(out->module), modulePath, _TRUNCATE);
    out->fromStreamline = out->originator == Originator::kStreamline;
    out->fromFFXFrameGeneration = out->originator == Originator::kFFXFrameGeneration;
    out->fromXeFG = out->originator == Originator::kXeFG;
}

constexpr size_t kOriginatorKinds = static_cast<size_t>(Originator::kPresentInterposer) + 1;

struct OriginSummary {
    std::atomic<uint64_t> windowStartMs{0};
    std::atomic<uint64_t> presents{0};
    std::atomic<uint64_t> byOriginator[kOriginatorKinds] = {};
};

OriginSummary& Summary() {
    static OriginSummary summary;
    return summary;
}

ce::log_meter::StreamChangeGate<64>& OriginGates() {
    static ce::log_meter::StreamChangeGate<64> gates;
    return gates;
}

void FormatChain(const PresentOrigin& origin, char* text, size_t textSize) {
    text[0] = '\0';
    size_t used = 0;
    for (uint8_t i = 0; i < origin.moduleCount && used + 1 < textSize; ++i) {
        const int written = std::snprintf(text + used, textSize - used, "%s%s", i == 0 ? "" : ">", origin.modules[i]);
        if (written < 0) {
            break;
        }
        used += static_cast<size_t>(written);
    }
}

void MaybeLogSummary(uint64_t nowMs) {
    constexpr uint64_t kWindowMs = 10000;
    OriginSummary& summary = Summary();
    uint64_t start = summary.windowStartMs.load(std::memory_order_acquire);
    if (start == 0) {
        summary.windowStartMs.compare_exchange_strong(start, nowMs, std::memory_order_acq_rel);
        return;
    }
    if (nowMs - start < kWindowMs ||
        !summary.windowStartMs.compare_exchange_strong(start, nowMs, std::memory_order_acq_rel)) {
        return;
    }
    uint64_t counts[kOriginatorKinds] = {};
    for (size_t i = 0; i < kOriginatorKinds; ++i) {
        counts[i] = summary.byOriginator[i].exchange(0, std::memory_order_acq_rel);
    }
    const uint64_t presents = summary.presents.exchange(0, std::memory_order_acq_rel);
    HookLogImportant(
        "[PRESENT ORIGIN] summary %llums: presents=%llu application=%llu streamline=%llu ffx=%llu xefg=%llu "
        "interposer=%llu unresolved=%llu",
        static_cast<unsigned long long>(nowMs - start), static_cast<unsigned long long>(presents),
        static_cast<unsigned long long>(counts[static_cast<size_t>(Originator::kApplication)]),
        static_cast<unsigned long long>(counts[static_cast<size_t>(Originator::kStreamline)]),
        static_cast<unsigned long long>(counts[static_cast<size_t>(Originator::kFFXFrameGeneration)]),
        static_cast<unsigned long long>(counts[static_cast<size_t>(Originator::kXeFG)]),
        static_cast<unsigned long long>(counts[static_cast<size_t>(Originator::kPresentInterposer)]),
        static_cast<unsigned long long>(counts[static_cast<size_t>(Originator::kUnresolved)]));
}

// What a stack frame's module is. Return addresses repeat from Present to Present, so each is classified once: the
// path copy, the token scans and above all the loader miss on a trampoline frame are all per module, not per frame.
// Base names only: a full path per entry takes the hook DLL past its thread-local budget, and only the one frame that
// decides needs its path, which WalkPresentOriginator looks up.
struct FrameVerdict {
    FrameKind kind = FrameKind::kNoImage;
    Originator originator = Originator::kUnresolved;
    char baseName[PresentOrigin::kModuleNameLength] = {};
};

const FrameVerdict& ClassifyWalkedFrame(const void* address, bool moduleSetObservable, uint64_t moduleSet) {
    thread_local ce::overlay_compat::CallerVerdictCache<FrameVerdict, 16> t_frames;
    return t_frames.Get(address, moduleSet, ce::overlay_compat::IdentifiedOverlayPathRefreshSequence().load(std::memory_order_acquire),
                        moduleSetObservable && address != nullptr, [address](FrameVerdict& fresh) {
        fresh = FrameVerdict{};
        HMODULE module = nullptr;
        char path[MAX_PATH] = {};
        // No module is an answer too (a trampoline or thunk), and it stands until a module loads.
        if (!TryGetModulePathFromCodeAddress(address, path, sizeof(path), &module) || !module) {
            return true;
        }
        fresh.kind = ce::present_originator::ClassifyFrame(path, module == CaptureEngineModuleHandle(), SystemDirectory());
        strncpy_s(fresh.baseName, sizeof(fresh.baseName), ce::overlay_compat::ExtractModuleBaseName(path), _TRUNCATE);
        if (fresh.kind == FrameKind::kOther) {
            fresh.originator = ce::present_originator::ClassifyOriginatorModule(path, IsStreamlineModuleHandle(module));
        }
        return true;
    });
}

// One pass over the frames: the first candidate decides, and frames above it are its own callers. Most stacks resolve
// within the first batch, and an x64 capture pays for every frame it unwinds.
void WalkPresentOriginator(PresentOrigin* out, bool moduleSetObservable, uint64_t moduleSet) {
    *out = PresentOrigin{};
    out->walked = true;
    constexpr USHORT kBatchFrames = 8;
    constexpr USHORT kBatches = 2;
    for (USHORT batch = 0; batch < kBatches; ++batch) {
        void* stackFrames[kBatchFrames] = {};
        const USHORT frameCount =
            CaptureStackBackTrace(2u + static_cast<DWORD>(batch) * kBatchFrames, kBatchFrames, stackFrames, nullptr);
        for (USHORT i = 0; i < frameCount; ++i) {
            const FrameVerdict& frame = ClassifyWalkedFrame(stackFrames[i], moduleSetObservable, moduleSet);
            if (frame.kind == FrameKind::kNoImage || frame.kind == FrameKind::kCaptureEngine) {
                continue;  // A trampoline or thunk, or CE's own wrapper/detour frames.
            }
            RecordWalkedModule(out, frame.baseName);
            if (frame.kind != FrameKind::kOther) {
                continue;  // Steam / RTSS (the chain CE sits below) and the system's dispatch frames between them.
            }
            char path[MAX_PATH] = {};
            TryGetModulePathFromCodeAddress(stackFrames[i], path, sizeof(path));
            SetOriginator(out, path, frame.originator);
            out->originatorFrame = stackFrames[i];
            return;
        }
        if (frameCount < kBatchFrames) {
            return;
        }
    }
}

}  // namespace

const PresentOrigin& ResolvePresentOriginatorBelowForeignChain() {
    thread_local PresentOrigin t_origin;
    thread_local ce::present_originator::OriginatorWalkMemo t_memo;
    namespace address_cache = ce::overlay_compat::module_address_cache;
    const bool moduleSetObservable = address_cache::IsEnabled();
    const uint64_t moduleSet = moduleSetObservable ? address_cache::ModuleSetGeneration() : 0;
    const uint64_t overlayIdentityGeneration =
        ce::overlay_compat::IdentifiedOverlayPathRefreshSequence().load(std::memory_order_acquire);
    if (!t_memo.ShouldWalk(moduleSetObservable && ce::present_originator::PresentRuntimeModules().OnlyApplicationCanOriginate(),
                           moduleSet, overlayIdentityGeneration)) {
        return t_origin;
    }
    ce::present_stage_cost::StageScope walkStage(ce::present_stage_cost::Stage::kOriginWalk);
    WalkPresentOriginator(&t_origin, moduleSetObservable, moduleSet);
    t_memo.NoteWalked(moduleSet, overlayIdentityGeneration);
    return t_origin;
}

void NoteModuleForPresentOriginator(HMODULE module) {
    const std::string path = module ? ce::runtime_override_path::ModulePath(module) : std::string{};
    ce::present_originator::PresentRuntimeModules().NoteModule(path.c_str(), ExportsStreamlinePluginApi(module));
}

void SeedPresentOriginatorModules() {
    std::vector<HMODULE> modules;
    if (!ce::EnumerateProcessModules(GetCurrentProcess(), modules)) {
        // Never completing the seed keeps the walk on every Present, which is always correct.
        HookLog("[PRESENT ORIGIN] module scan failed; the originator walk stays on every Present");
        return;
    }
    for (HMODULE module : modules) {
        // Pinned for the export lookup: a module on its way out must not unmap under it.
        HMODULE retained = nullptr;
        if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCSTR>(module), &retained)) {
            continue;  // gone since the enumeration, so it cannot be presenting
        }
        NoteModuleForPresentOriginator(retained);
        FreeLibrary(retained);
    }
    auto& watch = ce::present_originator::PresentRuntimeModules();
    watch.CompleteSeed();
    HookLogImportant("[PRESENT ORIGIN] scanned %zu modules: frame-generation runtime mapped=%d (no runtime: the "
                     "thread keeps its last originator walk until the module set changes or a runtime loads)",
                     modules.size(), watch.SawRuntime() ? 1 : 0);
}

void ClassifyPresentCallerOrigin(const void* callerAddress, PresentOrigin* out) {
    if (!out) {
        return;
    }
    *out = PresentOrigin{};
    HMODULE callerModule = nullptr;
    char callerPath[MAX_PATH] = {};
    if (!TryGetModulePathFromCodeAddress(callerAddress, callerPath, sizeof(callerPath), &callerModule) ||
        !callerModule) {
        return;
    }
    RecordWalkedModule(out, callerPath);
    if (callerModule == CaptureEngineModuleHandle()) {
        return;  // CE's own wrapper presenting: no foreign originator to name.
    }
    SetOriginator(out, callerPath,
                  ce::present_originator::ClassifyOriginatorModule(callerPath, IsStreamlineModuleHandle(callerModule)));
    out->originatorFrame = callerAddress;
}

const PresentCallerVerdict& ClassifyPresentCaller(const void* callerAddress) {
    thread_local ce::overlay_compat::CallerVerdictCache<PresentCallerVerdict, 2> t_verdicts;
    return ce::overlay_compat::GetCallerVerdict(t_verdicts, callerAddress, [callerAddress](PresentCallerVerdict& fresh) {
        fresh = PresentCallerVerdict{};
        fresh.pathResolved = TryGetModulePathFromCodeAddress(callerAddress, fresh.path, sizeof(fresh.path));
        fresh.thirdPartyOverlay = fresh.pathResolved && ce::overlay_compat::IsThirdPartyOverlayModulePath(fresh.path);
        ClassifyPresentCallerOrigin(callerAddress, &fresh.origin);
        fresh.streamlineModule = IsCodeAddressFromStreamlineModule(callerAddress);
        fresh.ffxFrameGenerationModule = ce::overlay_compat::IsCodeAddressFromFFXFrameGenerationModule(callerAddress);
        return fresh.pathResolved;  // a failed lookup asks again: it may be a module that is still loading
    });
}

void NotePresentOrigin(IDXGISwapChain* swapchain, bool present1, const PresentOrigin& origin) {
    const DWORD threadId = GetCurrentThreadId();
    const uint64_t nowMs = ce::hook_clock::TickCount64();
    OriginSummary& summary = Summary();
    summary.presents.fetch_add(1, std::memory_order_relaxed);
    summary.byOriginator[static_cast<size_t>(origin.originator)].fetch_add(1, std::memory_order_relaxed);

    const char* moduleName = origin.module[0] ? ce::overlay_compat::ExtractModuleBaseName(origin.module) : "none";
    uint64_t moduleHash = 1469598103934665603ull;
    for (const char* cursor = moduleName; *cursor; ++cursor) {
        moduleHash ^= static_cast<uint64_t>(ce::overlay_compat::detail::ToLowerAscii(*cursor));
        moduleHash *= 1099511628211ull;
    }
    const auto verdict = OriginGates().Observe(ce::log_meter::FieldKey(swapchain, threadId, present1),
                                               ce::log_meter::FieldKey(origin.originator, moduleHash, origin.walked));
    if (verdict) {
        char chain[PresentOrigin::kMaxModules * (PresentOrigin::kModuleNameLength + 1) + 1] = {};
        FormatChain(origin, chain, sizeof(chain));
        HookLogImportant("[PRESENT ORIGIN] sc=%p tid=0x%04lX %s originator=%s module=%s via=%s walked=%s%s", swapchain,
                         static_cast<unsigned long>(threadId), present1 ? "Present1" : "Present",
                         ce::present_originator::OriginatorName(origin.originator), moduleName,
                         origin.walked ? "below-chain-walk" : "immediate-caller", chain[0] ? chain : "none",
                         ce::log_meter::SuppressedNote(verdict.suppressed).c_str());
    }
    MaybeLogSummary(nowMs);
    DX12_NoteOverlayPresentOriginator(static_cast<uint8_t>(origin.originator));
    // Whether Streamline presents the frames decides whether a foreign-served DLSS-G API is a front; CE's DLSS-G
    // state follows when this present moved that verdict (a DLSS-G enabled before Streamline's first present).
    ce::fg_presenter::NotePresentOriginator(origin.originator == ce::present_originator::Originator::kStreamline);
    StreamlineHook::ReconcileFrontSignal("present originator");
}

}  // namespace DXGIShared
