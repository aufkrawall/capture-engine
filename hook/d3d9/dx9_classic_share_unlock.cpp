#include "hook/d3d9/dx9_classic_share_unlock.h"

#include "common/logging/log_meter.h"
#include "hook/d3d9/d3d9_device_lock_layout.h"
#include "hook/d3d9/d3d9_ex_flag_layout.h"
#include "hook/runtime/hook_common.h"

#include <psapi.h>

#include <atomic>
#include <cstring>
#include <cwchar>
#include <mutex>
#include <vector>

namespace {

using namespace ce::d3d9_ex_flag;
using ce::d3d9_device_lock::CheckDeviceLock;
using ce::d3d9_device_lock::LockCall;
using ce::d3d9_device_lock::LockCheck;
using ce::d3d9_device_lock::LockLayout;

// IDirect3DDevice9Ex::CheckResourceResidency: 119 IDirect3DDevice9 slots, then
// SetConvolutionMonoKernel, ComposeRects, PresentEx, Get/SetGPUThreadPriority,
// WaitForVBlank. Its first job is rejecting non-Ex devices.
constexpr size_t kCheckResourceResidencySlot = 125;

// IDirect3DDevice9::GetDirect3D; the runtime returns the factory field the Ex
// gate reads, so its result is the reference object, not a caller-held handle.
constexpr size_t kGetDirect3DSlot = 6;

constexpr CodeArch kNativeArch = sizeof(void*) == 8 ? CodeArch::X64 : CodeArch::X86;
static_assert(sizeof(CRITICAL_SECTION) == ce::d3d9_device_lock::CriticalSectionSize(kNativeArch));

class CurrentProcessReader final : public MemoryReader {
public:
    bool Read(uintptr_t address, void* out, size_t size) const override {
        SIZE_T copied = 0;
        return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(address), out, size, &copied) &&
               copied == size;
    }
};

enum class LocateState : uint8_t { NotTried, Found, Failed };

std::mutex g_locateMutex;
LocateState g_locateState = LocateState::NotTried;
Layout g_layout;
// The runtime-reported factory of CE's Ex reference device, held for the process
// lifetime: every unlock window points a classic device at it, so it must outlive
// CE's helper devices. It is never released (no COM calls during DLL detach).
IDirect3D9* g_exFactory = nullptr;
LockLayout g_lockLayout;
std::atomic<bool> g_lockKnown{false};
uintptr_t g_imageBase = 0;
uintptr_t g_imageEnd = 0;
std::atomic<bool> g_layoutKnown{false};
std::atomic<uint32_t> g_engagements{0};
ce::log_meter::ChangeGate g_declineGate;

bool InImage(const void* address) {
    const auto value = reinterpret_cast<uintptr_t>(address);
    return value >= g_imageBase && value < g_imageEnd;
}

// The object's vtable and the device's GetDirect3D must both be the runtime's:
// a wrapper factory or a proxied device is never the object the gate reads.
bool IsRuntimeObject(const void* object) {
    const void* vtable = nullptr;
    const CurrentProcessReader reader;
    return object && reader.Read(reinterpret_cast<uintptr_t>(object), &vtable, sizeof(vtable)) && InImage(vtable);
}

const void* VtableEntry(const void* object, size_t slot) {
    return (*reinterpret_cast<void* const* const*>(object))[slot];
}

const void* ExportAddress(const wchar_t* module, const char* name) {
    const HMODULE handle = GetModuleHandleW(module);
    return handle ? reinterpret_cast<const void*>(GetProcAddress(handle, name)) : nullptr;
}

// Resolves d3d9.dll import slots to the loaded critical-section entry points
// (kernel32 forwards them to ntdll, so either address can be bound).
class CriticalSectionImports final : public ce::d3d9_device_lock::ImportResolver {
public:
    CriticalSectionImports()
        : enter_{ExportAddress(L"kernel32.dll", "EnterCriticalSection"),
                 ExportAddress(L"ntdll.dll", "RtlEnterCriticalSection")},
          leave_{ExportAddress(L"kernel32.dll", "LeaveCriticalSection"),
                 ExportAddress(L"ntdll.dll", "RtlLeaveCriticalSection")} {}

