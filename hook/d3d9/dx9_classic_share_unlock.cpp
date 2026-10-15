#include "hook/d3d9/dx9_classic_share_unlock.h"

#include "common/logging/log_meter.h"
#include "hook/d3d9/d3d9_ex_flag_layout.h"
#include "hook/runtime/hook_common.h"

#include <psapi.h>

#include <atomic>
#include <cwchar>
#include <mutex>
#include <vector>

namespace {

using namespace ce::d3d9_ex_flag;

// IDirect3DDevice9Ex::CheckResourceResidency: 119 IDirect3DDevice9 slots, then
// SetConvolutionMonoKernel, ComposeRects, PresentEx, Get/SetGPUThreadPriority,
// WaitForVBlank. Its first job is rejecting non-Ex devices.
constexpr size_t kCheckResourceResidencySlot = 125;

constexpr CodeArch kNativeArch = sizeof(void*) == 8 ? CodeArch::X64 : CodeArch::X86;

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
std::atomic<bool> g_layoutKnown{false};
std::atomic<uint32_t> g_engagements{0};
ce::log_meter::ChangeGate g_declineGate;

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

    const auto imageEnd = reinterpret_cast<uintptr_t>(moduleInfo.lpBaseOfDll) + moduleInfo.SizeOfImage;
    const auto functionAddress = reinterpret_cast<uintptr_t>(function);
    const size_t scanBytes = imageEnd - functionAddress < kScanBytes ? imageEnd - functionAddress : kScanBytes;
    uint8_t code[kScanBytes] = {};
    const CurrentProcessReader reader;
    if (!reader.Read(functionAddress, code, scanBytes)) {
        HookLogImportant("DX9: Classic share layout not located (d3d9 %s code read failed)", moduleText);
        return false;
    }

    Candidate candidates[kMaxCandidates] = {};
    const size_t candidateCount = FindCandidates(code, scanBytes, kNativeArch, candidates, kMaxCandidates);
    const Selection selection =
        SelectLayout(reader, candidates, candidateCount, reinterpret_cast<uintptr_t>(exDevice),
                     reinterpret_cast<uintptr_t>(exFactory));
    if (!selection.found) {
        HookLogImportant(
            "DX9: Classic share layout not located (d3d9 %s candidates=%zu rejected=%zu lastReject=%s); classic "
            "capture keeps its fallback",
            moduleText, candidateCount, selection.rejected, ObjectCheckName(selection.lastReject));
        return false;
    }

    g_layout = selection.layout;
    HookLogImportant(
        "DX9: Classic share layout located (d3d9 %s candidates=%zu rejected=%zu factoryOff=0x%x exFlagOff=0x%x)",
        moduleText, candidateCount, selection.rejected, selection.layout.factoryFieldOffset,
        selection.layout.exFlagOffset);
    return true;
}

void LogDecline(const char* label, const char* reason, unsigned detail) {
    const auto verdict = g_declineGate.Observe(ce::log_meter::FieldKey(reason, detail));
    if (verdict) {
        HookLogImportant("DX9: Classic share unlock declined for %s (%s detail=0x%x repeats=%llu)",
                         label ? label : "?", reason, detail, static_cast<unsigned long long>(verdict.suppressed));
    }
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

    IDirect3D9* factory = nullptr;
    const HRESULT factoryHr = device->GetDirect3D(&factory);
    if (FAILED(factoryHr) || !factory) {
        LogDecline(label, "GetDirect3D-failed", static_cast<unsigned>(factoryHr));
        return;
    }

    // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization) - zero-initialized placeholder; enum fields are assigned before use
    D3DDEVICE_CREATION_PARAMETERS params = {};
    const bool multithreaded =
        FAILED(device->GetCreationParameters(&params)) || (params.BehaviorFlags & D3DCREATE_MULTITHREADED) != 0;
    const UnlockDecision decision =
        DecideUnlock(false, g_layoutKnown.load(std::memory_order_acquire), multithreaded, factory == ceOwnedFactory);
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

    auto* flag = reinterpret_cast<volatile LONG*>(reinterpret_cast<BYTE*>(factory) + g_layout.exFlagOffset);
    if (InterlockedCompareExchange(flag, TRUE, FALSE) != FALSE) {
        LogDecline(label, "flag-changed", 0);
        factory->Release();
        return;
    }

    factory_ = factory;
    flag_ = flag;
    const uint32_t engagement = g_engagements.fetch_add(1, std::memory_order_relaxed) + 1;
    if (engagement <= 4 || engagement % 64 == 0) {
        HookLogImportant("DX9: Classic share unlock engaged for %s (factory=%p ceOwned=%d count=%u)",
                         label ? label : "?", static_cast<void*>(factory), factory == ceOwnedFactory ? 1 : 0,
                         engagement);
    }
}

DX9ClassicShareUnlockScope::~DX9ClassicShareUnlockScope() {
    if (flag_ && InterlockedCompareExchange(flag_, FALSE, TRUE) != TRUE) {
        HookLogImportant("DX9: Classic share unlock restore found a changed flag for %s", label_ ? label_ : "?");
    }
    if (factory_)
        factory_->Release();
}
