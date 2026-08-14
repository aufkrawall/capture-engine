#include "main_ue5_internal.h"

namespace UE5 {
namespace detail {

std::array<std::atomic<HMODULE>, kPendingModuleCapacity> g_pendingModules{};
std::atomic<bool> g_fullRescanRequested{true};
std::atomic<uint32_t> g_queueOverflowCount{0};
std::array<std::atomic<HMODULE>, kCVarCount> g_activeModules{};
std::array<std::atomic<bool>, kCVarCount> g_activeModuleUnloaded{};
std::array<std::atomic<ForcedConsoleVariableData*>, kCVarCount> g_forcedData{};
std::array<OverrideState, kCVarCount> g_overrides{};
std::array<ce::ue5_cvar::ResolvedValue, kCVarCount> g_desired{};
bool g_missingSummaryLogged = false;

}  // namespace detail
namespace {

std::atomic<bool> g_policyRequested{false};
ce::ue5_cvar::Settings g_settings{};
bool g_wasEnabled = false;

ce::ue5_cvar::Settings MakeSettings(const GraphicsConfig& config) {
  return {
      config.forceRayReconstruction,
      config.rayReconstructionOptimalSettings,
      config.disablePostProcessingEffects,
      config.tonemapperSharpen,
  };
}

bool SameSettings(const ce::ue5_cvar::Settings& left, const ce::ue5_cvar::Settings& right) {
  return left.forceRayReconstruction == right.forceRayReconstruction &&
         left.rayReconstructionOptimalSettings == right.rayReconstructionOptimalSettings &&
         left.disablePostProcessingEffects == right.disablePostProcessingEffects &&
         std::bit_cast<uint32_t>(left.tonemapperSharpen) ==
             std::bit_cast<uint32_t>(right.tonemapperSharpen);
}

void UpdateDesiredOverrides(const ce::ue5_cvar::Settings& settings) {
  bool newlyEnabled = false;
  for (std::size_t index = 0; index < detail::kCVarCount; ++index) {
    const ce::ue5_cvar::ResolvedValue resolved = ce::ue5_cvar::Resolve(ce::ue5_cvar::kSpecs[index], settings);
    if (detail::g_desired[index].enabled && !resolved.enabled)
      detail::RestoreOverride(index, "configuration disabled");
    if (!detail::g_desired[index].enabled && resolved.enabled)
      newlyEnabled = true;
    detail::g_desired[index] = resolved;
    if (resolved.enabled)
      detail::UpdateForcedData(index, resolved.bits);
  }
  if (newlyEnabled) {
    detail::g_fullRescanRequested.store(true, std::memory_order_release);
    detail::g_missingSummaryLogged = false;
  }
}

}  // namespace

void NotifyModuleLoaded(HMODULE module) {
#ifdef _WIN64
  if (!module || !g_policyRequested.load(std::memory_order_acquire))
    return;
  for (auto& slot : detail::g_pendingModules) {
    HMODULE empty = nullptr;
    if (slot.compare_exchange_strong(empty, module, std::memory_order_acq_rel)) {
      if (g_hCheckHooksEvent)
        SetEvent(g_hCheckHooksEvent);
      return;
    }
    if (empty == module)
      return;
  }
  detail::g_queueOverflowCount.fetch_add(1, std::memory_order_relaxed);
  detail::g_fullRescanRequested.store(true, std::memory_order_release);
  if (g_hCheckHooksEvent)
    SetEvent(g_hCheckHooksEvent);
#else
  (void)module;
#endif
}

void NotifyModuleUnloaded(void* moduleBase, std::size_t moduleSize) {
#ifdef _WIN64
  (void)moduleSize;
  HMODULE module = static_cast<HMODULE>(moduleBase);
  if (!module)
    return;
  for (auto& slot : detail::g_pendingModules) {
    HMODULE pending = slot.load(std::memory_order_acquire);
    if (pending == module)
      slot.compare_exchange_strong(pending, nullptr, std::memory_order_acq_rel);
  }
  bool retiredAny = false;
  for (std::size_t index = 0; index < detail::kCVarCount; ++index) {
    HMODULE active = module;
    if (detail::g_activeModules[index].compare_exchange_strong(active, nullptr, std::memory_order_acq_rel)) {
      detail::g_activeModuleUnloaded[index].store(true, std::memory_order_release);
      retiredAny = true;
    }
  }
  if (retiredAny && g_hCheckHooksEvent)
    SetEvent(g_hCheckHooksEvent);
#else
  (void)moduleBase;
  (void)moduleSize;
#endif
}

void RefreshOverrides(const GraphicsConfig& config) {
  const ce::ue5_cvar::Settings settings = MakeSettings(config);
  const bool enabled = ce::ue5_cvar::AnyEnabled(settings);
  g_policyRequested.store(enabled, std::memory_order_release);
#ifdef _WIN64
  if (!SameSettings(settings, g_settings)) {
    UpdateDesiredOverrides(settings);
    g_settings = settings;
  }

  if (!enabled) {
    if (g_wasEnabled)
      detail::RestoreAllOverrides("all UE5 overrides disabled");
    g_wasEnabled = false;
    detail::g_missingSummaryLogged = false;
    detail::g_fullRescanRequested.store(true, std::memory_order_release);
    detail::ClearPendingModules();
    return;
  }

  if (!g_wasEnabled) {
    g_wasEnabled = true;
    detail::g_missingSummaryLogged = false;
    detail::g_fullRescanRequested.store(true, std::memory_order_release);
    HookLogImportant(
        "UE5 overrides enabled: forceRR=%d rrOptimal=%d disablePost=%d tonemapperSharpen=%.3f; "
        "installing persistent in-memory CVar shadows without changing Engine.ini",
        settings.forceRayReconstruction ? 1 : 0,
        settings.rayReconstructionOptimalSettings ? 1 : 0,
        settings.disablePostProcessingEffects ? 1 : 0, settings.tonemapperSharpen);
  }

  detail::ForgetUnloadedOverrides();
  if (detail::g_fullRescanRequested.exchange(false, std::memory_order_acq_rel)) {
    const uint32_t overflow = detail::g_queueOverflowCount.exchange(0, std::memory_order_acq_rel);
    if (overflow)
      HookLog("UE5 overrides: module notification queue overflowed %u time(s); using a full safe rescan",
              overflow);
    detail::ScanAllLoadedModules();
  }
  detail::ScanPendingModules();
#else
  static bool logged = false;
  if (enabled && !logged) {
    logged = true;
    HookLogImportant("UE5 overrides are unavailable in 32-bit processes");
  }
#endif
}

void ShutdownOverrides() {
  g_policyRequested.store(false, std::memory_order_release);
#ifdef _WIN64
  detail::RestoreAllOverrides("hook shutdown");
  g_settings = {};
  for (auto& desired : detail::g_desired)
    desired = {};
  g_wasEnabled = false;
  detail::g_missingSummaryLogged = false;
  detail::g_fullRescanRequested.store(true, std::memory_order_release);
  detail::ClearPendingModules();
#endif
}

}  // namespace UE5