    LockCall Resolve(uintptr_t slotAddress) const override {
        const void* target = nullptr;
        const CurrentProcessReader reader;
        if (!InImage(reinterpret_cast<const void*>(slotAddress)) ||
            !reader.Read(slotAddress, &target, sizeof(target)) || !target)
            return LockCall::Other;
        if (target == enter_[0] || target == enter_[1])
            return LockCall::Enter;
        if (target == leave_[0] || target == leave_[1])
            return LockCall::Leave;
        return LockCall::Other;
    }

private:
    const void* enter_[2];
    const void* leave_[2];
};

void ModuleBaseName(const void* address, char* out, size_t outSize) {
    snprintf(out, outSize, "?");
    HMODULE module = nullptr;
    if (!address || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                        reinterpret_cast<LPCWSTR>(address), &module))
        return;
    char path[MAX_PATH] = {};
    if (GetModuleFileNameA(module, path, MAX_PATH) == 0)
        return;
    const char* slash = strrchr(path, '\\');
    snprintf(out, outSize, "%s", slash ? slash + 1 : path);
}

// Reports every object the locate compared, so a mismatch names its owner.
void LogReferenceObjects(const char* moduleText, IDirect3D9Ex* heldFactory, IDirect3D9* reportedFactory,
                         uintptr_t storedFactory) {
    char heldOwner[64] = {};
    char reportedOwner[64] = {};
    char storedOwner[64] = {};
    const CurrentProcessReader reader;
    const void* heldVtable = nullptr;
    const void* reportedVtable = nullptr;
    const void* storedVtable = nullptr;
    if (heldFactory)
        reader.Read(reinterpret_cast<uintptr_t>(heldFactory), &heldVtable, sizeof(heldVtable));
    if (reportedFactory)
        reader.Read(reinterpret_cast<uintptr_t>(reportedFactory), &reportedVtable, sizeof(reportedVtable));
    if (storedFactory)
        reader.Read(storedFactory, &storedVtable, sizeof(storedVtable));
    ModuleBaseName(heldVtable, heldOwner, sizeof(heldOwner));
    ModuleBaseName(reportedVtable, reportedOwner, sizeof(reportedOwner));
    ModuleBaseName(storedVtable, storedOwner, sizeof(storedOwner));
    HookLogImportant(
        "DX9: Classic share reference objects (d3d9 %s held=%p vtbl=%s reported=%p vtbl=%s stored=%p vtbl=%s)",
        moduleText, static_cast<void*>(heldFactory), heldOwner, static_cast<void*>(reportedFactory), reportedOwner,
        reinterpret_cast<void*>(storedFactory), storedOwner);
}

bool DirectoryPrefixMatches(const wchar_t* path, const wchar_t* directory) {
    const size_t length = wcslen(directory);
    return length > 0 && _wcsnicmp(path, directory, length) == 0 && path[length] == L'\\' &&
           wcschr(path + length + 1, L'\\') == nullptr;
}

bool IsSystemDirectoryModule(HMODULE module) {
    wchar_t path[MAX_PATH] = {};
    const DWORD pathLength = GetModuleFileNameW(module, path, MAX_PATH);
    if (pathLength == 0 || pathLength >= MAX_PATH)
        return false;
    wchar_t systemDir[MAX_PATH] = {};
    wchar_t wow64Dir[MAX_PATH] = {};
    const UINT systemLength = GetSystemDirectoryW(systemDir, MAX_PATH);
    const UINT wow64Length = GetSystemWow64DirectoryW(wow64Dir, MAX_PATH);
    return (systemLength > 0 && systemLength < MAX_PATH && DirectoryPrefixMatches(path, systemDir)) ||
           (wow64Length > 0 && wow64Length < MAX_PATH && DirectoryPrefixMatches(path, wow64Dir));
}

