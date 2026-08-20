#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

// Why a D3D12 device could not be created, decided from evidence rather than guessed.
//
// A failing `D3D12CreateDevice` is the single most opaque thing that can happen inside an
// injected process: the HRESULT is the same `DXGI_ERROR_UNSUPPORTED` whether the adapter
// genuinely cannot do the job, a foreign entry patch answered the call, or the runtime
// refuses every adapter in the process. Witcher 3 session 20260820_211008 showed how far
// that ambiguity reaches — CE logged the same `hr=0x887A0004` thirty times over five
// seconds and the game threw an unhandled `HRESULT of 0x887A0004` a moment later, with
// nothing in either log able to say which of the three it was.
//
// This header holds the parts of that answer that are pure decisions, so the reporter in
// `hook/apis/dx12_device_creation_report.cpp` only has to gather facts. Entry-patch shapes
// are classified here rather than in the reporter because the same four encodings show up
// wherever CE has to decide "did somebody else hook this", and they are exactly the shapes
// `InlineHook::CreateBypassTrampoline` can step around.
namespace ce::d3d12_device_creation {

// Feature levels the probe walks, lowest first. The lowest is the baseline every D3D12
// adapter must satisfy; the highest is the one a modern renderer is likely to ask for.
inline constexpr int kProbedFeatureLevelCount = 4;

// A terminal failure is retried this many times before the temp-swapchain route stops
// paying for device creation. Each attempt costs a real driver load: the Witcher 3 session
// shows ~48 ms and one `nvldumdx.dll` map/unmap cycle per attempt, inside the game's
// startup, for a result that cannot change later in the process.
inline constexpr int kMaxTerminalDeviceCreationAttempts = 3;

// Reports are one per distinct HRESULT, capped. A second, different HRESULT is worth a
// second report; the same one repeated never is.
inline constexpr int kMaxDeviceCreationReports = 3;

// ---------------------------------------------------------------------------
// Entry-point patch classification
// ---------------------------------------------------------------------------

enum class EntryPatchKind {
    None,
    RelativeJump,     // E9 rel32
    IndirectJump,     // FF 25 disp32 - Microsoft Detours, RTSS
    AbsoluteMovJump,  // 48 B8 imm64 / FF E0
    PushRet,          // 68 imm32 / C3
};

inline EntryPatchKind ClassifyEntryPatch(const uint8_t* bytes, size_t size) {
    if (bytes == nullptr) {
        return EntryPatchKind::None;
    }
    if (size >= 5 && bytes[0] == 0xE9) {
        return EntryPatchKind::RelativeJump;
    }
    if (size >= 6 && bytes[0] == 0xFF && bytes[1] == 0x25) {
        return EntryPatchKind::IndirectJump;
    }
    if (size >= 12 && bytes[0] == 0x48 && bytes[1] == 0xB8 && bytes[10] == 0xFF && bytes[11] == 0xE0) {
        return EntryPatchKind::AbsoluteMovJump;
    }
    if (size >= 6 && bytes[0] == 0x68 && bytes[5] == 0xC3) {
        return EntryPatchKind::PushRet;
    }
    return EntryPatchKind::None;
}

inline bool IsForeignEntryPatch(EntryPatchKind kind) {
    return kind != EntryPatchKind::None;
}

inline const char* DescribeEntryPatch(EntryPatchKind kind) {
    switch (kind) {
        case EntryPatchKind::None:
            return "unpatched";
        case EntryPatchKind::RelativeJump:
            return "E9 rel32 jump";
        case EntryPatchKind::IndirectJump:
            return "FF25 indirect jump";
        case EntryPatchKind::AbsoluteMovJump:
            return "movabs+jmp rax";
        case EntryPatchKind::PushRet:
            return "push imm32 + ret";
    }
    return "unknown";
}

inline int32_t ReadDisplacement(const uint8_t* bytes, size_t offset) {
    int32_t value = 0;
    std::memcpy(&value, bytes + offset, sizeof(value));
    return value;
}

// Target of an `E9 rel32` entry patch. `entryAddress` is where those bytes live.
inline bool TryComputeRelativeJumpTarget(const uint8_t* bytes, size_t size, uint64_t entryAddress,
                                         uint64_t* targetOut) {
    if (bytes == nullptr || targetOut == nullptr || size < 5 || bytes[0] != 0xE9) {
        return false;
    }
    *targetOut = entryAddress + 5 + static_cast<uint64_t>(static_cast<int64_t>(ReadDisplacement(bytes, 1)));
    return true;
}

// Address of the pointer slot an `FF 25 disp32` entry patch reads through. The caller reads
// it - this header never dereferences process memory.
inline bool TryComputeIndirectJumpSlot(const uint8_t* bytes, size_t size, uint64_t entryAddress,
                                       uint64_t* slotAddressOut) {
    if (bytes == nullptr || slotAddressOut == nullptr || size < 6 || bytes[0] != 0xFF || bytes[1] != 0x25) {
        return false;
    }
    *slotAddressOut = entryAddress + 6 + static_cast<uint64_t>(static_cast<int64_t>(ReadDisplacement(bytes, 2)));
    return true;
}

inline bool TryComputeAbsoluteMovTarget(const uint8_t* bytes, size_t size, uint64_t* targetOut) {
    if (bytes == nullptr || targetOut == nullptr || size < 12 || bytes[0] != 0x48 || bytes[1] != 0xB8 ||
        bytes[10] != 0xFF || bytes[11] != 0xE0) {
        return false;
    }
    uint64_t value = 0;
    std::memcpy(&value, bytes + 2, sizeof(value));
    *targetOut = value;
    return true;
}

// ---------------------------------------------------------------------------
// Verdict
// ---------------------------------------------------------------------------

// What one adapter answered when asked to create a device. `baselineSupported` is the
// lowest probed feature level, `topSupported` the highest.
struct AdapterProbe {
    bool software = false;
    bool baselineSupported = false;
    bool topSupported = false;
};

struct Evidence {
    bool adapterEnumerationFailed = false;
    bool entryForeignPatched = false;
    bool bypassAttempted = false;
    bool bypassCreatedDevice = false;
    bool hookedCreatedDevice = false;
    // D3D11 on the same hardware, as the discriminator between "D3D12 is unhappy here" and
    // "the display driver will not give this process a device at all". Witcher 3 needed
    // exactly this: on the reporting machine a process named witcher3.exe is refused
    // DXGI_ERROR_UNSUPPORTED for both APIs while the identical binary under any other name
    // gets a device, which is a per-application driver decision no application can fix.
    bool d3d11Attempted = false;
    bool d3d11CreatedDevice = false;
    unsigned adapterCount = 0;
    unsigned hardwareAdapters = 0;
    unsigned hardwareAdaptersAtBaseline = 0;
    unsigned adaptersRejectingTopFeatureLevel = 0;
};

inline void AccumulateAdapter(Evidence* evidence, const AdapterProbe& probe) {
    if (evidence == nullptr) {
        return;
    }
    evidence->adapterCount++;
    if (!probe.software) {
        evidence->hardwareAdapters++;
        if (probe.baselineSupported) {
            evidence->hardwareAdaptersAtBaseline++;
        }
    }
    if (probe.baselineSupported && !probe.topSupported) {
        evidence->adaptersRejectingTopFeatureLevel++;
    }
}

enum class Verdict {
    Inconclusive,
    NoAdapterEnumerated,
    ForeignEntryPatchRejectsCreation,
    DisplayDriverRefusesThisProcess,
    RuntimeRejectsEveryAdapter,
    HardwareOkSomeAdapterBelowTopFeatureLevel,
    HardwareCreatesDevices,
};

inline Verdict Classify(const Evidence& evidence) {
    if (evidence.adapterEnumerationFailed || evidence.adapterCount == 0) {
        return Verdict::NoAdapterEnumerated;
    }
    // Only a bypass that succeeded where the patched entry failed identifies the patch as
    // the cause. A bypass that fails the same way proves the opposite and must not be read
    // as an accusation.
    if (evidence.entryForeignPatched && evidence.bypassAttempted && evidence.bypassCreatedDevice &&
        !evidence.hookedCreatedDevice) {
        return Verdict::ForeignEntryPatchRejectsCreation;
    }
    if (evidence.hardwareAdaptersAtBaseline == 0) {
        // DXGI still described the hardware adapter, so the GPU is present and enumerable;
        // if D3D11 is refused on it too, the refusal is the display driver's, below both
        // runtimes, and nothing the application or CE does can change it.
        if (evidence.d3d11Attempted && !evidence.d3d11CreatedDevice && evidence.hardwareAdapters > 0) {
            return Verdict::DisplayDriverRefusesThisProcess;
        }
        return Verdict::RuntimeRejectsEveryAdapter;
    }
    if (evidence.adaptersRejectingTopFeatureLevel > 0) {
        return Verdict::HardwareOkSomeAdapterBelowTopFeatureLevel;
    }
    if (evidence.hookedCreatedDevice || evidence.bypassCreatedDevice) {
        return Verdict::HardwareCreatesDevices;
    }
    return Verdict::Inconclusive;
}

inline const char* Describe(Verdict verdict) {
    switch (verdict) {
        case Verdict::Inconclusive:
            return "inconclusive - device creation failed but every probe below disagrees; treat the matrix as the "
                   "evidence";
        case Verdict::NoAdapterEnumerated:
            return "DXGI enumerated no usable adapter in this process, so no D3D12 device can exist here";
        case Verdict::ForeignEntryPatchRejectsCreation:
            return "a foreign entry patch on D3D12CreateDevice is rejecting the call - the unpatched body creates a "
                   "device, the patched entry does not";
        case Verdict::DisplayDriverRefusesThisProcess:
            return "the display driver refuses this process: DXGI describes the hardware adapter, but BOTH D3D11 and "
                   "D3D12 device creation fail on it - a per-application driver decision below both runtimes, which "
                   "no application or overlay can work around. Compare against the same executable renamed, and "
                   "against the vendor's per-application profile for this executable name";
        case Verdict::RuntimeRejectsEveryAdapter:
            return "the D3D12 runtime refuses every hardware adapter in this process, including the baseline feature "
                   "level, while D3D11 is not refused - this is a D3D12 runtime/state problem in this process, not a "
                   "CE one";
        case Verdict::HardwareOkSomeAdapterBelowTopFeatureLevel:
            return "hardware adapters create devices at the baseline, but at least one enumerated adapter cannot "
                   "reach the top feature level - an application that probes every adapter and treats a failure as "
                   "fatal will abort here";
        case Verdict::HardwareCreatesDevices:
            return "device creation works when probed directly - the observed failure was specific to the failing "
                   "call's parameters or timing, not to this process's D3D12 state";
    }
    return "unclassified";
}

// ---------------------------------------------------------------------------
// Retry and report budgets
// ---------------------------------------------------------------------------

// HRESULTs that describe a decision, not a moment: nothing later in the process turns them
// into success, so re-running device creation for them only costs another driver load.
inline bool IsTerminalCreationFailure(int32_t hr) {
    switch (static_cast<uint32_t>(hr)) {
        case 0x887A0004u:  // DXGI_ERROR_UNSUPPORTED
        case 0x887A002Du:  // DXGI_ERROR_SDK_COMPONENT_MISSING
        case 0x80004002u:  // E_NOINTERFACE
        case 0x80070057u:  // E_INVALIDARG
            return true;
        default:
            return false;
    }
}

inline bool ShouldRetryTempDeviceCreation(int priorTerminalFailures, int32_t lastHr) {
    if (!IsTerminalCreationFailure(lastHr)) {
        return true;
    }
    return priorTerminalFailures < kMaxTerminalDeviceCreationAttempts;
}

inline bool ShouldEmitReport(int reportsEmitted, int32_t lastReportedHr, int32_t hr) {
    if (reportsEmitted <= 0) {
        return true;
    }
    if (reportsEmitted >= kMaxDeviceCreationReports) {
        return false;
    }
    return hr != lastReportedHr;
}

}  // namespace ce::d3d12_device_creation
