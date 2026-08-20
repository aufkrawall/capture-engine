#include "dx12_device_creation_report.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <psapi.h>

#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>

#include "../common/d3d12_device_creation_policy.h"
#include "../common/hook_common.h"
#include "../common/module_pin.h"
#include "../wrappers/inline_hook.h"

namespace ce::dx12_device_creation_report {
namespace {

namespace policy = ce::d3d12_device_creation;

using PFN_D3D12CreateDeviceLocal = HRESULT(WINAPI*)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
using PFN_CreateDXGIFactory1Local = HRESULT(WINAPI*)(REFIID, void**);

constexpr D3D_FEATURE_LEVEL kProbedLevels[policy::kProbedFeatureLevelCount] = {
    D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_12_0, D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_2};
constexpr const char* kProbedLevelNames[policy::kProbedFeatureLevelCount] = {"11_0", "12_0", "12_1", "12_2"};

std::mutex g_reportMutex;
int g_reportsEmitted = 0;
int32_t g_lastReportedHr = 0;

std::atomic<int> g_terminalCreationFailures{0};
std::atomic<int32_t> g_lastCreationHr{0};

// Case-insensitive substring test over ASCII paths. Deliberately local: the report must not
// pull shlwapi into the hook DLL's import set just to filter a module list.
bool ContainsNoCase(const char* haystack, const char* needleLower) {
    if (haystack == nullptr || needleLower == nullptr) {
        return false;
    }
    for (const char* start = haystack; *start != '\0'; ++start) {
        const char* a = start;
        const char* b = needleLower;
        while (*b != '\0' && *a != '\0' && static_cast<char>(std::tolower(static_cast<unsigned char>(*a))) == *b) {
            ++a;
            ++b;
        }
        if (*b == '\0') {
            return true;
        }
    }
    return false;
}

const char* HrName(HRESULT hr) {
    switch (static_cast<uint32_t>(hr)) {
        case 0x00000000u:
            return "S_OK";
        case 0x00000001u:
            return "S_FALSE";
        case 0x887A0004u:
            return "DXGI_ERROR_UNSUPPORTED";
        case 0x887A0001u:
            return "DXGI_ERROR_INVALID_CALL";
        case 0x887A0005u:
            return "DXGI_ERROR_DEVICE_REMOVED";
        case 0x887A0020u:
            return "DXGI_ERROR_DRIVER_INTERNAL_ERROR";
        case 0x887A002Du:
            return "DXGI_ERROR_SDK_COMPONENT_MISSING";
        case 0x80004002u:
            return "E_NOINTERFACE";
        case 0x80004005u:
            return "E_FAIL";
        case 0x80070057u:
            return "E_INVALIDARG";
        case 0x8007000Eu:
            return "E_OUTOFMEMORY";
        default:
            return "unmapped";
    }
}

// Full path of the image owning `address`, plus the offset into it. Falls back to a bare
// address for CE trampoline pools and other non-module thunks, which have no owning module.
void DescribeAddress(const void* address, char* out, size_t outSize) {
    if (address == nullptr) {
        std::snprintf(out, outSize, "(null)");
        return;
    }
    HMODULE owner = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(address), &owner) &&
        owner != nullptr) {
        char path[MAX_PATH] = {};
        if (GetModuleFileNameA(owner, path, MAX_PATH) == 0) {
            path[0] = '\0';
        }
        std::snprintf(out, outSize, "%s+0x%llX", path[0] ? path : "<unnamed module>",
                      static_cast<unsigned long long>(reinterpret_cast<const uint8_t*>(address) -
                                                      reinterpret_cast<const uint8_t*>(owner)));
        return;
    }
    std::snprintf(out, outSize, "%p (no owning module)", address);
}