// File version plus link timestamp identify the d3d9.dll build a miss came from.
void DescribeModule(HMODULE module, char* out, size_t outSize) {
    snprintf(out, outSize, "?");
    wchar_t path[MAX_PATH] = {};
    if (GetModuleFileNameW(module, path, MAX_PATH) == 0)
        return;
    unsigned version[4] = {};
    DWORD handle = 0;
    const DWORD size = GetFileVersionInfoSizeW(path, &handle);
    if (size > 0) {
        std::vector<BYTE> data(size);
        VS_FIXEDFILEINFO* info = nullptr;
        UINT infoLength = 0;
        if (GetFileVersionInfoW(path, handle, size, data.data()) &&
            VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&info), &infoLength) && info) {
            version[0] = HIWORD(info->dwFileVersionMS);
            version[1] = LOWORD(info->dwFileVersionMS);
            version[2] = HIWORD(info->dwFileVersionLS);
            version[3] = LOWORD(info->dwFileVersionLS);
        }
    }
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(module);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(reinterpret_cast<const BYTE*>(module) + dos->e_lfanew);
    snprintf(out, outSize, "%u.%u.%u.%u ts=0x%08lx", version[0], version[1], version[2], version[3],
             static_cast<unsigned long>(nt->FileHeader.TimeDateStamp));
}

const char* LockCallName(LockCall call) {
    return call == LockCall::Enter ? "enter" : call == LockCall::Leave ? "leave" : "other";
}

// Optional: without a proven device lock multithreaded game devices keep the fallback.
// CE's Ex reference device is multithreaded, so its lock must be live and gated.
void LocateDeviceLockLocked(IDirect3DDevice9Ex* exDevice, const char* moduleText) {
    // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization) - zero-initialized placeholder; enum fields are assigned before use
    D3DDEVICE_CREATION_PARAMETERS params = {};
    const HRESULT paramsHr = exDevice->GetCreationParameters(&params);
    if (FAILED(paramsHr) || (params.BehaviorFlags & D3DCREATE_MULTITHREADED) == 0) {
        HookLogImportant(
            "DX9: Classic share device lock not located (d3d9 %s reference device hr=0x%08x flags=0x%lx is not "
            "multithreaded); multithreaded classic devices keep the fallback",
            moduleText, static_cast<unsigned>(paramsHr), static_cast<unsigned long>(params.BehaviorFlags));
        return;
    }
    const auto address = reinterpret_cast<uintptr_t>(VtableEntry(exDevice, kGetDirect3DSlot));
    const size_t scanBytes = g_imageEnd - address < kScanBytes ? g_imageEnd - address : kScanBytes;
    uint8_t code[kScanBytes] = {};
    const CurrentProcessReader reader;
    const CriticalSectionImports imports;
    const auto decode = reader.Read(address, code, scanBytes)
                            ? ce::d3d9_device_lock::DecodeLockLayout(code, scanBytes, address, kNativeArch, imports)
                            : ce::d3d9_device_lock::LockDecode{};
    const LockCheck check = decode.found ? CheckDeviceLock(reader, decode.layout,
                                                           reinterpret_cast<uintptr_t>(exDevice), true, kNativeArch)
                                         : LockCheck::ReadFailed;
    if (!decode.found || check != LockCheck::Match) {
        HookLogImportant(
            "DX9: Classic share device lock not located (d3d9 %s decoded=%d ownerOff=0x%x gateOff=0x%x check=%s); "
            "multithreaded classic devices keep the fallback",
            moduleText, decode.found ? 1 : 0, decode.layout.ownerOffset, decode.layout.gateOffset,
            ce::d3d9_device_lock::LockCheckName(check));
        return;
    }
    g_lockLayout = decode.layout;
    g_lockKnown.store(true, std::memory_order_release);
    HookLogImportant("DX9: Classic share device lock located (d3d9 %s ownerOff=0x%x gateOff=0x%x call=%s at=0x%x)",
                     moduleText, decode.layout.ownerOffset, decode.layout.gateOffset, LockCallName(decode.call),
                     decode.gateCodeOffset);
}

