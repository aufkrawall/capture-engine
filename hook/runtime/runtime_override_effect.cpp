#include "main_internal.h"

#include <atomic>
#include <string>

#include "hook/overrides/published_graphics_config.h"
#include "runtime_override_paths.h"

// Whether a configured dlss_*_dll_path is the DLSS snippet NGX actually runs.
//
// The redirect sees only loads by the plain snippet names. NGX maps its own
// OTA or NVIDIA-app-overridden copies from the model repository by full path
// (models\dlss\versions\<v>\files\<id>.bin), and a snippet the game loaded
// before CE attached keeps that copy; both leave the configured file unused
// without any other trace. Each snippet's verdict is logged once, from the
// loader notification and from the startup module scan.
void NoteDlssSnippetLoadedForOverridePolicy(const char *resolvedPath) {
  bool fromModelRepository = false;
  const char *snippet = ce::graphics_runtime::ResolveDlssSnippetName(resolvedPath, &fromModelRepository);
  if (!snippet || !CurrentProcessOwnsProcessLocalRuntimeOverrides()) {
    return;
  }
  const auto &paths = ce::published_config::CurrentRuntimeOverridePaths();
  const char *configured = paths.dlssRr;
  size_t slot = 2;
  if (ce::graphics_runtime::EqualsIgnoreCase(snippet, "nvngx_dlss.dll")) {
    configured = paths.dlssSr;
    slot = 0;
  } else if (ce::graphics_runtime::EqualsIgnoreCase(snippet, "nvngx_dlssg.dll")) {
    configured = paths.dlssFg;
    slot = 1;
  }
  if (!configured[0]) {
    return;
  }
  const std::string expected = CanonicalOverridePath(configured, snippet);
  const auto effect = ce::graphics_runtime::ClassifyDlssOverrideEffect(resolvedPath, fromModelRepository,
                                                                      expected.c_str());
  // CE's own preload maps the configured copy first; NGX can still map its own
  // copy later. So "in effect" and "not in effect" are each reported once.
  static std::atomic<bool> reportedInEffect[3] = {};
  static std::atomic<bool> reportedBypass[3] = {};
  auto &reported = effect == ce::graphics_runtime::DlssOverrideEffect::InEffect ? reportedInEffect[slot]
                                                                               : reportedBypass[slot];
  if (reported.exchange(true, std::memory_order_acq_rel)) {
    return;
  }
  switch (effect) {
    case ce::graphics_runtime::DlssOverrideEffect::InEffect:
      HookLogImportant("DLSS override in effect: %s is the configured %s", snippet, resolvedPath);
      break;
    case ce::graphics_runtime::DlssOverrideEffect::ModelRepository:
      HookLogImportant(
          "DLSS override NOT in effect for %s: NGX mapped the driver's model-repository copy %s (an OTA update "
          "or an NVIDIA app DLSS override) instead of the configured %s",
          snippet, resolvedPath, expected.c_str());
      break;
    case ce::graphics_runtime::DlssOverrideEffect::OtherCopy:
      HookLogImportant(
          "DLSS override NOT in effect for %s: %s was mapped instead of the configured %s (loaded before CE's "
          "override, or by a path the redirect does not see)",
          snippet, resolvedPath, expected.c_str());
      break;
  }
}