// Report one export's entry bytes and, when they are a foreign jump, who owns its target.
// Returns the classified patch kind so the caller can decide whether a bypass retry is
// worth attempting.
policy::EntryPatchKind ReportEntryIntegrity(const char* moduleName, const char* exportName) {
    HMODULE module = GetModuleHandleA(moduleName);
    if (module == nullptr) {
        HookLogImportant("DX12 device-creation report:   %s!%s - module not loaded", moduleName, exportName);
        return policy::EntryPatchKind::None;
    }
    auto* entry = reinterpret_cast<const uint8_t*>(GetProcAddress(module, exportName));
    if (entry == nullptr) {
        HookLogImportant("DX12 device-creation report:   %s!%s - export not present", moduleName, exportName);
        return policy::EntryPatchKind::None;
    }
    constexpr size_t kEntryBytes = 16;
    if (!ce::module_pin::IsReadableCode(entry, kEntryBytes)) {
        HookLogImportant("DX12 device-creation report:   %s!%s @%p - entry is not readable code", moduleName,
                         exportName, entry);
        return policy::EntryPatchKind::None;
    }

    char bytesText[kEntryBytes * 3 + 1] = {};
    for (size_t i = 0; i < kEntryBytes; ++i) {
        std::snprintf(bytesText + i * 3, 4, "%02X ", entry[i]);
    }

    const policy::EntryPatchKind kind = policy::ClassifyEntryPatch(entry, kEntryBytes);
    if (!policy::IsForeignEntryPatch(kind)) {
        HookLogImportant("DX12 device-creation report:   %s!%s @%p unpatched [%s]", moduleName, exportName, entry,
                         bytesText);
        return kind;
    }

    const auto entryAddress = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(entry));
    uint64_t target = 0;
    bool haveTarget = policy::TryComputeRelativeJumpTarget(entry, kEntryBytes, entryAddress, &target) ||
                      policy::TryComputeAbsoluteMovTarget(entry, kEntryBytes, &target);
    if (!haveTarget) {
        uint64_t slot = 0;
        if (policy::TryComputeIndirectJumpSlot(entry, kEntryBytes, entryAddress, &slot)) {
            const auto* slotPtr = reinterpret_cast<const void* const*>(static_cast<uintptr_t>(slot));
            // The slot lives in the patched module's data, not its code, so IsReadableCode
            // is the wrong guard; probe it as plain memory instead.
            MEMORY_BASIC_INFORMATION info = {};
            if (VirtualQuery(static_cast<const void*>(slotPtr), &info, sizeof(info)) == sizeof(info) &&
                info.State == MEM_COMMIT) {
                target = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(*slotPtr));
                haveTarget = true;
            }
        }
    }

    char ownerText[MAX_PATH + 64] = {};
    if (haveTarget) {
        DescribeAddress(reinterpret_cast<const void*>(static_cast<uintptr_t>(target)), ownerText, sizeof(ownerText));
    } else {
        std::snprintf(ownerText, sizeof(ownerText), "<target unresolved>");
    }
    HookLogImportant("DX12 device-creation report:   %s!%s @%p PATCHED (%s) -> %s [%s]", moduleName, exportName, entry,
                     policy::DescribeEntryPatch(kind), ownerText, bytesText);
    return kind;
}

// Ask one adapter whether a device *would* be created, without creating one: D3D12 answers
// S_FALSE for a null `ppDevice`, which keeps the probe free of the driver load/teardown the
// real call pays for.
HRESULT ProbeFeatureLevel(PFN_D3D12CreateDeviceLocal create, IUnknown* adapter, D3D_FEATURE_LEVEL level) {
    return create(adapter, level, __uuidof(ID3D12Device), nullptr);
}