bool LocateLocked(IDirect3DDevice9Ex* exDevice, IDirect3D9Ex* exFactory) {
    HMODULE d3d9 = GetModuleHandleW(L"d3d9.dll");
    if (!d3d9) {
        HookLogImportant("DX9: Classic share layout not located (d3d9.dll not loaded)");
        return false;
    }
    char moduleText[96] = {};
    DescribeModule(d3d9, moduleText, sizeof(moduleText));
    if (!IsSystemDirectoryModule(d3d9)) {
        HookLogImportant("DX9: Classic share layout not located (d3d9.dll %s is not the system runtime)", moduleText);
        return false;
    }

    void** vtable = *reinterpret_cast<void***>(exDevice);
    const auto* function = static_cast<const uint8_t*>(vtable[kCheckResourceResidencySlot]);
    HMODULE owner = nullptr;
    MODULEINFO moduleInfo = {};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(function), &owner) ||
        owner != d3d9 || !GetModuleInformation(GetCurrentProcess(), d3d9, &moduleInfo, sizeof(moduleInfo))) {
        HookLogImportant("DX9: Classic share layout not located (d3d9 %s CheckResourceResidency=%p is outside d3d9.dll)",
                         moduleText, function);
        return false;
    }

    g_imageBase = reinterpret_cast<uintptr_t>(moduleInfo.lpBaseOfDll);
    g_imageEnd = g_imageBase + moduleInfo.SizeOfImage;
    const uintptr_t imageEnd = g_imageEnd;
    const auto functionAddress = reinterpret_cast<uintptr_t>(function);
    const size_t scanBytes = imageEnd - functionAddress < kScanBytes ? imageEnd - functionAddress : kScanBytes;
    uint8_t code[kScanBytes] = {};
    const CurrentProcessReader reader;
    if (!reader.Read(functionAddress, code, scanBytes)) {
        HookLogImportant("DX9: Classic share layout not located (d3d9 %s code read failed)", moduleText);
        return false;
    }

    // The reference factory is what the runtime itself returns for the Ex device.
    // A handle CE holds can be an interposer's object when another module hooks
    // Direct3DCreate9Ex or CreateDeviceEx; the gate never reads that one.
    IDirect3D9* reportedFactory = nullptr;
    const HRESULT reportedHr = InImage(VtableEntry(exDevice, kGetDirect3DSlot))
                                   ? exDevice->GetDirect3D(&reportedFactory)
                                   : E_NOINTERFACE;
    if (FAILED(reportedHr) || !reportedFactory || !IsRuntimeObject(reportedFactory)) {
        LogReferenceObjects(moduleText, exFactory, reportedFactory, 0);
        HookLogImportant("DX9: Classic share layout not located (d3d9 %s reference factory unusable hr=0x%08x)",
                         moduleText, static_cast<unsigned>(reportedHr));
        if (reportedFactory)
            reportedFactory->Release();
        return false;
    }

    Candidate candidates[kMaxCandidates] = {};
    const size_t candidateCount = FindCandidates(code, scanBytes, kNativeArch, candidates, kMaxCandidates);
    const Selection selection =
        SelectLayout(reader, candidates, candidateCount, reinterpret_cast<uintptr_t>(exDevice),
                     reinterpret_cast<uintptr_t>(reportedFactory));
    uintptr_t storedFactory = 0;
    if (candidateCount > 0) {
        const Layout& probe = selection.found ? selection.layout : candidates[0].layout;
        reader.Read(reinterpret_cast<uintptr_t>(exDevice) + probe.factoryFieldOffset, &storedFactory,
                    sizeof(storedFactory));
    }
    if (!selection.found || reportedFactory != static_cast<IDirect3D9*>(exFactory))
        LogReferenceObjects(moduleText, exFactory, reportedFactory, storedFactory);
    if (!selection.found) {
        reportedFactory->Release();
        HookLogImportant(
            "DX9: Classic share layout not located (d3d9 %s candidates=%zu rejected=%zu lastReject=%s); classic "
            "capture keeps its fallback",
            moduleText, candidateCount, selection.rejected, ObjectCheckName(selection.lastReject));
        return false;
    }

    g_layout = selection.layout;
    if (g_exFactory)
        g_exFactory->Release();
    g_exFactory = reportedFactory;
    HookLogImportant(
        "DX9: Classic share layout located (d3d9 %s candidates=%zu rejected=%zu factoryOff=0x%x exFlagOff=0x%x "
        "exFactory=%p)",
        moduleText, candidateCount, selection.rejected, selection.layout.factoryFieldOffset,
        selection.layout.exFlagOffset, static_cast<void*>(g_exFactory));
    LocateDeviceLockLocked(exDevice, moduleText);
    return true;
}

