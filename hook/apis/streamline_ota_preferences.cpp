/*
 * ngx_ota=off, applied to the game's own Streamline runtime.
 *
 * Refusing the `nvngx_update.exe` launch stops NEW downloads. It does nothing
 * about downloads that already happened: Streamline's default
 * `Preferences::flags` carry `eAllowOTA | eLoadDownloadedPlugins`, and with
 * those set the runtime discovers plugins under
 * %ProgramData%\NVIDIA\NGX\models\sl_*_0\versions\... and arbitrates them
 * against the game's own set by version. Session 20260918_162809 is what that
 * costs: Alan Wake 2 resolved its `sl.common` core to
 * `...\sl_common_0\versions\134656\files\1B0_E658703.dll` rather than to either
 * its own folder or the configured `streamline_dll_path`, and CE then correctly
 * refused all six sl.* redirects rather than build a version-mixed stack. The
 * user's configured runtime never loaded and nothing failed loudly.
 *
 * Clearing those two bits in the Preferences the game passes to `slInit` is the
 * only point where that decision can still be influenced from outside the game.
 *
 * Three guards make this safe rather than merely effective:
 *
 *   1. Generation. `slInit` has a different signature in Streamline 1.x
 *      (`bool(const Preferences&, int)`) than in 2.x
 *      (`sl::Result(const Preferences&, uint64_t)`), and a different Preferences
 *      layout behind it. Only 2.x is hooked.
 *   2. Struct identity. `sl::Preferences` derives from `BaseStructure`, which
 *      carries the struct's own GUID and version. CE compares both against the
 *      SDK header it was compiled with and forwards the call untouched unless
 *      they match, so a runtime whose layout has moved is never reinterpreted.
 *   3. Copy, never mutate. The game owns the struct it passed by const
 *      reference. CE forwards a modified COPY; the game's own memory is never
 *      written.
 *
 * Any failure of those guards means the call is forwarded exactly as it
 * arrived, which is the same outcome as not hooking at all.
 */

#include "streamline_ota_preferences.h"

#include <atomic>

#include "../common/hook_common.h"
#include "../common/ngx_ota_policy.h"
#include "../common/ngx_ota_runtime.h"
#include "streamline_hook_internal.h"

// Streamline ships x64 only, and so does its SDK header set. The x86 hook DLL
// gets the no-op definition at the bottom of this file.
#if defined(_M_X64) || defined(__x86_64__)
#include "sl.h"
#include "sl_core_types.h"
#endif

namespace ce::streamline_ota {

#if defined(_M_X64) || defined(__x86_64__)

namespace {

using PFN_slInit2x = sl::Result (*)(const sl::Preferences&, uint64_t);

std::atomic<void*> g_OriginalSlInit{nullptr};
std::atomic<bool> g_Registered{false};
std::atomic<uint32_t> g_AppliedCount{0};

// True when the incoming struct is the `sl::Preferences` this translation unit
// was compiled against. `BaseStructure` places `structType` and `structVersion`
// at fixed offsets that have not moved across the 2.x line, which is exactly why
// they are usable as the identity check for everything that follows them.
bool IsKnownPreferencesLayout(const sl::Preferences& preferences) {
    return preferences.structType == sl::Preferences::s_structType &&
           preferences.structVersion >= sl::kStructVersion1;
}

sl::Result Hooked_slInit(const sl::Preferences& preferences, uint64_t sdkVersion) {
    auto original = reinterpret_cast<PFN_slInit2x>(g_OriginalSlInit.load(std::memory_order_acquire));
    if (!original) {
        // No original means CE has nothing to forward to; failing the call would
        // take the game's Streamline down with it.
        return sl::Result::eErrorNotInitialized;
    }

    const uint8_t mode = ce::ngx_ota::CurrentMode();
    if (!ce::ngx_ota::ShouldClearStreamlineOtaPreferences(mode) || !IsKnownPreferencesLayout(preferences)) {
        return original(preferences, sdkVersion);
    }

    // SL_ENUM_OPERATORS_64 declares operator| as a plain inline, so the mask is
    // built from the underlying values rather than from the enum operators.
    constexpr uint64_t kOtaBits = static_cast<uint64_t>(sl::PreferenceFlags::eAllowOTA) |
                                  static_cast<uint64_t>(sl::PreferenceFlags::eLoadDownloadedPlugins);
    const auto incoming = static_cast<uint64_t>(preferences.flags);
    const auto cleared = incoming & ~kOtaBits;
    if (cleared == incoming) {
        return original(preferences, sdkVersion);  // The game had already opted out.
    }

    sl::Preferences adjusted = preferences;
    adjusted.flags = static_cast<sl::PreferenceFlags>(cleared);

    const uint32_t index = g_AppliedCount.fetch_add(1, std::memory_order_relaxed);
    if (index < 4) {
        HookLogImportant(
            "NGX OTA: ngx_ota=off - cleared eAllowOTA|eLoadDownloadedPlugins from the game's slInit preferences "
            "(0x%016llX -> 0x%016llX) so the runtime loads its own plugin set instead of the driver's OTA copies "
            "under %%ProgramData%%\\NVIDIA\\NGX\\models",
            static_cast<unsigned long long>(incoming), static_cast<unsigned long long>(cleared));
    }
    return original(adjusted, sdkVersion);
}

}  // namespace

void RegisterDynamicHookOnce(ce::streamline_api::Generation generation) {
    // 1.x `slInit` takes an `int` where 2.x takes a `uint64_t`, and its
    // Preferences layout predates the struct-identity header this relies on.
    // There is no safe way to reach into it, so it is left alone.
    if (generation != ce::streamline_api::Generation::V2) {
        return;
    }
    // Nothing to install when no profile asks for it. The mode is resolved by
    // the time any Streamline module loads, because the hook thread publishes it
    // immediately after LoadConfig.
    if (!ce::ngx_ota::ShouldClearStreamlineOtaPreferences(ce::ngx_ota::CurrentMode())) {
        return;
    }
    if (g_Registered.exchange(true, std::memory_order_acq_rel)) {
        return;
    }

    IATHook::RegisterDynamicHookFiltered("slInit", reinterpret_cast<void*>(&Hooked_slInit),
                                         reinterpret_cast<void**>(&g_OriginalSlInit),
                                         IsStreamlineCoreDynamicHookModule);
    HookLogImportant("NGX OTA: registered the 2.x slInit route so ngx_ota=off can clear Streamline's OTA preferences");
}

#else

void RegisterDynamicHookOnce(ce::streamline_api::Generation) {
    // No Streamline on x86, so there is no slInit to route.
}

#endif

}  // namespace ce::streamline_ota
