#include "nv_lod_spread_override.h"

#include <atomic>
#include <cctype>
#include <cstring>

#include "../wrappers/inline_hook.h"
#include "hook_common.h"

namespace ce::nv_lod_spread {

namespace {

// Vulkan and WGL entry points CE re-checks the setting from. Both are exported by
// ordinary system modules, so no driver code is touched to obtain the anchor.
using PfnVkCreateDevice = int32_t(__stdcall*)(void*, const void*, const void*, void**);
using PfnWglMakeCurrent = BOOL(__stdcall*)(HDC, HGLRC);

// VK_ERROR_INITIALIZATION_FAILED, for the defensive path where a detour runs
// before its trampoline is published.
constexpr int32_t kVkErrorInitializationFailed = -3;

std::atomic<Mode> g_mode{Mode::kOff};
std::atomic<bool> g_moduleSeen{false};
std::atomic<bool> g_dataWriteApplied{false};
std::atomic<bool> g_codeFallbackApplied{false};
std::atomic<bool> g_codeFallbackAttempted{false};
std::atomic<uint32_t> g_clobbers{0};

std::atomic<HMODULE> g_patchedModule{nullptr};
std::atomic<uint32_t*> g_settingAddress{nullptr};
std::atomic<uint8_t*> g_branchAddress{nullptr};
std::atomic<size_t> g_branchLength{0};
std::atomic<bool> g_anchorsInstalled{false};

std::atomic<PfnVkCreateDevice> g_origVkCreateDevice{nullptr};
std::atomic<PfnWglMakeCurrent> g_origWglMakeCurrent{nullptr};

std::string Trimmed(const std::string& value) {
    size_t first = 0;
    while (first < value.size() && std::isspace(static_cast<unsigned char>(value[first]))) {
        ++first;
    }
    size_t last = value.size();
    while (last > first && std::isspace(static_cast<unsigned char>(value[last - 1]))) {
        --last;
    }
    std::string out = value.substr(first, last - first);
    for (char& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

const char* BaseName(const char* path) {
    if (!path) {
        return nullptr;
    }
    const char* base = path;
    for (const char* cursor = path; *cursor; ++cursor) {
        if (*cursor == '\\' || *cursor == '/') {
            base = cursor + 1;
        }
    }
    return base;
}

bool WriteProtectedMemory(void* address, const void* data, size_t size, bool executable) {
    DWORD oldProtect = 0;
    const DWORD wanted = executable ? PAGE_EXECUTE_READWRITE : PAGE_READWRITE;
    if (!VirtualProtect(address, size, wanted, &oldProtect)) {
        return false;
    }
    memcpy(address, data, size);
    DWORD restored = 0;
    VirtualProtect(address, size, oldProtect, &restored);
    if (executable) {
        FlushInstructionCache(GetCurrentProcess(), address, size);
    }
    return true;
}

// Locates the mapped image's bounds and its executable section. The ICD always
// matches CE's own bitness - a 64-bit driver cannot map into a 32-bit game - so a
// header that does not describe a native image is refused rather than guessed at.
bool DescribeModule(HMODULE module, const uint8_t*& image, size_t& imageSize, size_t& textRva, size_t& textSize,
                    bool& is64Bit) {
    const auto* base = reinterpret_cast<const uint8_t*>(module);
    if (!base) {
        return false;
    }
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) {
        return false;
    }
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR_MAGIC) {
        return false;
    }
#ifdef _WIN64
    is64Bit = nt->FileHeader.Machine == IMAGE_FILE_MACHINE_AMD64;
#else
    is64Bit = false;
    if (nt->FileHeader.Machine != IMAGE_FILE_MACHINE_I386) {
        return false;
    }
#endif

    image = base;
    imageSize = nt->OptionalHeader.SizeOfImage;

    const IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
        if ((section->Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0) {
            continue;
        }
        textRva = section->VirtualAddress;
        textSize = section->Misc.VirtualSize ? section->Misc.VirtualSize : section->SizeOfRawData;
        if (textRva + textSize > imageSize) {
            textSize = imageSize - textRva;
        }
        return textSize > kCmpLength;
    }
    return false;
}

bool WriteSettingValue(uint32_t* setting, uint32_t value) {
    return WriteProtectedMemory(setting, &value, sizeof(value), /*executable=*/false);
}

// NOPs the validated branch so the ON path is reached by fall-through. Only ever
// reached after the driver was observed overwriting CE's data write.
void ApplyCodeFallback(const char* reason) {
    if (g_codeFallbackAttempted.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    uint8_t* branch = g_branchAddress.load(std::memory_order_acquire);
    const size_t length = g_branchLength.load(std::memory_order_acquire);
    if (!branch || length == 0) {
        HookLogImportant(
            "NV LOD spread: driver overwrote the setting and no validated branch site is known (%s) - the data "
            "write is re-applied but cannot be made permanent",
            reason ? reason : "unknown");
        return;
    }

    uint8_t nops[8];
    memset(nops, 0x90, sizeof(nops));
    if (length > sizeof(nops) || !WriteProtectedMemory(branch, nops, length, /*executable=*/true)) {
        HookLogImportant("NV LOD spread: code fallback write FAILED at %p (%zu bytes, %s)", branch, length,
                         reason ? reason : "unknown");
        return;
    }
    g_codeFallbackApplied.store(true, std::memory_order_release);
    HookLogImportant(
        "NV LOD spread: code fallback applied - NOPed the OFF branch at %p (%zu bytes) because the driver "
        "overwrote CE's setting value (%s)",
        branch, length, reason ? reason : "unknown");
}

int32_t __stdcall Detour_vkCreateDevice(void* physicalDevice, const void* createInfo, const void* allocator,
                                        void** device) {
    const PfnVkCreateDevice original = g_origVkCreateDevice.load(std::memory_order_acquire);
    if (!original) {
        return kVkErrorInitializationFailed;
    }
    // Entry covers a settings load that ran during instance creation; the return
    // covers a driver that defers it into device creation. Both still precede any
    // sampler or pipeline, which is where the filtering record is built.
    VerifyAndReassert("vkCreateDevice entry");
    const int32_t result = original(physicalDevice, createInfo, allocator, device);
    VerifyAndReassert("vkCreateDevice return");
    return result;
}

BOOL __stdcall Detour_wglMakeCurrent(HDC dc, HGLRC context) {
    const PfnWglMakeCurrent original = g_origWglMakeCurrent.load(std::memory_order_acquire);
    if (!original) {
        return FALSE;
    }
    const BOOL result = original(dc, context);
    VerifyAndReassert("wglMakeCurrent");
    return result;
}

// The ICD is mapped from inside the caller's vkCreateInstance / context creation,
// so the anchors deliberately target the *next* creation step rather than the one
// already executing.
void InstallReassertAnchors() {
    if (g_anchorsInstalled.exchange(true, std::memory_order_acq_rel)) {
        return;
    }

    if (HMODULE loader = GetModuleHandleA("vulkan-1.dll")) {
        if (void* target = reinterpret_cast<void*>(GetProcAddress(loader, "vkCreateDevice"))) {
            void* trampoline = nullptr;
            if (InlineHook::Install(target, reinterpret_cast<void*>(&Detour_vkCreateDevice), &trampoline) &&
                trampoline) {
                g_origVkCreateDevice.store(reinterpret_cast<PfnVkCreateDevice>(trampoline), std::memory_order_release);
                HookLogImportant("NV LOD spread: re-check anchor installed on vulkan-1.dll!vkCreateDevice");
            } else {
                HookLogImportant("NV LOD spread: FAILED to anchor on vulkan-1.dll!vkCreateDevice");
            }
        }
    }

    if (HMODULE gl = GetModuleHandleA("opengl32.dll")) {
        if (void* target = reinterpret_cast<void*>(GetProcAddress(gl, "wglMakeCurrent"))) {
            void* trampoline = nullptr;
            if (InlineHook::Install(target, reinterpret_cast<void*>(&Detour_wglMakeCurrent), &trampoline) &&
                trampoline) {
                g_origWglMakeCurrent.store(reinterpret_cast<PfnWglMakeCurrent>(trampoline), std::memory_order_release);
                HookLogImportant("NV LOD spread: re-check anchor installed on opengl32.dll!wglMakeCurrent");
            } else {
                HookLogImportant("NV LOD spread: FAILED to anchor on opengl32.dll!wglMakeCurrent");
            }
        }
    }
}

bool ApplyToModule(HMODULE module, const char* modulePath) {
    if (g_mode.load(std::memory_order_acquire) != Mode::kOn || !module) {
        return false;
    }
    // Re-arming and repeated load notifications must not re-scan a 22 MB .text
    // section. Once this module is known, the cheap re-check is the right work.
    if (g_patchedModule.load(std::memory_order_acquire) == module) {
        VerifyAndReassert("re-arm");
        return true;
    }

    const uint8_t* image = nullptr;
    size_t imageSize = 0;
    size_t textRva = 0;
    size_t textSize = 0;
    bool is64Bit = false;
    if (!DescribeModule(module, image, imageSize, textRva, textSize, is64Bit)) {
        HookLogImportant("NV LOD spread: %s is not a usable image - not patched",
                         modulePath ? modulePath : "the ICD");
        return false;
    }

    Site site;
    if (!FindSettingSite(image, imageSize, textRva, textSize, is64Bit, reinterpret_cast<uintptr_t>(image), site)) {
        HookLogImportant(
            "NV LOD spread: no FERMI_UNOPT_LOD_SPREAD check found in %s - the driver layout changed, nothing "
            "patched",
            modulePath ? modulePath : "the ICD");
        return false;
    }

    auto* setting = const_cast<uint32_t*>(reinterpret_cast<const uint32_t*>(image + site.settingRva));
    g_settingAddress.store(setting, std::memory_order_release);
    if (site.branchFound) {
        g_branchAddress.store(const_cast<uint8_t*>(image + site.branchRva), std::memory_order_release);
        g_branchLength.store(site.branchLength, std::memory_order_release);
    }
    g_moduleSeen.store(true, std::memory_order_release);
    g_patchedModule.store(module, std::memory_order_release);

    if (site.settingValue == kSettingOn) {
        // A driver patched on disk, or a profile that already enables the setting.
        g_dataWriteApplied.store(true, std::memory_order_release);
        HookLogImportant("NV LOD spread: %s already reports the setting ON - nothing to change",
                         modulePath ? modulePath : "the ICD");
        InstallReassertAnchors();
        return true;
    }

    if (!WriteSettingValue(setting, kSettingOn)) {
        HookLogImportant("NV LOD spread: failed to write the setting value at %p in %s", setting,
                         modulePath ? modulePath : "the ICD");
        return false;
    }
    g_dataWriteApplied.store(true, std::memory_order_release);
    HookLogImportant(
        "NV LOD spread: forced FERMI_UNOPT_LOD_SPREAD ON in %s (setting %p 0x%08X -> 0x%08X, check at +0x%zX, "
        "fallback branch %s) - process-local, the driver file is untouched",
        modulePath ? modulePath : "the ICD", setting, site.settingValue, kSettingOn, site.cmpRva,
        site.branchFound ? "validated" : "UNAVAILABLE");

    InstallReassertAnchors();
    return true;
}

}  // namespace

Mode ParseMode(const std::string& configValue) {
    const std::string value = Trimmed(configValue);
    if (value == "on" || value == "1" || value == "true" || value == "enabled") {
        return Mode::kOn;
    }
    return Mode::kOff;
}

const char* GetModeName(Mode mode) {
    return mode == Mode::kOn ? "on" : "off";
}

bool IsIcdModuleName(const char* modulePathOrName) {
    const char* base = BaseName(modulePathOrName);
    if (!base || !*base) {
        return false;
    }
    return _stricmp(base, "nvoglv64.dll") == 0 || _stricmp(base, "nvoglv32.dll") == 0;
}

bool FindTableLoadDisp(const uint8_t* path, size_t pathSize, uint8_t& outDisp) {
    if (!path) {
        return false;
    }
    for (size_t i = 0; i + 2 < pathSize; ++i) {
        size_t cursor = i;
        // An optional REX prefix selects the extended registers the 64-bit driver
        // uses (mov r8d, ...); the 32-bit driver has none.
        if (path[cursor] >= 0x40 && path[cursor] <= 0x4F) {
            ++cursor;
            if (cursor + 2 >= pathSize) {
                return false;
            }
        }
        if (path[cursor] != 0x8B) {
            continue;
        }
        const uint8_t modrm = path[cursor + 1];
        if ((modrm & 0xC0) != 0x40) {
            continue;  // not [reg+disp8]
        }
        if ((modrm & 0x07) == 0x04) {
            continue;  // a SIB byte would shift the displacement; stay unambiguous
        }
        outDisp = path[cursor + 2];
        return true;
    }
    return false;
}

bool FindOnBranch(const uint8_t* image, size_t imageSize, size_t cmpRva, Site& out) {
    if (!image) {
        return false;
    }
    const size_t searchStart = cmpRva + kCmpLength;
    for (size_t offset = searchStart; offset < searchStart + kBranchSearchSpan; ++offset) {
        if (offset + 2 > imageSize) {
            return false;
        }
        const uint8_t opcode = image[offset];
        if (opcode < 0x70 || opcode > 0x7F) {
            continue;  // only the short jcc encoding the shipped drivers use
        }

        const size_t fallthrough = offset + 2;
        const auto displacement = static_cast<int8_t>(image[offset + 1]);
        const int64_t target = static_cast<int64_t>(fallthrough) + displacement;
        if (target < 0 || static_cast<size_t>(target) + kPathWindow > imageSize ||
            fallthrough + kPathWindow > imageSize) {
            continue;
        }

        uint8_t fallDisp = 0;
        uint8_t targetDisp = 0;
        if (!FindTableLoadDisp(image + fallthrough, kPathWindow, fallDisp) ||
            !FindTableLoadDisp(image + static_cast<size_t>(target), kPathWindow, targetDisp)) {
            continue;
        }
        // The ON slot is the higher of two neighbouring table entries, and forcing
        // ON by NOPing is only correct when fall-through is the ON path.
        if (fallDisp != static_cast<uint8_t>(targetDisp + 4)) {
            continue;
        }

        out.branchFound = true;
        out.branchRva = offset;
        out.branchLength = 2;
        return true;
    }
    return false;
}

bool FindSettingSite(const uint8_t* image, size_t imageSize, size_t textRva, size_t textSize, bool is64Bit,
                     uintptr_t imageBase, Site& out) {
    out = Site{};
    if (!image || textSize < kCmpLength || textRva + textSize > imageSize) {
        return false;
    }

    const size_t scanEnd = textRva + textSize - kCmpLength;
    for (size_t offset = textRva; offset <= scanEnd; ++offset) {
        if (image[offset] != kCmpOpcode0 || image[offset + 1] != kCmpOpcode1) {
            continue;
        }
        uint32_t immediate = 0;
        memcpy(&immediate, image + offset + 6, sizeof(immediate));
        if (immediate != kSettingOn) {
            continue;
        }

        size_t settingRva = 0;
        if (is64Bit) {
            int32_t displacement = 0;
            memcpy(&displacement, image + offset + 2, sizeof(displacement));
            const int64_t resolved = static_cast<int64_t>(offset) + static_cast<int64_t>(kCmpLength) + displacement;
            if (resolved < 0 || static_cast<uint64_t>(resolved) + sizeof(uint32_t) > imageSize) {
                continue;
            }
            settingRva = static_cast<size_t>(resolved);
        } else {
            uint32_t absolute = 0;
            memcpy(&absolute, image + offset + 2, sizeof(absolute));
            if (absolute < imageBase) {
                continue;
            }
            const uint64_t resolved = static_cast<uint64_t>(absolute) - imageBase;
            if (resolved + sizeof(uint32_t) > imageSize) {
                continue;
            }
            settingRva = static_cast<size_t>(resolved);
        }

        uint32_t settingValue = 0;
        memcpy(&settingValue, image + settingRva, sizeof(settingValue));
        // Self-validation: the resolved address has to actually hold one of the
        // two documented enum payloads, which is what proves this is the setting.
        if (settingValue != kSettingOn && settingValue != kSettingOff) {
            continue;
        }

        out.found = true;
        out.cmpRva = offset;
        out.settingRva = settingRva;
        out.settingValue = settingValue;
        FindOnBranch(image, imageSize, offset, out);
        return true;
    }
    return false;
}

Status GetStatus() {
    Status status;
    status.armed = g_mode.load(std::memory_order_acquire) == Mode::kOn;
    status.moduleSeen = g_moduleSeen.load(std::memory_order_acquire);
    status.dataWriteApplied = g_dataWriteApplied.load(std::memory_order_acquire);
    status.codeFallbackApplied = g_codeFallbackApplied.load(std::memory_order_acquire);
    status.clobbersObserved = g_clobbers.load(std::memory_order_acquire);
    return status;
}

Mode GetMode() {
    return g_mode.load(std::memory_order_acquire);
}

void VerifyAndReassert(const char* reason) {
    if (g_mode.load(std::memory_order_acquire) != Mode::kOn) {
        return;
    }
    uint32_t* setting = g_settingAddress.load(std::memory_order_acquire);
    if (!setting) {
        return;
    }

    uint32_t current = 0;
    memcpy(&current, setting, sizeof(current));
    if (current == kSettingOn) {
        return;
    }

    const uint32_t clobbers = g_clobbers.fetch_add(1, std::memory_order_acq_rel) + 1;
    const bool rewritten = WriteSettingValue(setting, kSettingOn);
    HookLogImportant(
        "NV LOD spread: the driver overwrote the setting with 0x%08X at %s (occurrence %u) - re-applied %s",
        current, reason ? reason : "unknown", clobbers, rewritten ? "successfully" : "FAILED");

    // The driver owns this global and has now proven it writes it. Re-applying the
    // value only fixes the window CE can observe, so make the ON path structural.
    ApplyCodeFallback(reason);
}

void OnModuleLoaded(HMODULE module, const char* modulePath) {
    if (g_mode.load(std::memory_order_acquire) != Mode::kOn || !IsIcdModuleName(modulePath)) {
        return;
    }
    ApplyToModule(module, modulePath);
}

bool Install(Mode mode) {
    const Mode previous = g_mode.exchange(mode, std::memory_order_acq_rel);
    if (mode != Mode::kOn) {
        if (previous != mode) {
            HookLogImportant("NV LOD spread: override disabled (nv_lod_spread_fix=%s)", GetModeName(mode));
        }
        return false;
    }
    if (previous != mode) {
        HookLogImportant("NV LOD spread: override enabled (nv_lod_spread_fix=on)");
    }

    // Covers a late injection whose ICD is already mapped, and a hot config
    // reload in a process that is already rendering.
    bool patched = false;
    for (const char* name : {"nvoglv64.dll", "nvoglv32.dll"}) {
        if (HMODULE module = GetModuleHandleA(name)) {
            patched = ApplyToModule(module, name) || patched;
        }
    }
    return patched;
}

}  // namespace ce::nv_lod_spread