void ReportAdapterMatrix(PFN_D3D12CreateDeviceLocal create, PFN_CreateDXGIFactory1Local createFactory,
                         policy::Evidence* evidence) {
    IDXGIFactory1* factory = nullptr;
    const HRESULT factoryHr = createFactory(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory));
    if (FAILED(factoryHr) || factory == nullptr) {
        evidence->adapterEnumerationFailed = true;
        HookLogImportant("DX12 device-creation report:   CreateDXGIFactory1 failed hr=0x%08X (%s) - no adapter could "
                         "be enumerated at all",
                         static_cast<unsigned>(factoryHr), HrName(factoryHr));
        return;
    }

    IDXGIAdapter1* adapter = nullptr;
    for (UINT index = 0; factory->EnumAdapters1(index, &adapter) != DXGI_ERROR_NOT_FOUND; ++index) {
        if (adapter == nullptr) {
            continue;
        }
        DXGI_ADAPTER_DESC1 desc = {};
        adapter->GetDesc1(&desc);

        policy::AdapterProbe probe;
        probe.software = (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;

        char levels[256] = {};
        int written = 0;
        for (int i = 0; i < policy::kProbedFeatureLevelCount; ++i) {
            const HRESULT hr = ProbeFeatureLevel(create, adapter, kProbedLevels[i]);
            const bool ok = SUCCEEDED(hr);
            if (i == 0) {
                probe.baselineSupported = ok;
            }
            if (i == policy::kProbedFeatureLevelCount - 1) {
                probe.topSupported = ok;
            }
            const int room = static_cast<int>(sizeof(levels)) - written;
            if (room > 1) {
                written += std::snprintf(levels + written, static_cast<size_t>(room), "%sFL%s=0x%08X(%s)",
                                         written ? " " : "", kProbedLevelNames[i], static_cast<unsigned>(hr),
                                         HrName(hr));
            }
        }

        HookLogImportant("DX12 device-creation report:   adapter %u '%ls' vendor=0x%04X device=0x%04X flags=0x%X "
                         "vram=%lluMB software=%d | %s",
                         index, desc.Description, desc.VendorId, desc.DeviceId, desc.Flags,
                         static_cast<unsigned long long>(desc.DedicatedVideoMemory >> 20), probe.software ? 1 : 0,
                         levels);

        policy::AccumulateAdapter(evidence, probe);
        adapter->Release();
        adapter = nullptr;
    }
    factory->Release();
}

// Ask D3D11 for a hardware device on the same GPU. This is the discriminator that turns
// "D3D12 will not work here" into "the display driver will not give this process a device
// at all", and it is worth loading d3d11.dll for: a driver-level refusal is the one outcome
// where no amount of CE or application work can help, and saying so is the whole value of
// the report. d3d11.dll is pinned like every other runtime module CE reaches into.
void ProbeD3D11(policy::Evidence* evidence) {
    using PFN_D3D11CreateDevice = HRESULT(WINAPI*)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT,
                                                   const D3D_FEATURE_LEVEL*, UINT, UINT, ID3D11Device**,
                                                   D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
    HMODULE d3d11 = ce::module_pin::PinByName("d3d11.dll");
    if (d3d11 == nullptr) {
        d3d11 = LoadLibraryA("d3d11.dll");
        if (d3d11 != nullptr) {
            d3d11 = ce::module_pin::PinByName("d3d11.dll");
        }
    }
    if (d3d11 == nullptr) {
        HookLogImportant("DX12 device-creation report:   d3d11.dll unavailable - cannot tell a D3D12-only refusal "
                         "from a driver-level one");
        return;
    }
    auto create = reinterpret_cast<PFN_D3D11CreateDevice>(GetProcAddress(d3d11, "D3D11CreateDevice"));
    if (create == nullptr) {
        return;
    }
    const D3D_FEATURE_LEVEL wanted[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    D3D_FEATURE_LEVEL obtained = static_cast<D3D_FEATURE_LEVEL>(0);
    ID3D11Device* device = nullptr;
    const HRESULT hr = create(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, wanted,
                              static_cast<UINT>(sizeof(wanted) / sizeof(wanted[0])), D3D11_SDK_VERSION, &device,
                              &obtained, nullptr);
    evidence->d3d11Attempted = true;
    evidence->d3d11CreatedDevice = SUCCEEDED(hr) && device != nullptr;
    HookLogImportant("DX12 device-creation report:   D3D11CreateDevice(hardware) -> 0x%08X (%s) featureLevel=0x%X",
                     static_cast<unsigned>(hr), HrName(hr), static_cast<unsigned>(obtained));
    if (device != nullptr) {
        device->Release();
    }
}

void ReportRuntimeProvenance() {
    static const char* const kRuntimeModules[] = {"d3d12.dll", "D3D12Core.dll", "dxgi.dll", "D3D12SDKLayers.dll"};
    for (const char* name : kRuntimeModules) {
        HMODULE module = GetModuleHandleA(name);
        if (module == nullptr) {
            HookLogImportant("DX12 device-creation report:   %s not loaded", name);
            continue;
        }
        char path[MAX_PATH] = {};
        if (GetModuleFileNameA(module, path, MAX_PATH) == 0) {
            path[0] = '\0';
        }
        HookLogImportant("DX12 device-creation report:   %s loaded from %s", name, path[0] ? path : "<unknown>");
    }

    // An Agility SDK request the runtime silently declined changes which core answers every
    // later call, so the declared version belongs next to the core that actually loaded.
    HMODULE host = GetModuleHandleW(nullptr);
    const auto* declaredVersion = reinterpret_cast<const UINT*>(GetProcAddress(host, "D3D12SDKVersion"));
    const auto* declaredPath = reinterpret_cast<const char* const*>(GetProcAddress(host, "D3D12SDKPath"));
    if (declaredVersion != nullptr) {
        HookLogImportant("DX12 device-creation report:   host exe declares D3D12SDKVersion=%u path=%s", *declaredVersion,
                         (declaredPath != nullptr && *declaredPath != nullptr) ? *declaredPath : "<none>");
    } else {
        HookLogImportant("DX12 device-creation report:   host exe declares no Agility SDK version");
    }
}

// Everything mapped into the process that is neither Windows nor CE. A D3D12 runtime that
// refuses this process and no other is almost always explained by one of these, so the list
// belongs in the same report rather than in a separate dump nobody correlates.
void ReportForeignModules() {
    HMODULE modules[512] = {};
    DWORD needed = 0;
    if (!EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules), &needed)) {
        return;
    }
    const size_t count = needed / sizeof(HMODULE) < 512 ? needed / sizeof(HMODULE) : 512;
    char line[1024] = {};
    int written = 0;
    unsigned listed = 0;
    for (size_t i = 0; i < count; ++i) {
        char path[MAX_PATH] = {};
        if (GetModuleFileNameA(modules[i], path, MAX_PATH) == 0) {
            continue;
        }
        if (ContainsNoCase(path, "\\windows\\") || ContainsNoCase(path, "capture_hook")) {
            continue;
        }
        const char* leaf = std::strrchr(path, '\\');
        leaf = (leaf != nullptr) ? leaf + 1 : path;
        const int room = static_cast<int>(sizeof(line)) - written;
        if (room <= 32) {
            break;
        }
        written += std::snprintf(line + written, static_cast<size_t>(room), "%s%s", written ? ", " : "", leaf);
        listed++;
    }
    HookLogImportant("DX12 device-creation report:   %u non-Windows modules in process: %s", listed,
                     listed ? line : "<none>");
}

}  // namespace

