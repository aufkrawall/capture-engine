#include "nv_lod_spread_override.h"

#include <atomic>
#include <cctype>
#include <cstring>
#include <cwchar>

#ifdef VK_LAYER_CE_OVERLAY
#include "../vulkan_layer/layer_main.h"
#define NV_LOD_LOG(...) LayerLog(__VA_ARGS__)
#else
#include "hook_common.h"
#define NV_LOD_LOG(...) HookLogImportant(__VA_ARGS__)
#endif

namespace ce::nv_lod_spread {

namespace {

static_assert(sizeof(LONG) == 4);
static_assert(sizeof(LONG64) == 8);

std::atomic<Mode> g_mode{Mode::kOff};
std::atomic<bool> g_moduleSeen{false};
std::atomic<bool> g_codePatchApplied{false};
std::atomic<bool> g_branchWasAlreadyPatched{false};
std::atomic<HMODULE> g_patchedModule{nullptr};
std::atomic<uint8_t*> g_branchAddress{nullptr};

class ProcessPatchMutex {
   public:
    ProcessPatchMutex() {
        wchar_t name[64] = {};
        _snwprintf_s(name, _TRUNCATE, L"Local\\CaptureEngine-NvLodSpread-%lu",
                     static_cast<unsigned long>(GetCurrentProcessId()));
        handle_ = CreateMutexW(nullptr, FALSE, name);
        if (!handle_) {
            return;
        }
        const DWORD waitResult = WaitForSingleObject(handle_, INFINITE);
        acquired_ = waitResult == WAIT_OBJECT_0 || waitResult == WAIT_ABANDONED;
    }

    ~ProcessPatchMutex() {
        if (acquired_) {
            ReleaseMutex(handle_);
        }
        if (handle_) {
            CloseHandle(handle_);
        }
    }

    ProcessPatchMutex(const ProcessPatchMutex&) = delete;
    ProcessPatchMutex& operator=(const ProcessPatchMutex&) = delete;

    bool Acquired() const { return acquired_; }

