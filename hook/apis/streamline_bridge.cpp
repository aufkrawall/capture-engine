#include "streamline_bridge.h"

#include <windows.h>

#include <d3d12.h>
#include <dxgi1_6.h>

#include <atomic>
#include <cstring>
#include <string>
#include <vector>

#include "../../common/config.h"
#include "../common/dll_utils.h"
#include "../common/graphics_runtime_module_policy.h"
#include "../common/hook_common.h"
#include "../wrappers/iat_hook.h"
#include "streamline_bridge_policy.h"

// `g_pLocalConfig` (AppConfig*) comes from hook_common.h.

namespace ce::streamline_bridge {
namespace {

namespace api = ce::streamline_api;

// The 2.x runtime CE owns. Loaded by full path so it maps its own image rather than
// resolving to the 1.x module that already holds these base names.
std::atomic<bool> g_active{false};
HMODULE g_v2Interposer = nullptr;

// One entry per import slot the bridge takes over. `original` is what the slot held before
// - the 1.x interposer's own export - and is the fallback whenever the 2.x runtime turns
// out not to provide the symbol, so a takeover can never make a call disappear.
struct BridgedSlot {
    const char* name = nullptr;
    void* original = nullptr;
    void* v2Target = nullptr;
};
std::vector<BridgedSlot>* g_slots = nullptr;

void* V2Target(const char* name) {
    if (!g_slots) {
        return nullptr;
    }
    for (const BridgedSlot& slot : *g_slots) {
        if (NamesEqual(slot.name, name)) {
            return slot.v2Target ? slot.v2Target : slot.original;
        }
    }
    return nullptr;
}

// Rate-limited so a per-frame call from a game that ignores the failure cannot flood the
// log, while the first few remain visible for diagnosis.
bool ShouldLogCall(std::atomic<uint32_t>& counter) {
    const uint32_t index = counter.fetch_add(1, std::memory_order_relaxed);
    return index < 4 || (index % 4096) == 0;
}

// ---------------------------------------------------------------------------
// Translated 1.x entry points
// ---------------------------------------------------------------------------
//
// M2 stage: the bridge owns these slots but does not translate them yet, so each one
// refuses politely and the game falls back to running without Streamline features. Every
// 1.x entry point here returns `bool` in AL (not the 2.x `sl::Result` in EAX), so the
// refusal has to be `false` and not a zeroed 32-bit result.
//
// Arguments are deliberately untyped: nothing reads them at this stage, and on x64 the
// first four integer/pointer arguments arrive in the same registers regardless of their
// declared types, so a uniform four-argument shape is call-compatible with each of the 1.x
// prototypes without asserting a layout CE has not verified.
#define CE_SL_BRIDGE_REFUSE(exportName)                                                         \
    bool Bridged_##exportName(void*, void*, void*, void*) {                                     \
        static std::atomic<uint32_t> calls{0};                                                  \
        if (ShouldLogCall(calls)) {                                                             \
            HookLogImportant(                                                                   \
                "Streamline bridge: refusing %s - the 1.x -> 2.x translation for this call is " \
                "not implemented yet, so the game runs without Streamline features",            \
                #exportName);                                                                   \
        }                                                                                       \
        return false;                                                                           \
    }

CE_SL_BRIDGE_REFUSE(slInit)
CE_SL_BRIDGE_REFUSE(slShutdown)
CE_SL_BRIDGE_REFUSE(slIsFeatureSupported)
CE_SL_BRIDGE_REFUSE(slSetTag)
CE_SL_BRIDGE_REFUSE(slSetConstants)
CE_SL_BRIDGE_REFUSE(slEvaluateFeature)

#undef CE_SL_BRIDGE_REFUSE

// The two calls whose 1.x payload layout is not public, and what CE does about it.
//
// `slSetFeatureConstants` and `slGetFeatureSettings` carry an opaque per-feature struct -
// `sl::DLSSConstants`, `sl::DLSSGConstants`, `sl::DLSSGSettings`. Translating either one
// needs those layouts as they stood in the interposer the game actually ships, and for
// 1.5.6 they are unpublished: NVIDIA's repository has no 1.x release at all and its 1.x
// tags stop at v1.1.1, which predates DLSS-G entirely. sl.dlss_g 1.5.6 proves the types
// exist (it carries `sl::DLSSGSettings::status` and `numFramesToGenerate` in its own
// diagnostics) but not their offsets.
//
// Guessing them is the one thing CE must not do here. A wrong field in a feature-constants
// struct is not a crash, it is silently wrong frame generation - and a guessed Streamline
// ABI is precisely what put a truncated command-list pointer into sl.common and killed The
// Witcher 3 in `20260820_221409`.
//
// So the call is refused, and the payload is RECORDED instead. One real run of a bridged
// game turns the missing layout from an unavailable document into a measurement: the size
// the game passes, the feature it names, and the leading bytes are exactly what identifies
// the mode field and the struct's shape.
bool ReadableBytes(const void* address, size_t wanted) {
    if (!address || wanted == 0) {
        return false;
    }
    MEMORY_BASIC_INFORMATION info{};
    if (VirtualQuery(address, &info, sizeof(info)) != sizeof(info)) {
        return false;
    }
    if (info.State != MEM_COMMIT) {
        return false;
    }
    constexpr DWORD kNoRead = PAGE_NOACCESS | PAGE_GUARD;
    if ((info.Protect & kNoRead) != 0 || info.Protect == 0) {
        return false;
    }
    const auto base = static_cast<const uint8_t*>(info.BaseAddress);
    const auto start = static_cast<const uint8_t*>(address);
    const size_t remaining = info.RegionSize - static_cast<size_t>(start - base);
    return remaining >= wanted;
}

// Bounded, guarded, and rate-limited: a diagnostic that reads somebody else's struct must
// never be the thing that faults, and must never become hot-path noise.
void RecordOpaqueFeaturePayload(const char* call, uint32_t v1Feature, const void* payload) {
    static std::atomic<uint32_t> recorded{0};
    const uint32_t index = recorded.fetch_add(1, std::memory_order_relaxed);
    if (index >= 12 && (index % 8192) != 0) {
        return;
    }

    constexpr size_t kDumpBytes = 64;
    if (!ReadableBytes(payload, kDumpBytes)) {
        HookLogImportant("Streamline bridge: %s(%s) payload=%p is not readable - nothing recorded", call,
                         DescribeV1Feature(v1Feature), payload);
        return;
    }

    const auto* bytes = static_cast<const uint8_t*>(payload);
    char hex[kDumpBytes * 3 + 1] = {};
    for (size_t i = 0; i < kDumpBytes; ++i) {
        static const char kDigits[] = "0123456789abcdef";
        hex[i * 3 + 0] = kDigits[bytes[i] >> 4];
        hex[i * 3 + 1] = kDigits[bytes[i] & 0xF];
        hex[i * 3 + 2] = ' ';
    }
    uint32_t leadingDword = 0;
    memcpy(&leadingDword, bytes, sizeof(leadingDword));
    HookLogImportant(
        "Streamline bridge: %s(%s) refused - the 1.x layout for this struct is not published for sl.interposer "
        "1.5.6, and CE will not guess a foreign ABI. Recording it instead so one run yields the layout. "
        "leading uint32=%u first %zu bytes: %s",
        call, DescribeV1Feature(v1Feature), leadingDword, kDumpBytes, hex);
}

// 1.x: bool slSetFeatureConstants(Feature, const void* consts, uint32_t frameIndex, uint32_t id)
bool Bridged_slSetFeatureConstants(uint32_t feature, const void* consts, uint32_t, uint32_t) {
    RecordOpaqueFeaturePayload("slSetFeatureConstants", feature, consts);
    return false;
}

// 1.x: bool slGetFeatureSettings(Feature, const void* consts, void* settings)
bool Bridged_slGetFeatureSettings(uint32_t feature, const void* consts, void*) {
    RecordOpaqueFeaturePayload("slGetFeatureSettings", feature, consts);
    return false;
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

HRESULT WINAPI Bridged_CreateDXGIFactory(REFIID riid, void** ppFactory) {
    auto fn = reinterpret_cast<PFN_CreateDXGIFactory>(V2Target("CreateDXGIFactory"));
    return fn ? fn(riid, ppFactory) : DXGI_ERROR_UNSUPPORTED;
}

HRESULT WINAPI Bridged_CreateDXGIFactory1(REFIID riid, void** ppFactory) {
    auto fn = reinterpret_cast<PFN_CreateDXGIFactory>(V2Target("CreateDXGIFactory1"));
    return fn ? fn(riid, ppFactory) : DXGI_ERROR_UNSUPPORTED;
}

HRESULT WINAPI Bridged_CreateDXGIFactory2(UINT flags, REFIID riid, void** ppFactory) {
    auto fn = reinterpret_cast<PFN_CreateDXGIFactory2>(V2Target("CreateDXGIFactory2"));
    return fn ? fn(flags, riid, ppFactory) : DXGI_ERROR_UNSUPPORTED;
}

HRESULT WINAPI Bridged_DXGIGetDebugInterface1(UINT flags, REFIID riid, void** pDebug) {
    auto fn = reinterpret_cast<PFN_DXGIGetDebugInterface1>(V2Target("DXGIGetDebugInterface1"));
    return fn ? fn(flags, riid, pDebug) : DXGI_ERROR_UNSUPPORTED;
}

HRESULT WINAPI Bridged_D3D12CreateDevice(IUnknown* adapter, D3D_FEATURE_LEVEL minimumFeatureLevel, REFIID riid,
                                         void** ppDevice) {
    auto fn = reinterpret_cast<PFN_D3D12CreateDevice>(V2Target("D3D12CreateDevice"));
    return fn ? fn(adapter, minimumFeatureLevel, riid, ppDevice) : DXGI_ERROR_UNSUPPORTED;
}

HRESULT WINAPI Bridged_D3D12GetDebugInterface(REFIID riid, void** ppDebug) {
    auto fn = reinterpret_cast<PFN_D3D12GetDebugInterface>(V2Target("D3D12GetDebugInterface"));
    return fn ? fn(riid, ppDebug) : DXGI_ERROR_UNSUPPORTED;
}

HRESULT WINAPI Bridged_D3D12SerializeVersionedRootSignature(const D3D12_VERSIONED_ROOT_SIGNATURE_DESC* desc,
                                                            ID3DBlob** blob, ID3DBlob** error) {
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

// The generation of the interposer the game itself is running. Read off the loaded module's
// own file so it is available before CE has hooked anything.
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

// Whether the game has already brought its own 1.x runtime up.
//
// Streamline 1.x loads its plugins from inside `slInit` - before that call only the
// statically imported interposer is resident. A loaded `sl.common.dll` is therefore proof
// that the game already initialised the runtime CE would be replacing, and the bridge must
// decline rather than take half the surface over. The signal is deliberately conservative:
// something else having loaded that module is also a reason not to bridge.
bool GameAlreadyInitializedStreamline() { return GetModuleHandleA("sl.common.dll") != nullptr; }

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

// Repoints every module that imports from sl.interposer.dll at CE's thunks.
//
// The game's imports were resolved by the loader before CE existed, so the module load
// itself cannot be redirected - the slots have to be rewritten in memory. That is also why
// this works without touching anything on disk: nothing is renamed or patched, and the
// takeover disappears with the process.
size_t TakeOverImports() {
    size_t patched = 0;
    for (BridgedSlot& slot : *g_slots) {
        void* thunk = IsTranslatedV1Export(slot.name) ? TranslatedThunk(slot.name) : PassThroughThunk(slot.name);
        if (!thunk) {
            continue;
        }
        void* original = nullptr;
        if (IATHook::PatchIATAllModules("sl.interposer.dll", slot.name, thunk, &original)) {
            slot.original = original;
            ++patched;
        }
    }
    return patched;
}

// Brings the 2.x runtime up with the preferences the bridge pins. Returns false without
// leaving anything taken over, so a failure here is simply "no bridge".
bool InitializeV2Runtime(const std::string& runtimeDir) {
    // sl::Preferences 2.11.1. The hook DLL cannot include sl.h - the SDK include path is
    // wired for the FG test apps only - so this mirrors the SDK layout, which makes it a
    // claim about somebody else's ABI. A wrong field here is a silent mis-init rather than
    // a crash, so it was MEASURED against the real header rather than read off it:
    // sizeof and all 18 field offsets compared equal, and the struct GUID matched
    // (scratchpad sl_prefs_layout.cpp, 2026-08-21). Note the BaseStructure header order -
    // `next` leads at offset 0 and `structType` follows at 8, which is the opposite of what
    // the declaration reads like.
    struct SlPreferences {
        void* next = nullptr;                                          // 0
        uint8_t structType[16] = {0x65, 0x09, 0xa1, 0x1c, 0x8e, 0xbf,  // 8
                                  0x2b, 0x43, 0x8d, 0xa1, 0x67, 0x16, 0xd8, 0x79, 0xfb, 0x14};
        uint32_t structVersion = 1;  // 24
        uint32_t pad0 = 0;
        bool showConsole = false;
        uint32_t logLevel = 1;  // eDefault
        const wchar_t** pathsToPlugins = nullptr;
        uint32_t numPathsToPlugins = 0;
        const wchar_t* pathToLogsAndData = nullptr;
        void* allocateCallback = nullptr;
        void* releaseCallback = nullptr;
        void* logMessageCallback = nullptr;
        uint64_t flags = 0;
        const uint32_t* featuresToLoad = nullptr;
        uint32_t numFeaturesToLoad = 0;
        uint32_t applicationId = 0;
        uint32_t engine = 0;  // eCustom
        const char* engineVersion = nullptr;
        const char* projectId = nullptr;
        uint32_t renderAPI = 1;  // eD3D12
    };

    auto slInit = reinterpret_cast<int (*)(const void*, uint64_t)>(GetProcAddress(g_v2Interposer, "slInit"));
    if (!slInit) {
        HookLogImportant("Streamline bridge: the 2.x runtime exports no slInit - not bridging");
        return false;
    }

    const std::wstring widePath = [&] {
        const int len = MultiByteToWideChar(CP_UTF8, 0, runtimeDir.c_str(), -1, nullptr, 0);
        std::wstring out(len > 0 ? static_cast<size_t>(len) : 0, L'\0');
        if (len > 0) {
            MultiByteToWideChar(CP_UTF8, 0, runtimeDir.c_str(), -1, out.data(), len);
        }
        while (!out.empty() && out.back() == L'\0') {
            out.pop_back();
        }
        return out;
    }();

    const wchar_t* paths[] = {widePath.c_str()};
    const uint32_t features[] = {kV2FeatureDLSS, kV2FeatureDLSS_G, kV2FeatureReflex, kV2FeaturePCL};

    SlPreferences prefs;
    prefs.pathsToPlugins = paths;
    prefs.numPathsToPlugins = 1;
    prefs.flags = BridgePreferenceFlags();
    prefs.featuresToLoad = features;
    prefs.numFeaturesToLoad = static_cast<uint32_t>(sizeof(features) / sizeof(features[0]));

    // Declare the SDK version this runtime actually is, not one CE was built against.
    uint32_t major = 0, minor = 0, patch = 0;
    const std::string interposerPath = runtimeDir + "\\sl.interposer.dll";
    if (!DllFileVersionParts(interposerPath.c_str(), &major, &minor, &patch) || major != 2) {
        HookLogImportant("Streamline bridge: cannot read a 2.x version from %s - not bridging",
                         interposerPath.c_str());
        return false;
    }
    const int result = slInit(&prefs, StreamlineSdkVersion(major, minor, patch));
    if (result != 0) {
        HookLogImportant(
            "Streamline bridge: Streamline %u.%u.%u refused to initialise (sl::Result=%d) with plugins from %s - "
            "not bridging, the game keeps its own Streamline",
            major, minor, patch, result, runtimeDir.c_str());
        return false;
    }
    HookLogImportant(
        "Streamline bridge: Streamline %u.%u.%u initialised with plugins pinned to %s (OTA off, DXGI factory "
        "proxy on)",
        major, minor, patch, runtimeDir.c_str());
    return true;
}

}  // namespace

bool IsActive() { return g_active.load(std::memory_order_acquire); }

void TryActivate() {
    static std::atomic<bool> s_attempted{false};
    if (s_attempted.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    if (!g_pLocalConfig) {
        return;
    }

    const std::string runtimeDir = TrimTrailingSeparator(g_pLocalConfig->graphics.streamlineDllPath);

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
    inputs.gameAlreadyCreatedDeviceOrFactory = false;

    const ActivationDecision decision = DecideActivation(inputs);
    if (decision != ActivationDecision::Activate) {
        // Silent only in the overwhelmingly common case of a profile that never asked.
        if (inputs.upgradeEnabled) {
            HookLogImportant(
                "Streamline bridge: not activating - %s (process runs %s, %s holds %s)", Describe(decision),
                api::Describe(inputs.processGeneration),
                inputs.runtimePathConfigured ? runtimeDir.c_str() : "(no streamline_dll_path)",
                api::Describe(inputs.runtimeGeneration));
        }
        return;
    }

    HookLogImportant(
        "Streamline bridge: this process runs %s and %s holds %s - upgrading it in place instead of "
        "substituting DLLs",
        api::Describe(inputs.processGeneration), runtimeDir.c_str(), api::Describe(inputs.runtimeGeneration));

    const std::string interposerPath = runtimeDir + "\\sl.interposer.dll";
    g_v2Interposer = LoadLibraryA(interposerPath.c_str());
    if (!g_v2Interposer) {
        HookLogImportant("Streamline bridge: failed to load %s (error %lu) - not bridging", interposerPath.c_str(),
                         GetLastError());
        return;
    }
    if (!ExportsLookLike2x(g_v2Interposer)) {
        HookLogImportant(
            "Streamline bridge: %s does not present the 2.x export surface despite its file version - not "
            "bridging",
            interposerPath.c_str());
        return;
    }

    if (!InitializeV2Runtime(runtimeDir)) {
        return;
    }

    // Resolve every slot's 2.x target before touching a single import, so the takeover is
    // the all-or-nothing switch the policy promises rather than a partial rewrite.
    static std::vector<BridgedSlot> slots;
    slots.clear();
    for (size_t i = 0; i < kTranslatedV1ExportCount; ++i) {
        slots.push_back(BridgedSlot{kTranslatedV1Exports[i], nullptr, nullptr});
    }
    for (size_t i = 0; i < kPassThroughExportCount; ++i) {
        BridgedSlot slot{kPassThroughExports[i], nullptr, nullptr};
        slot.v2Target = reinterpret_cast<void*>(GetProcAddress(g_v2Interposer, kPassThroughExports[i]));
        if (!slot.v2Target) {
            HookLogImportant(
                "Streamline bridge: the 2.x runtime does not export %s - that slot will keep forwarding to the "
                "game's own interposer",
                kPassThroughExports[i]);
        }
        slots.push_back(slot);
    }
    g_slots = &slots;

    const size_t patched = TakeOverImports();
    if (patched == 0) {
        HookLogImportant(
            "Streamline bridge: no sl.interposer import slot could be repointed - leaving the process on its own "
            "Streamline");
        g_slots = nullptr;
        return;
    }

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

    g_active.store(true, std::memory_order_release);
    HookLogImportant(
        "Streamline bridge ACTIVE: %zu of %zu sl.interposer import slots now reach CE (%zu translated, %zu "
        "forwarded to the 2.x runtime). The game's own 1.x runtime stays loaded and untouched",
        patched, slots.size(), kTranslatedV1ExportCount, kPassThroughExportCount);
}

}  // namespace ce::streamline_bridge
