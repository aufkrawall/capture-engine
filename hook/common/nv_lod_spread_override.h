/**
 * NVIDIA OpenGL/Vulkan LOD-spread quality override (config key `nv_lod_spread_fix`).
 *
 * The NVIDIA GL/VK ICD (nvoglv64.dll / nvoglv32.dll) still carries the
 * FERMI_UNOPT_LOD_SPREAD driver setting. It defaults to OFF, which makes the ICD
 * emit a LOD spread of 0 into its texture-filtering state record, and that is the
 * long-standing negative-LOD-bias filtering quality bug NVIDIA has not fixed.
 * Forcing the ON path restores a spread of 0x10.
 *
 * The community fix byte-patches the DLL on disk. CE applies the same validated
 * two-byte branch patch to the process-local image, so the machine keeps
 * NVIDIA's signed driver files.
 *
 * Verified against 32.0.16.1088 and 32.0.16.2012 (620.12), both architectures.
 * The relevant code is:
 *
 *     cmp dword ptr [<global>], 0x37299934   ; the ON constant - the only reader
 *     jne OFF                                ; ON  path: r8d = [table+0x30] (1)
 *     ...                                    ; OFF path: r8d = [table+0x2C] (0)
 *                                            ; both shifted by [table+0x28] (4)
 *
 * The first implementation wrote the setting global instead of the branch. A
 * real DXVK run disproved the assumed timing equivalence: the Vulkan instance
 * and device already existed before the injected hook DLL ran, while the Vulkan
 * layer had participated before both. CE therefore performs the exact proven
 * NOP patch from the Vulkan layer immediately after the ICD is mapped and before
 * vkCreateDevice, with the injected hook retaining coverage for OpenGL and
 * already-loaded modules.
 *
 * Detection is entirely pattern-based and self-validating - the resolved global
 * has to actually hold the documented ON or OFF constant, and the fallback site
 * has to actually be a short conditional branch whose two paths load table slots
 * exactly four bytes apart with the ON slot on the fall-through side. Anything
 * else is refused and logged rather than patched, so a future driver layout can
 * never turn this into a blind write.
 */

#pragma once

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <string>

namespace ce::nv_lod_spread {

enum class Mode { kOff, kOn };

// The FERMI_UNOPT_LOD_SPREAD enum payloads, and the DRS id the ICD reads it with.
inline constexpr uint32_t kSettingOn = 0x37299934u;
inline constexpr uint32_t kSettingOff = 0x56023627u;
inline constexpr uint32_t kDrsSettingId = 0x003001ACu;

// cmp dword ptr [<global>], imm32  ->  81 3D <disp32> <imm32>
inline constexpr uint8_t kCmpOpcode0 = 0x81;
inline constexpr uint8_t kCmpOpcode1 = 0x3D;
inline constexpr size_t kCmpLength = 10;

// How far past the cmp the guarding short jcc may sit, and how much of each
// branch path is inspected for its table load. Both are generous next to the
// 0x21/0x23-byte distances the shipped drivers use.
inline constexpr size_t kBranchSearchSpan = 0x40;
inline constexpr size_t kPathWindow = 0x20;

// "off"/""/"default"/"0"/"false"/"disabled" -> kOff,
// "on"/"1"/"true"/"enabled" -> kOn. Case- and whitespace-tolerant; anything
// unrecognized stays kOff so a typo never patches a driver.
Mode ParseMode(const std::string& configValue);
const char* GetModeName(Mode mode);

// True for nvoglv64.dll / nvoglv32.dll, by base name, path or bare name.
bool IsIcdModuleName(const char* modulePathOrName);

// What a scan of a mapped ICD image found. Offsets are image-relative so the
// scanner stays testable against a synthetic buffer.
struct Site {
    bool found = false;
    size_t cmpRva = 0;
    size_t settingRva = 0;
    uint32_t settingValue = 0;  // must be kSettingOn or kSettingOff to be accepted
    bool branchFound = false;
    bool branchAlreadyPatched = false;
    size_t branchRva = 0;
    size_t branchLength = 0;
};

// Locates the setting check inside a mapped image. `image`/`imageSize` must span
// the whole image because the global lives in .data while the scan is confined to
// `textRva`/`textSize`. `imageBase` is only consulted for 32-bit images, whose cmp
// encodes an absolute address rather than a RIP-relative displacement.
bool FindSettingSite(const uint8_t* image, size_t imageSize, size_t textRva, size_t textSize, bool is64Bit,
                     uintptr_t imageBase, Site& out);

// Structural validation of the patch site: the two paths leaving `branchRva`
// must load table slots four bytes apart with the larger (ON) slot reached by
// fall-through, which is what makes NOPing the branch equivalent to forcing ON.
// An already-NOPed site is recognized only when the same two paths remain
// structurally provable.
bool FindOnBranch(const uint8_t* image, size_t imageSize, size_t cmpRva, Site& out);

// First `mov r32, dword ptr [reg+disp8]` in a branch path, which is the ON/OFF
// table slot load. Returns false when the window holds no unambiguous candidate.
bool FindTableLoadDisp(const uint8_t* path, size_t pathSize, uint8_t& outDisp);

// The live two-byte branch replacement is performed through an aligned 32-bit
// compare/exchange. Refuse the one layout that would straddle that word rather
// than exposing an instruction stream containing one old and one new byte.
bool CanPatchTwoBytesAtomically(const void* address);

// Runtime state, for diagnostics and tests of the decision logic.
struct Status {
    bool armed = false;
    bool moduleSeen = false;
    bool codePatchApplied = false;
    bool branchWasAlreadyPatched = false;
};

Status GetStatus();
Mode GetMode();

// Arms the override and sweeps already-loaded ICDs, which is what covers a late
// injection into a process whose driver is mapped. Idempotent; kOff installs
// nothing. Returns true when an ICD was patched.
bool Install(Mode mode);

// Module-load participation, called from CE's loader notification.
void OnModuleLoaded(HMODULE module, const char* modulePath);

}  // namespace ce::nv_lod_spread
