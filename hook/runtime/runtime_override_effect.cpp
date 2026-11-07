#include "main_internal.h"

#include <atomic>
#include <string>

#include "hook/overrides/published_graphics_config.h"
#include "runtime_override_paths.h"

// Which configured and alternate DLSS snippets are mapped.
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
  static std::atomic<bool> reportedConfigured[3] = {};
  static std::atomic<bool> reportedRepository[3] = {};
  static std::atomic<bool> reportedOther[3] = {};
  auto &reported = effect == ce::graphics_runtime::DlssOverrideEffect::InEffect ? reportedConfigured[slot]
      : effect == ce::graphics_runtime::DlssOverrideEffect::ModelRepository ? reportedRepository[slot]
                                                                           : reportedOther[slot];
  if (reported.exchange(true, std::memory_order_acq_rel)) {
    return;
  }
  switch (effect) {
    case ce::graphics_runtime::DlssOverrideEffect::InEffect:
      HookLogImportant("DLSS override: mapped configured %s from %s", snippet, resolvedPath);
      break;
    case ce::graphics_runtime::DlssOverrideEffect::ModelRepository:
      // W3 (session 20261106_195839): NGX mapped this copy of nvngx_dlss.dll and nvngx_dlssd.dll beside the configured
      // ones and then ran the configured ones, so with the configured copy mapped this is no verdict.
      if (ce::graphics_runtime::IsDlssOverrideBypassedByModelRepository(
              reportedConfigured[slot].load(std::memory_order_acquire))) {
        HookLogImportant(
            "DLSS override: NGX mapped the driver's model-repository copy of %s at %s before the configured %s",
            snippet, resolvedPath, expected.c_str());
      } else {
        HookLogImportant(
            "DLSS override: NGX also mapped its model-repository copy %s of %s beside the configured %s; NGX's own "
            "log names the copy it runs",
            resolvedPath, snippet, expected.c_str());
      }
      break;
    case ce::graphics_runtime::DlssOverrideEffect::OtherCopy:
      HookLogImportant(
          "DLSS override: mapped alternate copy of %s at %s (configured %s)",
          snippet, resolvedPath, expected.c_str());
      break;
  }
}