   private:
    HANDLE handle_ = nullptr;
    bool acquired_ = false;
};

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

uint64_t AtomicCompareExchangeWord(void* word, AtomicPatchWidth width, uint64_t exchange, uint64_t comparand) {
    if (width == AtomicPatchWidth::k32Bit) {
        auto* destination = static_cast<volatile LONG*>(word);
        return static_cast<uint32_t>(InterlockedCompareExchange(destination, static_cast<LONG>(exchange),
                                                                 static_cast<LONG>(comparand)));
    }
    auto* destination = static_cast<volatile LONG64*>(word);
    return static_cast<uint64_t>(InterlockedCompareExchange64(destination, static_cast<LONG64>(exchange),
                                                               static_cast<LONG64>(comparand)));
}

// Locates the mapped image's bounds and its executable section. The ICD always
// matches CE's own bitness, so a non-native image is refused rather than guessed.
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

bool FindAlreadyPatchedBranch(const uint8_t* image, size_t imageSize, size_t offset, Site& out) {
    if (offset + 2 > imageSize || image[offset] != 0x90 || image[offset + 1] != 0x90) {
        return false;
    }

    const size_t fallthrough = offset + 2;
    const size_t jumpSearchEnd = (fallthrough + kPathWindow < imageSize) ? fallthrough + kPathWindow : imageSize;
    for (size_t jump = fallthrough; jump + 2 <= jumpSearchEnd; ++jump) {
        if (image[jump] != 0xEB) {
            continue;
        }
        const size_t offPath = jump + 2;
        const int64_t target = static_cast<int64_t>(offPath) + static_cast<int8_t>(image[jump + 1]);
        if (target <= static_cast<int64_t>(offPath) || static_cast<uint64_t>(target) > imageSize) {
            continue;
        }

        uint8_t onDisp = 0;
        uint8_t offDisp = 0;
        if (!FindTableLoadDisp(image + fallthrough, jump - fallthrough, onDisp) ||
            !FindTableLoadDisp(image + offPath, static_cast<size_t>(target) - offPath, offDisp)) {
            continue;
        }
        if (onDisp != static_cast<uint8_t>(offDisp + 4)) {
            continue;
        }

        out.branchFound = true;
        out.branchAlreadyPatched = true;
        out.branchRva = offset;
        out.branchLength = 2;
        return true;
    }
    return false;
}

bool ApplyToModule(HMODULE module, const char* modulePath) {
    if (g_mode.load(std::memory_order_acquire) != Mode::kOn || !module) {
        return false;
    }

    ProcessPatchMutex patchMutex;
    if (!patchMutex.Acquired()) {
        NV_LOD_LOG("NV LOD spread: could not acquire the process patch lock for %s - nothing patched",
                   modulePath ? modulePath : "the ICD");
        return false;
    }

    if (g_patchedModule.load(std::memory_order_acquire) == module) {
        uint8_t* branch = g_branchAddress.load(std::memory_order_acquire);
        if (branch && branch[0] == 0x90 && branch[1] == 0x90) {
            return true;
        }
    }

    const uint8_t* image = nullptr;
    size_t imageSize = 0;
    size_t textRva = 0;
    size_t textSize = 0;
    bool is64Bit = false;
    if (!DescribeModule(module, image, imageSize, textRva, textSize, is64Bit)) {
        NV_LOD_LOG("NV LOD spread: %s is not a usable image - not patched", modulePath ? modulePath : "the ICD");
        return false;
    }

    Site site;
    if (!FindSettingSite(image, imageSize, textRva, textSize, is64Bit, reinterpret_cast<uintptr_t>(image), site)) {
        NV_LOD_LOG("NV LOD spread: no FERMI_UNOPT_LOD_SPREAD check found in %s - the driver layout changed, "
                   "nothing patched",
                   modulePath ? modulePath : "the ICD");
        return false;
    }

    g_moduleSeen.store(true, std::memory_order_release);
    if (!site.branchFound || site.branchLength != 2) {
        NV_LOD_LOG("NV LOD spread: setting check found at +0x%zX in %s, but the ON/OFF branch was not "
                   "structurally validated - nothing patched",
                   site.cmpRva, modulePath ? modulePath : "the ICD");
        return false;
    }

    uint8_t* branch = const_cast<uint8_t*>(image + site.branchRva);
    if (site.branchAlreadyPatched) {
        g_patchedModule.store(module, std::memory_order_release);
        g_branchAddress.store(branch, std::memory_order_release);
        g_codePatchApplied.store(true, std::memory_order_release);
        g_branchWasAlreadyPatched.store(true, std::memory_order_release);
        NV_LOD_LOG("NV LOD spread: %s already has the validated ON branch patch at +0x%zX",
                   modulePath ? modulePath : "the ICD", site.branchRva);
        return true;
    }

    const AtomicPatchWidth patchWidth = SelectAtomicPatchWidth(branch);
    if (patchWidth == AtomicPatchWidth::kNone) {
        NV_LOD_LOG("NV LOD spread: validated branch at +0x%zX in %s crosses the widest supported aligned atomic "
                   "word (addressMod8=%zu) - nothing patched",
                   site.branchRva, modulePath ? modulePath : "the ICD",
                   reinterpret_cast<uintptr_t>(branch) & uintptr_t{7});
        return false;
    }

    const uint8_t original0 = branch[0];
    const uint8_t original1 = branch[1];
    const CodePatchOutcome outcome = WriteTwoByteCodePatch(branch, original0, original1);
    if (!outcome.Succeeded()) {
        if (outcome.bytesPatched && outcome.verified) {
            g_patchedModule.store(module, std::memory_order_release);
            g_branchAddress.store(branch, std::memory_order_release);
            g_codePatchApplied.store(true, std::memory_order_release);
        }
        NV_LOD_LOG("NV LOD spread: FAILED to safely complete the validated OFF-branch patch at %p in %s "
                   "(result=%s, atomicWidth=%u-bit, bytesPatched=%u, wroteBytes=%u, cacheFlushed=%u, "
                   "protectionRestored=%u, verified=%u)",
                   branch, modulePath ? modulePath : "the ICD", GetCodePatchResultName(outcome.result),
                   static_cast<unsigned>(outcome.width) * 8u, outcome.bytesPatched ? 1u : 0u,
                   outcome.wroteBytes ? 1u : 0u, outcome.instructionCacheFlushed ? 1u : 0u,
                   outcome.protectionRestored ? 1u : 0u, outcome.verified ? 1u : 0u);
        return false;
    }

    g_patchedModule.store(module, std::memory_order_release);
    g_branchAddress.store(branch, std::memory_order_release);
    g_codePatchApplied.store(true, std::memory_order_release);
    if (outcome.result == CodePatchResult::kAlreadyPatched) {
        g_branchWasAlreadyPatched.store(true, std::memory_order_release);
    }
    NV_LOD_LOG("NV LOD spread: forced FERMI_UNOPT_LOD_SPREAD ON in %s (validated branch +0x%zX: %02X %02X -> "
               "90 90 via atomic %u-bit compare/exchange, check +0x%zX, setting 0x%08X) - process-local, the driver "
               "file is untouched",
               modulePath ? modulePath : "the ICD", site.branchRva, original0, original1,
               static_cast<unsigned>(outcome.width) * 8u, site.cmpRva, site.settingValue);
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

AtomicPatchWidth SelectAtomicPatchWidth(const void* address) {
    if (!address) {
        return AtomicPatchWidth::kNone;
    }
    const uintptr_t value = reinterpret_cast<uintptr_t>(address);
    if ((value & uintptr_t{3}) != 3) {
        return AtomicPatchWidth::k32Bit;
    }
    if ((value & uintptr_t{7}) != 7) {
        return AtomicPatchWidth::k64Bit;
    }
    return AtomicPatchWidth::kNone;
}

bool CanPatchTwoBytesAtomically(const void* address) {
    return SelectAtomicPatchWidth(address) != AtomicPatchWidth::kNone;
}

CodePatchOutcome WriteTwoByteCodePatch(uint8_t* address, uint8_t expected0, uint8_t expected1) {
    CodePatchOutcome outcome;
    outcome.width = SelectAtomicPatchWidth(address);
    if (!address) {
        return outcome;
    }
    if (outcome.width == AtomicPatchWidth::kNone) {
        outcome.result = CodePatchResult::kUnsupportedAlignment;
        return outcome;
    }

    const size_t wordSize = static_cast<size_t>(outcome.width);
    const uintptr_t alignedAddress = reinterpret_cast<uintptr_t>(address) & ~(static_cast<uintptr_t>(wordSize) - 1);
    const unsigned shift = static_cast<unsigned>((reinterpret_cast<uintptr_t>(address) - alignedAddress) * 8);
    constexpr uint64_t kPatchedBytes = 0x9090u;
    const uint64_t mask = uint64_t{0xFFFF} << shift;
    const uint64_t expectedPair = static_cast<uint64_t>(expected0) | (static_cast<uint64_t>(expected1) << 8);
    void* word = reinterpret_cast<void*>(alignedAddress);

    DWORD oldProtect = 0;
    if (!VirtualProtect(word, wordSize, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        outcome.result = CodePatchResult::kProtectionFailed;
        return outcome;
    }

    const uint64_t current = AtomicCompareExchangeWord(word, outcome.width, 0, 0);
    const uint64_t currentPair = (current & mask) >> shift;
    if (currentPair == kPatchedBytes) {
        outcome.result = CodePatchResult::kAlreadyPatched;
        outcome.bytesPatched = true;
    } else if (currentPair != expectedPair) {
        outcome.result = CodePatchResult::kUnexpectedBytes;
    } else {
        const uint64_t desired = (current & ~mask) | (kPatchedBytes << shift);
        const uint64_t observed = AtomicCompareExchangeWord(word, outcome.width, desired, current);
        if (observed != current) {
            outcome.result = CodePatchResult::kCompareExchangeLost;
        } else {
            outcome.result = CodePatchResult::kPatched;
            outcome.bytesPatched = true;
            outcome.wroteBytes = true;
            outcome.instructionCacheFlushed = FlushInstructionCache(GetCurrentProcess(), address, 2) != FALSE;
        }
    }

    outcome.verified = address[0] == 0x90 && address[1] == 0x90;
    outcome.bytesPatched = outcome.bytesPatched || outcome.verified;
    DWORD restored = 0;
    outcome.protectionRestored = VirtualProtect(word, wordSize, oldProtect, &restored) != FALSE;

    if (outcome.wroteBytes && !outcome.instructionCacheFlushed) {
        outcome.result = CodePatchResult::kInstructionCacheFlushFailed;
    } else if (!outcome.protectionRestored) {
        outcome.result = CodePatchResult::kProtectionRestoreFailed;
    } else if (outcome.bytesPatched && !outcome.verified) {
        outcome.result = CodePatchResult::kVerificationFailed;
    }
    return outcome;
}

const char* GetCodePatchResultName(CodePatchResult result) {
    switch (result) {
        case CodePatchResult::kPatched:
            return "patched";
        case CodePatchResult::kAlreadyPatched:
            return "already-patched";
        case CodePatchResult::kInvalidAddress:
            return "invalid-address";
        case CodePatchResult::kUnsupportedAlignment:
            return "unsupported-alignment";
        case CodePatchResult::kProtectionFailed:
            return "protection-failed";
        case CodePatchResult::kUnexpectedBytes:
            return "unexpected-bytes";
        case CodePatchResult::kCompareExchangeLost:
            return "compare-exchange-lost";
        case CodePatchResult::kInstructionCacheFlushFailed:
            return "instruction-cache-flush-failed";
        case CodePatchResult::kProtectionRestoreFailed:
            return "protection-restore-failed";
        case CodePatchResult::kVerificationFailed:
            return "verification-failed";
    }
    return "unknown";
}

bool FindTableLoadDisp(const uint8_t* path, size_t pathSize, uint8_t& outDisp) {
    if (!path) {
        return false;
    }
    for (size_t i = 0; i + 2 < pathSize; ++i) {
        size_t cursor = i;
        if (path[cursor] >= 0x40 && path[cursor] <= 0x4F) {
            ++cursor;
            if (cursor + 2 >= pathSize) {
                continue;
            }
        }
        if (path[cursor] != 0x8B) {
            continue;
        }
        const uint8_t modrm = path[cursor + 1];
        if ((modrm & 0xC0) != 0x40 || (modrm & 0x07) == 0x04) {
            continue;
        }
        outDisp = path[cursor + 2];
        return true;
    }
    return false;
}

bool FindOnBranch(const uint8_t* image, size_t imageSize, size_t cmpRva, Site& out) {
    if (!image || cmpRva + kCmpLength > imageSize) {
        return false;
    }
    const size_t searchStart = cmpRva + kCmpLength;
    const size_t searchEnd = (searchStart + kBranchSearchSpan < imageSize) ? searchStart + kBranchSearchSpan : imageSize;
    for (size_t offset = searchStart; offset + 2 <= searchEnd; ++offset) {
        if (FindAlreadyPatchedBranch(image, imageSize, offset, out)) {
            return true;
        }

        const uint8_t opcode = image[offset];
        if (opcode < 0x70 || opcode > 0x7F) {
            continue;
        }
        const size_t fallthrough = offset + 2;
        const int64_t target = static_cast<int64_t>(fallthrough) + static_cast<int8_t>(image[offset + 1]);
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
    if (!image || textSize < kCmpLength || textRva > imageSize || textSize > imageSize - textRva) {
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
    status.codePatchApplied = g_codePatchApplied.load(std::memory_order_acquire);
    status.branchWasAlreadyPatched = g_branchWasAlreadyPatched.load(std::memory_order_acquire);
    return status;
}

Mode GetMode() {
    return g_mode.load(std::memory_order_acquire);
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
            NV_LOD_LOG("NV LOD spread: override disabled (nv_lod_spread_fix=%s)", GetModeName(mode));
        }
        return false;
    }
    if (previous != mode) {
        NV_LOD_LOG("NV LOD spread: override enabled (nv_lod_spread_fix=on)");
    }

    bool patched = false;
    for (const char* name : {"nvoglv64.dll", "nvoglv32.dll"}) {
        if (HMODULE module = GetModuleHandleA(name)) {
            patched = ApplyToModule(module, name) || patched;
        }
    }
    return patched;
}

}  // namespace ce::nv_lod_spread

#undef NV_LOD_LOG
