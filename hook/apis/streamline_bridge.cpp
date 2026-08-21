#include "streamline_bridge.h"

#include <windows.h>

#include <d3d12.h>
#include <dxgi1_6.h>

#include <atomic>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "../../common/config.h"
#include "../common/dll_utils.h"
#include "../common/graphics_runtime_module_policy.h"
#include "../common/hook_common.h"
#include "../wrappers/iat_hook.h"
#include "streamline_bridge_policy.h"
#include "streamline_bridge_runtime.h"
#include "streamline_bridge_translate.h"
#include "streamline_v1_feature_probe.h"

// `g_pLocalConfig` (AppConfig*) comes from hook_common.h.

namespace ce::streamline_bridge {
namespace {

namespace api = ce::streamline_api;

// The 2.x runtime CE owns. Loaded by full path so it maps its own image rather than
// resolving to the 1.x module that already holds these base names.
std::atomic<bool> g_active{false};
HMODULE g_v2Interposer = nullptr;
std::string* g_runtimeDir = nullptr;

// One entry per import slot the bridge takes over. `original` is what the slot held before
// - the 1.x interposer's own export - and is both the fallback whenever the 2.x runtime
// turns out not to provide the symbol and the handle CE shuts the 1.x runtime down with.
struct BridgedSlot {
    const char* name = nullptr;
    void* original = nullptr;
    void* v2Target = nullptr;
};
std::vector<BridgedSlot>* g_slots = nullptr;

// Bring-up state. The import slots are taken over BEFORE the 2.x runtime is loaded, so a
// game call that arrives in between has to be able to wait for it - see EnsureRuntimeReady.
std::once_flag g_bringUpOnce;
std::atomic<bool> g_runtimeReady{false};
std::atomic<bool> g_runtimeFailed{false};

// Set when the game had already run its own `slInit` before CE could take the imports over.
// The 1.x runtime is then shut back down, but only from a call the GAME makes, never from
// CE's hook thread - see MaybeQuiesceLegacyRuntime.
std::atomic<bool> g_quiescePending{false};

BridgedSlot* FindSlot(const char* name) {
    if (!g_slots) {
        return nullptr;
    }
    for (BridgedSlot& slot : *g_slots) {
        if (NamesEqual(slot.name, name)) {
            return &slot;
        }
    }
    return nullptr;
}

// The 1.x export a slot held before the takeover. Calling it is what the call would have
// done unbridged, which is the only honest fallback when the 2.x runtime cannot serve it.
void* V1Original(const char* name) {
    const BridgedSlot* slot = FindSlot(name);
    return slot ? slot->original : nullptr;
}

bool EnsureRuntimeReady();
void MaybeQuiesceLegacyRuntime();

void* V2Target(const char* name) {
    const BridgedSlot* slot = FindSlot(name);
    if (!slot) {
        return nullptr;
    }
    return slot->v2Target ? slot->v2Target : slot->original;
}

// Every bridged entry point starts here: on a late start the game's own 1.x runtime is
// shut down first, on the game's own thread rather than concurrently with whatever it is
// doing, and only then is the 2.x runtime guaranteed up (or known to have failed).
//
// That order is load-bearing. Both runtimes' plugins carry the same base names -
// `sl.dlss_g.dll`, `sl.common.dll` - and CE's own Streamline hooks are keyed on those
// names, so bringing 2.x up while the 1.x set is still resident leaves two images competing
// for one identity, with `GetModuleHandleW` answering the 1.x one. Shutting 1.x down first
// means the 2.x plugins load into a process where those names are free, and CE's existing
// unload handling re-installs its hooks against the fresh images.
bool BridgeCallReady() {
    MaybeQuiesceLegacyRuntime();
    return EnsureRuntimeReady();
}

// ---------------------------------------------------------------------------
// Translated 1.x entry points
// ---------------------------------------------------------------------------

// The 1.x entry points, in the shapes 1.5.6 uses, each handing off to the translation.
// Verified by disassembly of sl.interposer 1.5.6 and by OptiScaler's vendored SL1 headers:
//   slInit                 (const Preferences&, int applicationId)                 -> bool
//   slShutdown             ()                                                      -> bool
//   slIsFeatureSupported   (Feature, uint32_t* adapterBitMask)                      -> bool
//   slSetTag               (const Resource*, BufferType, uint32_t id, const Extent*)-> bool
//   slSetConstants         (const Constants&, uint32_t frameIndex, uint32_t id)     -> bool
//   slEvaluateFeature      (CommandBuffer*, Feature, uint32_t frameIndex, uint32_t) -> bool
using PFN_V1_slInit = bool (*)(const void*, int);
using PFN_V1_slShutdown = bool (*)();
using PFN_V1_slIsFeatureSupported = bool (*)(uint32_t, uint32_t*);
using PFN_V1_slSetTag = bool (*)(const void*, uint32_t, uint32_t, const void*);
using PFN_V1_slSetConstants = bool (*)(const void*, uint32_t, uint32_t);
using PFN_V1_slSetFeatureConstants = bool (*)(uint32_t, const void*, uint32_t, uint32_t);
using PFN_V1_slGetFeatureSettings = bool (*)(uint32_t, const void*, void*);
using PFN_V1_slEvaluateFeature = bool (*)(void*, uint32_t, uint32_t, uint32_t);

// The fallback is not a courtesy, it is what keeps the takeover honest. The slots are
// repointed before the 2.x runtime is loaded, so "the runtime failed to come up" has to
// mean the process behaves exactly as it would have unbridged - every call reaching the 1.x
// export the slot used to hold - rather than a process with a hole where Streamline was.
template <typename Fn, typename... Args>
bool ForwardToV1(const char* name, Args... args) {
    auto original = reinterpret_cast<Fn>(V1Original(name));
    return original ? original(args...) : false;
}

bool Bridged_slInit(const void* preferences, int applicationId) {
    if (!BridgeCallReady()) {
        return ForwardToV1<PFN_V1_slInit>("slInit", preferences, applicationId);
    }
    return TranslateInit(preferences, applicationId);
}
bool Bridged_slShutdown() {
    if (!BridgeCallReady()) {
        return ForwardToV1<PFN_V1_slShutdown>("slShutdown");
    }
    return TranslateShutdown();
}
bool Bridged_slIsFeatureSupported(uint32_t feature, uint32_t* adapterBitMask) {
    if (!BridgeCallReady()) {
        return ForwardToV1<PFN_V1_slIsFeatureSupported>("slIsFeatureSupported", feature, adapterBitMask);
    }
    return TranslateIsFeatureSupported(feature, adapterBitMask);
}
bool Bridged_slSetTag(const void* resource, uint32_t bufferType, uint32_t id, const void* extent) {
    if (!BridgeCallReady()) {
        return ForwardToV1<PFN_V1_slSetTag>("slSetTag", resource, bufferType, id, extent);
    }
    return TranslateSetTag(resource, bufferType, id, extent);
}
bool Bridged_slSetConstants(const void* constants, uint32_t frameIndex, uint32_t id) {
    if (!BridgeCallReady()) {
        return ForwardToV1<PFN_V1_slSetConstants>("slSetConstants", constants, frameIndex, id);
    }
    return TranslateSetConstants(constants, frameIndex, id);
}
bool Bridged_slEvaluateFeature(void* commandBuffer, uint32_t feature, uint32_t frameIndex, uint32_t id) {
    if (!BridgeCallReady()) {
        return ForwardToV1<PFN_V1_slEvaluateFeature>("slEvaluateFeature", commandBuffer, feature, frameIndex, id);
    }
    return TranslateEvaluateFeature(commandBuffer, feature, frameIndex, id);
}

// The two calls whose 1.x payload layout was never published.
//
// `slSetFeatureConstants` and `slGetFeatureSettings` carry an opaque per-feature struct.
// NVIDIA published no 1.x release and its public 1.x tags stop before DLSS-G existed, so
// these layouts were MEASURED from a real session by CE's own probe rather than inferred
// (`streamline_v1_feature_probe.cpp`; The Witcher 3 `20260821_042540`). The probe stays in
// the path: a title whose constants differ from the measured shape is how a wrong
// assumption surfaces, and the translation refuses anything it has not verified.
// 1.x: bool slSetFeatureConstants(Feature, const void* consts, uint32_t frameIndex, uint32_t id)
// The payload is still recorded on the way through: the layouts these translate against
// were measured, and a future title's constants are how a wrong assumption gets caught.
bool Bridged_slSetFeatureConstants(uint32_t feature, const void* consts, uint32_t frameIndex, uint32_t id) {
    ce::streamline_v1::RecordOpaqueFeaturePayload("slSetFeatureConstants", feature, consts);
    if (!BridgeCallReady()) {
        return ForwardToV1<PFN_V1_slSetFeatureConstants>("slSetFeatureConstants", feature, consts, frameIndex, id);
    }
    return TranslateSetFeatureConstants(feature, consts, frameIndex, id);
}

// 1.x: bool slGetFeatureSettings(Feature, const void* consts, void* settings)
bool Bridged_slGetFeatureSettings(uint32_t feature, const void* consts, void* settings) {
    ce::streamline_v1::RecordOpaqueFeaturePayload("slGetFeatureSettings", feature, consts);
    if (!BridgeCallReady()) {
        return ForwardToV1<PFN_V1_slGetFeatureSettings>("slGetFeatureSettings", feature, consts, settings);
    }
    return TranslateGetFeatureSettings(feature, consts, settings);
}

void* TranslatedThunk(const char* name) {
    if (NamesEqual(name, "slInit")) return reinterpret_cast<void*>(&Bridged_slInit);
    if (NamesEqual(name, "slShutdown")) return reinterpret_cast<void*>(&Bridged_slShutdown);
    if (NamesEqual(name, "slIsFeatureSupported")) return reinterpret_cast<void*>(&Bridged_slIsFeatureSupported);
    if (NamesEqual(name, "slSetTag")) return reinterpret_cast<void*>(&Bridged_slSetTag);
    if (NamesEqual(name, "slSetConstants")) return reinterpret_cast<void*>(&Bridged_slSetConstants);
    if (NamesEqual(name, "slSetFeatureConstants")) return reinterpret_cast<void*>(&Bridged_slSetFeatureConstants);
    if (NamesEqual(name, "slGetFeatureSettings")) return reinterpret_cast<void*>(&Bridged_slGetFeatureSettings);
    if (NamesEqual(name, "slEvaluateFeature")) return reinterpret_cast<void*>(&Bridged_slEvaluateFeature);
    return nullptr;
}

// ---------------------------------------------------------------------------
// Pass-through DXGI / D3D12 entry points
// ---------------------------------------------------------------------------
//
// These are Microsoft's signatures, not NVIDIA's, and the 1.x and 2.x interposers re-export
// them identically so Streamline can interpose device and factory creation. They forward to
// the 2.x runtime unchanged - that is what puts the game's device behind the 2.x
// interposer. If the 2.x runtime turns out not to export one, the original 1.x slot value
// is used, which is exactly what the call would have reached without the bridge.

using PFN_CreateDXGIFactory = HRESULT(WINAPI*)(REFIID, void**);
using PFN_CreateDXGIFactory2 = HRESULT(WINAPI*)(UINT, REFIID, void**);
using PFN_DXGIGetDebugInterface1 = HRESULT(WINAPI*)(UINT, REFIID, void**);
using PFN_D3D12CreateDevice = HRESULT(WINAPI*)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
using PFN_D3D12GetDebugInterface = HRESULT(WINAPI*)(REFIID, void**);
using PFN_D3D12SerializeVersionedRootSignature = HRESULT(WINAPI*)(const D3D12_VERSIONED_ROOT_SIGNATURE_DESC*,
                                                                  ID3DBlob**, ID3DBlob**);

// The device and factory routes are the ones that decide whether frame generation can work
// at all: the 2.x runtime can only drive a device its own interposer created. Recording that
// they came through is what turns "did CE arrive before the game made its device?" - a
// question CE cannot answer in advance, because it never saw the process before injection -
// into a fact the log states outright.
// `reached2x` is not decoration. If the runtime failed to come up, this call is on its way
// to the 1.x export the slot used to hold, and recording it as an interposed creation would
// both state something false in the log and suppress the warning that exists to catch a
// runtime with no device.
void NoteInterposedCreation(const char* what, bool reached2x, std::atomic<bool>& latch) {
    if (!reached2x) {
        return;
    }
    if (!latch.exchange(true, std::memory_order_relaxed)) {
        HookLogImportant("Streamline bridge: the game's %s reached the CE-owned 2.x runtime", what);
    }
}

HRESULT WINAPI Bridged_CreateDXGIFactory(REFIID riid, void** ppFactory) {
    static std::atomic<bool> noted{false};
    NoteInterposedCreation("CreateDXGIFactory", BridgeCallReady(), noted);
    auto fn = reinterpret_cast<PFN_CreateDXGIFactory>(V2Target("CreateDXGIFactory"));
    return fn ? fn(riid, ppFactory) : DXGI_ERROR_UNSUPPORTED;
}

HRESULT WINAPI Bridged_CreateDXGIFactory1(REFIID riid, void** ppFactory) {
    static std::atomic<bool> noted{false};
    NoteInterposedCreation("CreateDXGIFactory1", BridgeCallReady(), noted);
    auto fn = reinterpret_cast<PFN_CreateDXGIFactory>(V2Target("CreateDXGIFactory1"));
    return fn ? fn(riid, ppFactory) : DXGI_ERROR_UNSUPPORTED;
}

HRESULT WINAPI Bridged_CreateDXGIFactory2(UINT flags, REFIID riid, void** ppFactory) {
    static std::atomic<bool> noted{false};
    NoteInterposedCreation("CreateDXGIFactory2", BridgeCallReady(), noted);
    auto fn = reinterpret_cast<PFN_CreateDXGIFactory2>(V2Target("CreateDXGIFactory2"));
    return fn ? fn(flags, riid, ppFactory) : DXGI_ERROR_UNSUPPORTED;
}

HRESULT WINAPI Bridged_DXGIGetDebugInterface1(UINT flags, REFIID riid, void** pDebug) {
    BridgeCallReady();
    auto fn = reinterpret_cast<PFN_DXGIGetDebugInterface1>(V2Target("DXGIGetDebugInterface1"));
    return fn ? fn(flags, riid, pDebug) : DXGI_ERROR_UNSUPPORTED;
}

HRESULT WINAPI Bridged_D3D12CreateDevice(IUnknown* adapter, D3D_FEATURE_LEVEL minimumFeatureLevel, REFIID riid,
                                         void** ppDevice) {
    static std::atomic<bool> noted{false};
    const bool ready = BridgeCallReady();
    NoteInterposedCreation("D3D12CreateDevice", ready, noted);
    auto fn = reinterpret_cast<PFN_D3D12CreateDevice>(V2Target("D3D12CreateDevice"));
    if (!fn) {
        return DXGI_ERROR_UNSUPPORTED;
    }
    const HRESULT hr = fn(adapter, minimumFeatureLevel, riid, ppDevice);
    // The 2.x interposer created this device, so it already has it. Deliberately NOT
    // followed by slSetD3DDevice: Streamline offers interposed creation OR that call for a
    // host that made its own device, and doing both binds the same device twice through a
    // path documented as "NOT thread safe and should be called IMMEDIATELY after main device
    // is created" - from inside the creation CE is currently returning from. CE also has its
    // own inline hook on that export while bridged, so the redundant call re-enters CE's
    // Streamline layer mid-creation as well.
    if (ready && SUCCEEDED(hr) && ppDevice && *ppDevice) {
        NoteV2RuntimeOwnsDevice("created by the 2.x interposer through the bridge");
    }
    return hr;
}

HRESULT WINAPI Bridged_D3D12GetDebugInterface(REFIID riid, void** ppDebug) {
    BridgeCallReady();
    auto fn = reinterpret_cast<PFN_D3D12GetDebugInterface>(V2Target("D3D12GetDebugInterface"));
    return fn ? fn(riid, ppDebug) : DXGI_ERROR_UNSUPPORTED;
}

HRESULT WINAPI Bridged_D3D12SerializeVersionedRootSignature(const D3D12_VERSIONED_ROOT_SIGNATURE_DESC* desc,
                                                            ID3DBlob** blob, ID3DBlob** error) {
    BridgeCallReady();
    auto fn = reinterpret_cast<PFN_D3D12SerializeVersionedRootSignature>(
        V2Target("D3D12SerializeVersionedRootSignature"));
    return fn ? fn(desc, blob, error) : E_NOTIMPL;
}

void* PassThroughThunk(const char* name) {
    if (NamesEqual(name, "CreateDXGIFactory")) return reinterpret_cast<void*>(&Bridged_CreateDXGIFactory);
    if (NamesEqual(name, "CreateDXGIFactory1")) return reinterpret_cast<void*>(&Bridged_CreateDXGIFactory1);
    if (NamesEqual(name, "CreateDXGIFactory2")) return reinterpret_cast<void*>(&Bridged_CreateDXGIFactory2);
    if (NamesEqual(name, "DXGIGetDebugInterface1")) return reinterpret_cast<void*>(&Bridged_DXGIGetDebugInterface1);
    if (NamesEqual(name, "D3D12CreateDevice")) return reinterpret_cast<void*>(&Bridged_D3D12CreateDevice);
    if (NamesEqual(name, "D3D12GetDebugInterface")) return reinterpret_cast<void*>(&Bridged_D3D12GetDebugInterface);
    if (NamesEqual(name, "D3D12SerializeVersionedRootSignature"))
        return reinterpret_cast<void*>(&Bridged_D3D12SerializeVersionedRootSignature);
    return nullptr;
}

// ---------------------------------------------------------------------------
// Activation
// ---------------------------------------------------------------------------

std::string TrimTrailingSeparator(const std::string& path) {
    std::string out = path;
    while (!out.empty() && (out.back() == '\\' || out.back() == '/')) {
        out.pop_back();
    }
    return out;
}

// Whether the game has already brought its own 1.x runtime up.
//
// Streamline 1.x loads its core from inside `slInit` - before that call only the statically
// imported interposer is resident. A loaded `sl.common.dll` is therefore proof that the game
// already initialised the runtime CE is taking over from. That is a reason to shut that
// runtime back down after the takeover, not a reason to refuse it; see DecideActivation.
bool GameAlreadyInitializedStreamline() { return GetModuleHandleA("sl.common.dll") != nullptr; }

// Shuts the game's own 1.x runtime back down after the takeover.
//
// Only ever called from a call the GAME makes. That is what makes it safe rather than
// racy: reaching one of CE's thunks proves the game has returned from whatever 1.x call it
// was in, so nothing is inside that runtime while it is being torn down. Doing the same
// thing from CE's hook thread would be a genuine race against an `slInit` that may still be
// running, and no amount of ordering on CE's side could rule that out.
//
// The shutdown goes through the export pointer saved while repointing the slot, so it
// reaches the 1.x runtime rather than the thunk that now stands in front of it.
void MaybeQuiesceLegacyRuntime() {
    // The load is the fast path - this sits in front of every bridged call, including the
    // per-frame ones - and the exchange is what makes exactly one caller do the work.
    if (!g_quiescePending.load(std::memory_order_acquire)) {
        return;
    }
    if (!g_quiescePending.exchange(false, std::memory_order_acq_rel)) {
        return;
    }
    auto shutdown = reinterpret_cast<bool (*)()>(V1Original("slShutdown"));
    if (!shutdown) {
        HookLogImportant(
            "Streamline bridge: the game's 1.x runtime is up but its slShutdown slot was never captured, so it "
            "stays loaded beside the CE-owned 2.x one");
        return;
    }
    const bool ok = shutdown();
    HookLogImportant(
        "Streamline bridge: shut the game's own 1.x Streamline runtime down (returned %s) - the 2.x runtime CE "
        "owns is now the only initialised one in this process",
        ok ? "true" : "false");
    LogStreamlineModuleInventory("after quiescing the 1.x runtime");
}

// Repoints every module that imports from sl.interposer.dll at CE's thunks.
//
// The game's imports were resolved by the loader before CE existed, so the module load
// itself cannot be redirected - the slots have to be rewritten in memory. That is also why
// this works without touching anything on disk: nothing is renamed or patched, and the
// takeover disappears with the process.
size_t TakeOverImports() {
    // Resolve every original BEFORE the first slot is repointed. The slot value and the
    // interposer's export are the same address, so reading it from the module is equivalent
    // to reading the slot - and unlike the out-parameter of the patch itself, it is already
    // stored by the time a thunk can possibly be entered. Taking the originals afterwards
    // leaves a window in which a call reaching a repointed slot finds no fallback.
    HMODULE v1Interposer = GetModuleHandleA("sl.interposer.dll");
    for (BridgedSlot& slot : *g_slots) {
        slot.original = v1Interposer ? reinterpret_cast<void*>(GetProcAddress(v1Interposer, slot.name)) : nullptr;
    }

    size_t patched = 0;
    for (BridgedSlot& slot : *g_slots) {
        void* thunk = IsTranslatedV1Export(slot.name) ? TranslatedThunk(slot.name) : PassThroughThunk(slot.name);
        if (!thunk) {
            continue;
        }
        void* original = nullptr;
        if (IATHook::PatchIATAllModules("sl.interposer.dll", slot.name, thunk, &original)) {
            if (original) {
                slot.original = original;
            }
            ++patched;
        }
    }
    return patched;
}

// Loads and initialises the CE-owned 2.x runtime, once, whichever thread gets here first.
//
// This deliberately runs AFTER the import slots have been repointed, which is the opposite
// of the obvious order and is the point of the whole restructure. Bringing the runtime up
// costs a LoadLibrary plus an `slInit` that maps sl.common and every pinned plugin - a few
// hundred milliseconds - and the two recorded sessions show the game reaching its own
// Streamline inside exactly that budget. Doing the expensive part first means racing the
// game for it; doing the cheap in-memory takeover first means the game cannot get past CE
// at all, and a call that arrives mid-bring-up simply waits here for it. `std::call_once`
// gives that for nothing: the first caller performs it, everyone else blocks until it is
// done, and no thread ever observes a half-built runtime.
//
// A failure is a complete, coherent end state rather than a hole: every thunk then forwards
// to the 1.x export its slot used to hold, which is what the call would have done unbridged.
bool EnsureRuntimeReady() {
    if (g_runtimeReady.load(std::memory_order_acquire)) {
        return true;
    }
    if (g_runtimeFailed.load(std::memory_order_acquire)) {
        return false;
    }
    std::call_once(g_bringUpOnce, [] {
        g_v2Interposer = LoadAndInitializeV2Runtime(*g_runtimeDir);
        if (!g_v2Interposer || !ResolveTranslationTargets(g_v2Interposer)) {
            g_runtimeFailed.store(true, std::memory_order_release);
            return;
        }

        // Resolve the forwarded DXGI/D3D12 slots now that the module exists. A missing one
        // keeps the 1.x export the slot already held, which is exactly the call the game
        // would otherwise have made.
        for (BridgedSlot& slot : *g_slots) {
            if (!IsPassThroughExport(slot.name)) {
                continue;
            }
            slot.v2Target = reinterpret_cast<void*>(GetProcAddress(g_v2Interposer, slot.name));
            if (!slot.v2Target) {
                HookLogImportant(
                    "Streamline bridge: the 2.x runtime does not export %s - that slot keeps forwarding to the "
                    "game's own interposer",
                    slot.name);
            }
        }

        g_runtimeReady.store(true, std::memory_order_release);
    });
    return g_runtimeReady.load(std::memory_order_acquire);
}

}  // namespace

bool IsActive() { return g_active.load(std::memory_order_acquire); }

void NotifyD3D12Device(void* device) {
    if (!device || !g_active.load(std::memory_order_acquire)) {
        return;
    }
    if (!EnsureRuntimeReady()) {
        return;
    }
    SetV2RuntimeDevice(device);
}

void TryActivate() {
    static std::atomic<bool> s_attempted{false};
    if (s_attempted.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    if (!g_pLocalConfig) {
        return;
    }

    static std::string runtimeDir = TrimTrailingSeparator(g_pLocalConfig->graphics.streamlineDllPath);

    ActivationInputs inputs;
    inputs.upgradeEnabled = g_pLocalConfig->graphics.streamlineUpgrade;
    inputs.runtimePathConfigured = !runtimeDir.empty();
    inputs.processGeneration = ProcessGeneration();
    inputs.runtimeGeneration = api::Generation::Unknown;
    if (inputs.runtimePathConfigured) {
        const std::string candidate = runtimeDir + "\\sl.interposer.dll";
        if (GetFileAttributesA(candidate.c_str()) != INVALID_FILE_ATTRIBUTES) {
            inputs.runtimeGeneration = api::GenerationFromMajorVersion(DllFileMajorVersion(candidate.c_str()));
        }
    }
    inputs.gameAlreadyInitializedStreamline = GameAlreadyInitializedStreamline();
    // Not knowable from inside the process: CE was injected after the game started, so it
    // cannot observe a device created before that. WarnIfTheGameOwnsTheDevice answers it
    // from the calls that actually arrive instead of from a guess made here.
    inputs.gameAlreadyCreatedDeviceOrFactory = false;

    const ActivationDecision decision = DecideActivation(inputs);
    if (!ShouldActivate(inputs)) {
        // Silent only in the overwhelmingly common case of a profile that never asked.
        if (inputs.upgradeEnabled) {
            HookLogImportant(
                "Streamline bridge: not activating - %s (process runs %s, %s holds %s)", Describe(decision),
                api::Describe(inputs.processGeneration),
                inputs.runtimePathConfigured ? runtimeDir.c_str() : "(no streamline_dll_path)",
                api::Describe(inputs.runtimeGeneration));
            LogStreamlineModuleInventory("at the refused takeover");
        }
        return;
    }

    HookLogImportant(
        "Streamline bridge: this process runs %s and %s holds %s - upgrading it in place instead of "
        "substituting DLLs (%s)",
        api::Describe(inputs.processGeneration), runtimeDir.c_str(), api::Describe(inputs.runtimeGeneration),
        Describe(decision));
    LogStreamlineModuleInventory("at the takeover");

    g_runtimeDir = &runtimeDir;

    // The slot table is built and repointed before the 2.x runtime is loaded. Everything
    // here is memory writes into import tables - no loader work, no Streamline call - which
    // is what lets the takeover happen in the narrow window between CE arriving in the
    // process and the game reaching its own Streamline. The expensive half runs behind it,
    // in EnsureRuntimeReady, and any call that arrives meanwhile waits there.
    static std::vector<BridgedSlot> slots;
    slots.clear();
    for (size_t i = 0; i < kTranslatedV1ExportCount; ++i) {
        slots.push_back(BridgedSlot{kTranslatedV1Exports[i], nullptr, nullptr});
    }
    for (size_t i = 0; i < kPassThroughExportCount; ++i) {
        slots.push_back(BridgedSlot{kPassThroughExports[i], nullptr, nullptr});
    }
    g_slots = &slots;

    const size_t patched = TakeOverImports();
    if (patched == 0) {
        HookLogImportant(
            "Streamline bridge: no sl.interposer import slot could be repointed - leaving the process on its own "
            "Streamline");
        g_slots = nullptr;
        g_runtimeDir = nullptr;
        return;
    }

    // On a late start the game's own runtime is already up. It is shut back down from the
    // first call the game makes through a thunk, never from here: see
    // MaybeQuiesceLegacyRuntime for why the thread this happens on is the whole argument.
    if (RequiresLegacyQuiesce(decision)) {
        g_quiescePending.store(true, std::memory_order_release);
    }

    g_active.store(true, std::memory_order_release);
    HookLogImportant(
        "Streamline bridge ACTIVE: %zu of %zu sl.interposer import slots now reach CE (%zu translated, %zu "
        "forwarded to the 2.x runtime)%s",
        patched, slots.size(), kTranslatedV1ExportCount, kPassThroughExportCount,
        RequiresLegacyQuiesce(decision) ? ". The game's 1.x runtime was already initialised and will be shut down "
                                          "on its next Streamline call"
                                        : ". The game's own 1.x runtime never initialised");

    // The NGX runtimes are a separate override family and keep working while bridged:
    // `dlss_sr_dll_path` / `dlss_fg_dll_path` are not sl.* names, so the stand-down above
    // never touches them. They do have to agree with the Streamline folder, though - a
    // bridged runtime resolves its own NGX snippets out of the plugin folder it was pinned
    // to, and a name already loaded from there wins over a later preload from somewhere
    // else. Same folder is the configuration that behaves; anything else is worth saying
    // out loud rather than silently preferring one of them.
    {
        const auto& gfx = g_pLocalConfig->graphics;
        for (const auto& [key, path] : {std::pair<const char*, const std::string&>{"dlss_sr_dll_path",
                                                                                   gfx.dlssSrDllPath},
                                        {"dlss_fg_dll_path", gfx.dlssFgDllPath},
                                        {"dlss_rr_dll_path", gfx.dlssRrDllPath}}) {
            if (path.empty()) {
                continue;
            }
            if (!ce::graphics_runtime::EqualsModulePathIgnoreCase(TrimTrailingSeparator(path).c_str(),
                                                                  runtimeDir.c_str())) {
                HookLogImportant(
                    "Streamline bridge: %s points at %s but the bridged runtime loads its NGX snippets from %s. "
                    "Whichever registers a given nvngx_* name first wins; point both at the same folder to make "
                    "the choice explicit",
                    key, path.c_str(), runtimeDir.c_str());
            }
        }
    }

    // Bring the runtime up now rather than leaving it to the game's first call. The slots
    // are already CE's, so this no longer races anything - it only decides which thread
    // pays for it, and CE's hook thread is the one with time to spare.
    //
    // Except on a late start, where it must NOT happen here. The 1.x set has to be shut
    // down before the 2.x plugins load, or the two claim the same base names at once, and
    // that shutdown only happens on a thread the game itself is on. So the first bridged
    // call does both, in that order, and pays the bring-up cost once - the same cost the
    // game's own slInit would have had.
    if (!RequiresLegacyQuiesce(decision)) {
        EnsureRuntimeReady();
    }
}

}  // namespace ce::streamline_bridge