void LogDecline(const char* label, const char* reason, unsigned detail) {
    const auto verdict = g_declineGate.Observe(ce::log_meter::FieldKey(reason, detail));
    if (verdict) {
        HookLogImportant("DX9: Classic share unlock declined for %s (%s detail=0x%x repeats=%llu)",
                         label ? label : "?", reason, detail, static_cast<unsigned long long>(verdict.suppressed));
    }
}

// The window answers the runtime's adapter queries during resource creation
// (CheckDeviceFormat, per-adapter tables) from CE's reference factory, so both
// factories must describe the device's adapter identically.
bool AdaptersAgree(IDirect3D9* factory, UINT adapter) {
    const UINT count = factory->GetAdapterCount();
    if (adapter >= count || g_exFactory->GetAdapterCount() != count)
        return false;
    const HMONITOR monitor = factory->GetAdapterMonitor(adapter);
    if (!monitor || g_exFactory->GetAdapterMonitor(adapter) != monitor)
        return false;
    D3DADAPTER_IDENTIFIER9 gameId = {};
    D3DADAPTER_IDENTIFIER9 exId = {};
    return SUCCEEDED(factory->GetAdapterIdentifier(adapter, 0, &gameId)) &&
           SUCCEEDED(g_exFactory->GetAdapterIdentifier(adapter, 0, &exId)) && gameId.VendorId == exId.VendorId &&
           gameId.DeviceId == exId.DeviceId && gameId.DeviceIdentifier == exId.DeviceIdentifier;
}

}  // namespace

bool DX9_LocateClassicShareLayout(IDirect3DDevice9Ex* exDevice, IDirect3D9Ex* exFactory) {
    std::lock_guard<std::mutex> lock(g_locateMutex);
    if (g_locateState == LocateState::NotTried && exDevice && exFactory) {
        g_locateState = LocateLocked(exDevice, exFactory) ? LocateState::Found : LocateState::Failed;
        g_layoutKnown.store(g_locateState == LocateState::Found, std::memory_order_release);
    }
    return g_locateState == LocateState::Found;
}