bool ShouldAttemptTempDeviceCreation() {
    return policy::ShouldRetryTempDeviceCreation(g_terminalCreationFailures.load(std::memory_order_relaxed),
                                                 g_lastCreationHr.load(std::memory_order_relaxed));
}

void NoteTempDeviceCreationResult(HRESULT hr) {
    g_lastCreationHr.store(static_cast<int32_t>(hr), std::memory_order_relaxed);
    if (FAILED(hr) && policy::IsTerminalCreationFailure(static_cast<int32_t>(hr))) {
        const int failures = g_terminalCreationFailures.fetch_add(1, std::memory_order_relaxed) + 1;
        if (failures == policy::kMaxTerminalDeviceCreationAttempts) {
            HookLogImportant(
                "DX12: D3D12CreateDevice has failed %d times with the terminal hr=0x%08X (%s) - the temp-swapchain "
                "route stops paying for device creation; Present hooks now depend on intercepting the game's own "
                "CreateSwapChainForHwnd",
                failures, static_cast<unsigned>(hr), HrName(hr));
        }
    } else if (SUCCEEDED(hr)) {
        g_terminalCreationFailures.store(0, std::memory_order_relaxed);
    }
}

void ReportDeviceCreationFailure(HRESULT observedHr, const char* callSite) {
    {
        std::lock_guard<std::mutex> lock(g_reportMutex);
        if (!policy::ShouldEmitReport(g_reportsEmitted, g_lastReportedHr, static_cast<int32_t>(observedHr))) {
            return;
        }
        g_reportsEmitted++;
        g_lastReportedHr = static_cast<int32_t>(observedHr);
    }

    HMODULE d3d12 = ce::module_pin::PinByName("d3d12.dll");
    HMODULE dxgi = ce::module_pin::PinByName("dxgi.dll");
    if (d3d12 == nullptr || dxgi == nullptr) {
        HookLogImportant("DX12 device-creation report: d3d12/dxgi not pinnable - no report possible (hr=0x%08X)",
                         static_cast<unsigned>(observedHr));
        return;
    }
    auto create = reinterpret_cast<PFN_D3D12CreateDeviceLocal>(GetProcAddress(d3d12, "D3D12CreateDevice"));
    auto createFactory = reinterpret_cast<PFN_CreateDXGIFactory1Local>(GetProcAddress(dxgi, "CreateDXGIFactory1"));
    if (create == nullptr || createFactory == nullptr) {
        HookLogImportant("DX12 device-creation report: D3D12CreateDevice/CreateDXGIFactory1 export missing");
        return;
    }

    HookLogImportant("DX12 device-creation report: BEGIN (hr=0x%08X %s, site=%s) - why this process cannot create a "
                     "D3D12 device",
                     static_cast<unsigned>(observedHr), HrName(observedHr), (callSite && callSite[0]) ? callSite : "?");

    policy::Evidence evidence;

    HookLogImportant("DX12 device-creation report: entry integrity");
    const policy::EntryPatchKind createKind = ReportEntryIntegrity("d3d12.dll", "D3D12CreateDevice");
    ReportEntryIntegrity("d3d12.dll", "D3D12GetInterface");
    ReportEntryIntegrity("d3d12.dll", "D3D12EnableExperimentalFeatures");
    ReportEntryIntegrity("dxgi.dll", "CreateDXGIFactory1");
    ReportEntryIntegrity("dxgi.dll", "CreateDXGIFactory2");
    evidence.entryForeignPatched = policy::IsForeignEntryPatch(createKind);

    HookLogImportant("DX12 device-creation report: runtime provenance");
    ReportRuntimeProvenance();
    ReportForeignModules();

    HookLogImportant("DX12 device-creation report: adapter x feature-level matrix (S_FALSE means the device would be "
                     "created; nothing is created here)");
    ReportAdapterMatrix(create, createFactory, &evidence);

    // The call that actually failed, repeated: once as every other caller sees it, and once
    // through the unpatched body when a foreign patch owns the entry. Only a disagreement
    // between the two identifies the patch, which is why both are logged even when they agree.
    const HRESULT hookedHr = ProbeFeatureLevel(create, nullptr, D3D_FEATURE_LEVEL_11_0);
    evidence.hookedCreatedDevice = SUCCEEDED(hookedHr);
    HookLogImportant("DX12 device-creation report:   default adapter FL11_0 via the live entry -> 0x%08X (%s)",
                     static_cast<unsigned>(hookedHr), HrName(hookedHr));

    if (evidence.entryForeignPatched) {
        void* bypass = InlineHook::CreateBypassTrampoline(reinterpret_cast<void*>(create));
        if (bypass != nullptr) {
            evidence.bypassAttempted = true;
            const HRESULT bypassHr =
                ProbeFeatureLevel(reinterpret_cast<PFN_D3D12CreateDeviceLocal>(bypass), nullptr,
                                  D3D_FEATURE_LEVEL_11_0);
            evidence.bypassCreatedDevice = SUCCEEDED(bypassHr);
            HookLogImportant("DX12 device-creation report:   default adapter FL11_0 past the foreign entry patch -> "
                             "0x%08X (%s)",
                             static_cast<unsigned>(bypassHr), HrName(bypassHr));
        } else {
            HookLogImportant("DX12 device-creation report:   the foreign entry patch could not be bypassed, so the "
                             "patched and unpatched paths cannot be compared");
        }
    }

    // Only worth asking when D3D12 has already refused every hardware adapter: that is the
    // one case where the answer changes the verdict, and it keeps a healthy process from
    // paying for a D3D11 device it does not need.
    if (evidence.hardwareAdapters > 0 && evidence.hardwareAdaptersAtBaseline == 0) {
        ProbeD3D11(&evidence);
    }

    const policy::Verdict verdict = policy::Classify(evidence);
    HookLogImportant("DX12 device-creation report: VERDICT - %s", policy::Describe(verdict));
    HookLogImportant("DX12 device-creation report: END (adapters=%u hardware=%u hardwareAtBaseline=%u "
                     "belowTopFeatureLevel=%u foreignEntryPatch=%d)",
                     evidence.adapterCount, evidence.hardwareAdapters, evidence.hardwareAdaptersAtBaseline,
                     evidence.adaptersRejectingTopFeatureLevel, evidence.entryForeignPatched ? 1 : 0);
}

}  // namespace ce::dx12_device_creation_report
