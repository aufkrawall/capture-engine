#include "present_origin.h"

#include "dxgi_shared_internal.h"
#include "common/logging/log_meter.h"
#include "hook/runtime/hook_clock.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>

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

void SetOriginator(PresentOrigin* out, const char* modulePath, bool streamlineModule) {
    out->originator = ce::present_originator::ClassifyOriginatorModule(modulePath, streamlineModule);
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

}  // namespace

void ResolvePresentOriginatorBelowForeignChain(PresentOrigin* out) {
    if (!out) {
        return;
    }
    *out = PresentOrigin{};
    out->walked = true;
    constexpr USHORT kMaxFrames = 16;
    void* stackFrames[kMaxFrames] = {};
    const USHORT frameCount = CaptureStackBackTrace(1, kMaxFrames, stackFrames, nullptr);
    for (USHORT i = 0; i < frameCount; ++i) {
        HMODULE frameModule = nullptr;
        char framePath[MAX_PATH] = {};
        if (!TryGetModulePathFromCodeAddress(stackFrames[i], framePath, sizeof(framePath), &frameModule) ||
            !frameModule) {
            continue;  // Trampoline / JIT-style thunk: not a module frame, keep walking out.
        }
        const FrameKind kind =
            ce::present_originator::ClassifyFrame(framePath, frameModule == CaptureEngineModuleHandle(), SystemDirectory());
        if (kind == FrameKind::kCaptureEngine) {
            continue;  // CE's own wrapper/detour frames.
        }
        RecordWalkedModule(out, framePath);
        if (kind != FrameKind::kOther) {
            continue;  // Steam / RTSS (the chain CE sits below) and the system's dispatch frames between them.
        }
        // First real originator decides; frames above it are its own callers.
        SetOriginator(out, framePath, IsStreamlineModuleHandle(frameModule));
        return;
    }
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
    SetOriginator(out, callerPath, IsStreamlineModuleHandle(callerModule));
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
}

}  // namespace DXGIShared