DX9ClassicShareUnlockScope::DX9ClassicShareUnlockScope(IDirect3DDevice9* device, const IDirect3D9* ceOwnedFactory,
                                                       const char* label)
    : label_(label) {
    if (!device)
        return;

    IDirect3DDevice9Ex* deviceEx = nullptr;
    const bool isEx = SUCCEEDED(device->QueryInterface(__uuidof(IDirect3DDevice9Ex), reinterpret_cast<void**>(&deviceEx))) &&
                      deviceEx;
    if (deviceEx)
        deviceEx->Release();
    if (isEx)
        return;

    if (!g_layoutKnown.load(std::memory_order_acquire)) {
        LogDecline(label, UnlockDecisionName(UnlockDecision::NoLayout), 0);
        return;
    }
    if (!InImage(VtableEntry(device, kGetDirect3DSlot))) {
        LogDecline(label, "device-not-runtime", 0);
        return;
    }
    IDirect3D9* factory = nullptr;
    const HRESULT factoryHr = device->GetDirect3D(&factory);
    if (FAILED(factoryHr) || !factory) {
        LogDecline(label, "GetDirect3D-failed", static_cast<unsigned>(factoryHr));
        return;
    }
    if (!IsRuntimeObject(factory)) {
        LogDecline(label, "factory-not-runtime", 0);
        factory->Release();
        return;
    }

    // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization) - zero-initialized placeholder; enum fields are assigned before use
    D3DDEVICE_CREATION_PARAMETERS params = {};
    const HRESULT paramsHr = device->GetCreationParameters(&params);
    if (FAILED(paramsHr)) {
        LogDecline(label, "creation-params-failed", static_cast<unsigned>(paramsHr));
        factory->Release();
        return;
    }
    const bool multithreaded = (params.BehaviorFlags & D3DCREATE_MULTITHREADED) != 0;
    const bool lockKnown = g_lockKnown.load(std::memory_order_acquire);
    const UnlockDecision decision = DecideUnlock({.layoutKnown = true,
                                                  .multithreaded = multithreaded,
                                                  .deviceLockKnown = lockKnown,
                                                  .ceOwnedFactory = factory == ceOwnedFactory});
    if (decision != UnlockDecision::Unlock) {
        LogDecline(label, UnlockDecisionName(decision), static_cast<unsigned>(params.BehaviorFlags));
        factory->Release();
        return;
    }

    const CurrentProcessReader reader;
    const ObjectCheck check = CheckObjects(reader, g_layout, reinterpret_cast<uintptr_t>(device),
                                           reinterpret_cast<uintptr_t>(factory), 0);
    if (check != ObjectCheck::Match) {
        LogDecline(label, ObjectCheckName(check), 0);
        factory->Release();
        return;
    }
    // The runtime's own gate, not the creation flags, decides whether it locks; both must agree.
    const LockCheck lockCheck = lockKnown ? CheckDeviceLock(reader, g_lockLayout, reinterpret_cast<uintptr_t>(device),
                                                            multithreaded, kNativeArch)
                                          : LockCheck::Match;
    if (lockCheck != LockCheck::Match) {
        LogDecline(label, ce::d3d9_device_lock::LockCheckName(lockCheck), static_cast<unsigned>(params.BehaviorFlags));
        factory->Release();
        return;
    }
    if (!AdaptersAgree(factory, params.AdapterOrdinal)) {
        LogDecline(label, "adapter-mismatch", params.AdapterOrdinal);
        factory->Release();
        return;
    }

    auto* deviceLock =
        lockKnown && multithreaded
            ? reinterpret_cast<CRITICAL_SECTION*>(reinterpret_cast<BYTE*>(device) + g_lockLayout.ownerOffset)
            : nullptr;
    const uint32_t engagement = g_engagements.fetch_add(1, std::memory_order_relaxed) + 1;
    if (engagement <= 4 || engagement % 64 == 0) {
        HookLogImportant(
            "DX9: Classic share unlock engaged for %s (factory=%p exFactory=%p ceOwned=%d count=%u multithreaded=%d "
            "locked=%d)",
            label ? label : "?", static_cast<void*>(factory), static_cast<void*>(g_exFactory),
            factory == ceOwnedFactory ? 1 : 0, engagement, multithreaded ? 1 : 0, deviceLock ? 1 : 0);
    }

    // Nothing is logged inside the window: application threads wait on this lock.
    if (deviceLock)
        EnterCriticalSection(deviceLock);
    auto* field = reinterpret_cast<void* volatile*>(reinterpret_cast<BYTE*>(device) + g_layout.factoryFieldOffset);
    if (InterlockedCompareExchangePointer(field, g_exFactory, factory) != factory) {
        if (deviceLock)
            LeaveCriticalSection(deviceLock);
        LogDecline(label, "factory-changed", 0);
        factory->Release();
        return;
    }
    factoryField_ = field;
    deviceLock_ = deviceLock;
    factory_ = factory;
}

DX9ClassicShareUnlockScope::~DX9ClassicShareUnlockScope() {
    const bool restored =
        !factoryField_ || InterlockedCompareExchangePointer(factoryField_, factory_, g_exFactory) == g_exFactory;
    if (deviceLock_)
        LeaveCriticalSection(deviceLock_);
    if (!restored)
        HookLogImportant("DX9: Classic share unlock restore found a changed factory field for %s", label_ ? label_ : "?");
    if (factory_)
        factory_->Release();
}
