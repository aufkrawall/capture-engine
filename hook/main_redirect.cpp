#include "main_internal.h"

#include "../common/module_enumeration.h"

#include <atomic>

namespace {

// True when honouring a redirect to `finalPath` would map a SECOND instance of
// that module base name, because the name is already loaded from a different
// file. See ce::graphics_runtime::WouldRedirectDuplicateLoadedModule for why a
// duplicate Streamline/NGX instance is fatal rather than merely useless.
//
// GetModuleHandleA/GetModuleFileNameA take the loader lock, which this thread
// already owns when the call arrives through HookedLdrLoadDll; the lock is
// re-entrant for its owner, and the surrounding redirect resolution already
// touches the file system here.
bool RedirectWouldDuplicateLoadedModule(const std::string &finalPath) {
  const char *baseName = ce::graphics_runtime::ModuleFileName(finalPath.c_str());
  if (!baseName || !baseName[0]) {
    return false;
  }
  const HMODULE loaded = GetModuleHandleA(baseName);
  if (!loaded) {
    return false;
  }
  char loadedPath[MAX_PATH] = {};
  const DWORD length = GetModuleFileNameA(loaded, loadedPath, MAX_PATH);
  if (length == 0 || length >= MAX_PATH) {
    loadedPath[0] = '\0';
  }
  if (!ce::graphics_runtime::WouldRedirectDuplicateLoadedModule(finalPath.c_str(), true, loadedPath)) {
    return false;
  }

  static std::atomic<uint32_t> refusalLogs{0};
  const uint32_t logIndex = refusalLogs.fetch_add(1, std::memory_order_relaxed);
  if (logIndex < 8 || (logIndex % 1000) == 0) {
    HookLogImportant("Loader redirect refused for %s: %s is already loaded from %s, so redirecting would map a "
                     "SECOND instance of a process-global runtime; keeping the loaded copy",
                     finalPath.c_str(), baseName, loadedPath[0] ? loadedPath : "an unresolved path");
  }
  return true;
}

// Latched the moment an image providing the Streamline core (sl.common) is seen
// from anywhere other than the configured override location. See
// ce::graphics_runtime::ShouldApplyStreamlineOverrideRedirect.
std::atomic<bool> g_ForeignStreamlineCoreObserved{false};

// Builds the override path for `filename` from an override setting that may name
// either a directory or a specific file. Shared by the redirect decision and by
// the "is this resolved path our override copy" test so the two can never drift.
std::string BuildOverridePath(const std::string &overridePath, const std::string &filename) {
  if (overridePath.empty() || filename.empty()) {
    return "";
  }
  const size_t overrideLastSlash = overridePath.find_last_of("\\/");
  const size_t overrideLastDot = overridePath.find_last_of('.');
  const bool hasExtension =
      (overrideLastDot != std::string::npos &&
       (overrideLastSlash == std::string::npos || overrideLastDot > overrideLastSlash));

  if (!hasExtension) {
    if (overridePath.back() == '\\' || overridePath.back() == '/') {
      return overridePath + filename;
    }
    return overridePath + "\\" + filename;
  }

  // The setting names a file. Use it directly when it is the requested file,
  // otherwise take its parent folder and append the requested name.
  std::string cfgFilename = overrideLastSlash != std::string::npos
                                ? overridePath.substr(overrideLastSlash + 1)
                                : overridePath;
  if (ce::graphics_runtime::EqualsIgnoreCase(cfgFilename.c_str(), filename.c_str())) {
    return overridePath;
  }
  if (overrideLastSlash != std::string::npos) {
    return overridePath.substr(0, overrideLastSlash) + "\\" + filename;
  }
  return filename;
}

// Gate for every sl.* redirect: CE may only place override plugins while it owns
// the Streamline core. Once the core is foreign, the remaining plugins must stay
// with the distribution the runtime already chose.
bool StreamlineOverrideRedirectAllowed(const char *targetDllName) {
  // Only the sl.* plugin set shares one distribution. nvngx_deepdvc /
  // nvlowlatencyvk merely live in the same override folder and are negotiated by
  // NGX and the Vulkan loader independently, so they are never gated on it.
  if (targetDllName && !ce::graphics_runtime::HasPrefixIgnoreCase(
                           ce::graphics_runtime::ModuleFileName(targetDllName), "sl.")) {
    return true;
  }
  if (ce::graphics_runtime::ShouldApplyStreamlineOverrideRedirect(
          true, g_ForeignStreamlineCoreObserved.load(std::memory_order_acquire))) {
    return true;
  }
  static std::atomic<uint32_t> refusalLogs{0};
  const uint32_t logIndex = refusalLogs.fetch_add(1, std::memory_order_relaxed);
  if (logIndex < 8 || (logIndex % 1000) == 0) {
    HookLogImportant(
        "Streamline override redirect refused for %s: the runtime already resolved a foreign sl.common core, so "
        "overriding this plugin alone would mix Streamline versions in one runtime",
        targetDllName ? targetDllName : "an sl.* plugin");
  }
  return false;
}

}  // namespace

// Records which physical image provides a Streamline plugin, from the loader
// notification that resolves every load's full path.
//
// Cyberpunk 20260816_153027: NVIDIA's NGX cache loaded sl.common 2.11
// (`...\models\sl_common_0\...\1B0_E658703.dll`) 463 ms before CE's loader
// redirect was armed, so Streamline had already resolved its core. Every LATER
// plugin load did reach the redirect and became the override copy, leaving
// sl.interposer 2.7.1 + sl.common 2.11 + sl.reflex/sl.dlss_g/sl.dlss_d/sl.pcl
// 2.12 in one runtime. sl.reflex 2.12 asked that sl.common for an interface it
// does not provide and called through the null result. A partially applied
// Streamline override is worse than none, so losing the core disables the whole
// sl.* redirect family.
void NoteRuntimeModuleLoadedForOverridePolicy(const char *resolvedPath) {
  if (!resolvedPath || !resolvedPath[0] || !g_pLocalConfig) {
    return;
  }
  const std::string &overridePath = g_pLocalConfig->graphics.streamlineDllPath;
  if (overridePath.empty() || g_ForeignStreamlineCoreObserved.load(std::memory_order_acquire)) {
    return;
  }
  char providedName[MAX_PATH] = {};
  if (!ce::graphics_runtime::ResolveStreamlineProvidedDllName(resolvedPath, providedName, sizeof(providedName)) ||
      !ce::graphics_runtime::IsStreamlineCoreProvidedDllName(providedName)) {
    return;
  }
  std::string expected = BuildOverridePath(overridePath, providedName);
  // The loader reports a canonical path; a configured override may be relative or
  // carry ".." segments. Canonicalize before comparing so a spelling difference
  // cannot latch CE's own copy as foreign. GetFullPathNameA is pure string work.
  char canonicalExpected[MAX_PATH] = {};
  const DWORD canonicalLength = GetFullPathNameA(expected.c_str(), MAX_PATH, canonicalExpected, nullptr);
  if (canonicalLength > 0 && canonicalLength < MAX_PATH) {
    expected.assign(canonicalExpected);
  }
  if (ce::graphics_runtime::EqualsModulePathIgnoreCase(resolvedPath, expected.c_str())) {
    return;  // CE's own override copy is the core: the override owns the stack.
  }
  if (!g_ForeignStreamlineCoreObserved.exchange(true, std::memory_order_acq_rel)) {
    HookLogImportant(
        "Streamline override disabled: the runtime resolved its core (%s) to %s, not to the configured override "
        "%s. Redirecting only the remaining plugins would build a version-mixed Streamline stack, so every sl.* "
        "redirect is refused from here on and the game keeps its own coherent set",
        providedName, resolvedPath, expected.empty() ? overridePath.c_str() : expected.c_str());
  }
}

// Startup answer for the same question when CE injected after the core was
// already mapped: the loader notification never saw that load.
void ScanLoadedModulesForForeignStreamlineCore() {
  if (!g_pLocalConfig || g_pLocalConfig->graphics.streamlineDllPath.empty()) {
    return;
  }
  std::vector<HMODULE> modules;
  if (!ce::EnumerateProcessModules(GetCurrentProcess(), modules)) {
    return;
  }
  for (HMODULE module : modules) {
    char path[MAX_PATH] = {};
    if (GetModuleFileNameA(module, path, MAX_PATH)) {
      NoteRuntimeModuleLoadedForOverridePolicy(path);
    }
  }
}

// DLL Redirection Helper
std::string GetRedirectedPath(const std::string &requestedPath) {
  if (requestedPath.empty())
    return "";

  try {
    // Basic path parsing without std::filesystem
    std::string filename;
    size_t lastSlash = requestedPath.find_last_of("\\/");
    if (lastSlash != std::string::npos) {
      filename = requestedPath.substr(lastSlash + 1);
    } else {
      filename = requestedPath;
    }

    std::string filenameLower = filename;
    std::transform(filenameLower.begin(), filenameLower.end(),
                   filenameLower.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });

    std::string overridePath;
    bool isStreamlineMatch = false;

    // NVIDIA's NGX model repository (C:\ProgramData\NVIDIA\NGX\models\...) is
    // the driver-managed Streamline plugin store: the plugins are stored under
    // hashed file names (e.g. 1B0_E658703.dll) in folders such as
    // "sl_dlss_g_0\versions\133888\files\". The loader-visible base name
    // carries no sl.* token, so the base-name matching below cannot see it.
    // Map the model folder to the real Streamline DLL and redirect it to the
    // configured override directory when one is set.
    if (overridePath.empty() && g_pLocalConfig &&
        !g_pLocalConfig->graphics.streamlineDllPath.empty() &&
        ce::graphics_runtime::IsNgxModelRepositoryPath(requestedPath.c_str())) {
      char modelDllName[MAX_PATH] = {};
      char segmentBuf[MAX_PATH] = {};
      if (ce::graphics_runtime::NgxModelSegment(requestedPath.c_str(), segmentBuf, sizeof(segmentBuf)) &&
          ce::graphics_runtime::ModelSegmentToDllName(segmentBuf, modelDllName, sizeof(modelDllName))) {
        if (!StreamlineOverrideRedirectAllowed(modelDllName)) {
          return "";
        }
        std::string modelFinal = BuildOverridePath(g_pLocalConfig->graphics.streamlineDllPath, modelDllName);
        if (!modelFinal.empty() &&
            GetFileAttributesA(modelFinal.c_str()) != INVALID_FILE_ATTRIBUTES) {
          if (RedirectWouldDuplicateLoadedModule(modelFinal)) {
            return "";
          }
          HookLog("Redirecting %s (NGX model %s) to: %s", filename.c_str(), segmentBuf,
                  modelFinal.c_str());
          return modelFinal;
        }
        HookLog("Streamline model DLL %s not found at redirect path %s - "
                "falling back to default load path",
                modelDllName, modelFinal.c_str());
      }
    }

    // 1. DLSS/Streamline Logic - Only if no custom detour set
    if (overridePath.empty() && g_pLocalConfig) {
      if (filenameLower == "nvngx_dlss.dll") {
        overridePath = g_pLocalConfig->graphics.dlssSrDllPath;
      }
      // 2. DLSS Frame Generation
      else if (filenameLower == "nvngx_dlssg.dll") {
        overridePath = g_pLocalConfig->graphics.dlssFgDllPath;
      }
      // 3. DLSS Ray Reconstruction (Denoiser)
      else if (filenameLower == "nvngx_dlssd.dll") {
        overridePath = g_pLocalConfig->graphics.dlssRrDllPath;
      }
      // 4. Streamline and related components
      else if (filenameLower.find("sl.") == 0 ||
               filenameLower == "nvngx_deepdvc.dll" ||
               filenameLower == "nvlowlatencyvk.dll") {
        overridePath = g_pLocalConfig->graphics.streamlineDllPath;
        isStreamlineMatch = true;
      }
    }

    if (!overridePath.empty()) {
      if (isStreamlineMatch && !StreamlineOverrideRedirectAllowed(filename.c_str())) {
        return "";
      }

      std::string finalPath = BuildOverridePath(overridePath, filename);

      // For streamline DLLs, verify the file exists at the redirect path.
      // If absent, fall back gracefully to the default load path.
      if (isStreamlineMatch && !finalPath.empty()) {
        if (GetFileAttributesA(finalPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
          HookLog("Streamline DLL %s not found at redirect path %s - "
                  "falling back to default load path",
                  filename.c_str(), finalPath.c_str());
          return "";
        }
      }

      if (RedirectWouldDuplicateLoadedModule(finalPath)) {
        return "";
      }

      static std::atomic<uint32_t> redirectLogs{0};
      const uint32_t logIndex = redirectLogs.fetch_add(1, std::memory_order_relaxed);
      if (logIndex < 8 || (logIndex % 1000) == 0) {
        HookLog("Redirecting %s to: %s", filename.c_str(), finalPath.c_str());
      }
      return finalPath;
    }

  } catch (...) {
    // Never let a redirect-resolution failure escape into the game's loader.
    // Falling back to the default load path is correct, but silently doing so
    // hid why a Streamline/FG DLL was not redirected.
    HookLog("Loader redirect resolution threw for %s - falling back to "
            "default load path",
            requestedPath.c_str());
  }
  return "";
}

bool NeedsLoaderRedirectionHook() {
  if (!g_pLocalConfig) {
    return false;
  }

  const auto &gfx = g_pLocalConfig->graphics;
  return !gfx.dlssSrDllPath.empty() || !gfx.dlssFgDllPath.empty() ||
         !gfx.dlssRrDllPath.empty() || !gfx.streamlineDllPath.empty();
}

bool NeedsLowLevelModuleLoadObservationHook() {
  // Some launchers and overlays load native FG runtimes through ntdll directly.
  // Observing LdrLoadDll lets us arm FFX/Streamline hooks before the game can
  // cache API pointers such as ffxConfigure.
  return true;
}

namespace {

// Loads one override DLL from `directory` under its plain file name. Skips
// silently when the directory is unset, the file is absent, or a module with
// the same base name is already loaded (name-based loads would keep returning
// that existing instance; adding the override as a second copy would not take
// effect and could confuse the runtime).
void PreloadOverrideDll(const std::string& directory, const char* fileName) {
  if (directory.empty() || !fileName || !fileName[0]) {
    return;
  }
  if (GetModuleHandleA(fileName)) {
    HookLog("Runtime preload: %s already loaded; keeping the existing copy", fileName);
    return;
  }

  std::string fullPath = directory;
  if (!fullPath.empty() && fullPath.back() != '\\' && fullPath.back() != '/') {
    fullPath += '\\';
  }
  fullPath += fileName;
  if (GetFileAttributesA(fullPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
    HookLog("Runtime preload: %s not found at %s - skipping", fileName, fullPath.c_str());
    return;
  }

  const int wideLen = MultiByteToWideChar(CP_UTF8, 0, fullPath.c_str(), -1, nullptr, 0);
  if (wideLen <= 0) {
    return;
  }
  std::wstring wide(static_cast<size_t>(wideLen), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, fullPath.c_str(), -1, wide.data(), wideLen);
  if (!wide.empty() && wide.back() == L'\0') {
    wide.pop_back();
  }

  const HMODULE hMod = LoadRuntimeDllViaOriginal(wide.c_str(), fullPath.c_str());
  HookLogImportant("Runtime preload: %s %s (module=%p)", fileName, hMod ? "loaded" : "FAILED",
                   reinterpret_cast<void*>(hMod));
}

}  // namespace

void PreloadConfiguredGraphicsRuntimeDlls() {
  if (!g_pLocalConfig) {
    return;
  }
  static std::atomic<bool> s_preloaded{false};
  if (s_preloaded.exchange(true, std::memory_order_acq_rel)) {
    return;
  }

  const auto& gfx = g_pLocalConfig->graphics;
  if (gfx.streamlineDllPath.empty() && gfx.dlssSrDllPath.empty() &&
      gfx.dlssFgDllPath.empty() && gfx.dlssRrDllPath.empty()) {
    return;
  }

  // Answer "did CE lose the Streamline core" before placing anything: injecting
  // into a process whose runtime already mapped its core happens whenever the
  // driver's NGX cache wins the race, and the loader notification cannot have
  // seen a load that predates CE.
  ScanLoadedModulesForForeignStreamlineCore();

  // Streamline stack first: sl.interposer pulls sl.common as a dependent from
  // the same directory; then the feature plugins and the NGX snippets. Once a
  // name is registered, every later name-based load (including Streamline's
  // own internal loads) resolves to these override copies.
  //
  // Skipped entirely once the core is foreign: those copies could then only ever
  // become the minority half of a version-mixed stack (or an unused third
  // instance), which is the Cyberpunk 20260816_153027 crash.
  if (StreamlineOverrideRedirectAllowed("sl.* plugin set")) {
    PreloadOverrideDll(gfx.streamlineDllPath, "sl.interposer.dll");
    PreloadOverrideDll(gfx.streamlineDllPath, "sl.common.dll");
    PreloadOverrideDll(gfx.streamlineDllPath, "sl.dlss.dll");
    PreloadOverrideDll(gfx.streamlineDllPath, "sl.dlss_g.dll");
    PreloadOverrideDll(gfx.streamlineDllPath, "sl.dlss_d.dll");
  }
  PreloadOverrideDll(gfx.dlssSrDllPath, "nvngx_dlss.dll");
  PreloadOverrideDll(gfx.dlssFgDllPath, "nvngx_dlssg.dll");
  PreloadOverrideDll(gfx.dlssRrDllPath, "nvngx_dlssd.dll");
}

void PatchLoadLibraryIatForLateLoadedModule(HMODULE module, const char* moduleNameOrPath) {
  if (!module || !moduleNameOrPath || !moduleNameOrPath[0] || !NeedsLoaderRedirectionHook()) {
    return;
  }

  // Never patch CE's own modules or third-party overlays: their LoadLibrary
  // traffic is either already ours or must stay untouched.
  if (strstr(moduleNameOrPath, "capture_hook") != nullptr ||
      strstr(moduleNameOrPath, "d3d12_wrappers") != nullptr) {
    return;
  }
  if (ce::overlay_compat::IsThirdPartyOverlayModulePath(moduleNameOrPath)) {
    return;
  }

  // The initial IAT pass is a snapshot: modules that load later (sl.common.dll,
  // sl.interposer.dll, the NGX snippets, ...) keep their real LoadLibrary*
  // imports, so their internal loads bypass the redirect entirely. Patch the
  // four loader imports here, inside the load notification, so the next load
  // from this module reaches the redirect. Only kernel32 loader imports are
  // touched - no graphics API wrapper is installed into runtime modules.
  void* dummy = nullptr;
  IATHook::PatchIAT(module, "kernel32.dll", "LoadLibraryA",
                    reinterpret_cast<void*>(&HookedLoadLibraryA), &dummy);
  IATHook::PatchIAT(module, "kernel32.dll", "LoadLibraryW",
                    reinterpret_cast<void*>(&HookedLoadLibraryW), &dummy);
  IATHook::PatchIAT(module, "kernel32.dll", "LoadLibraryExA",
                    reinterpret_cast<void*>(&HookedLoadLibraryExA), &dummy);
  IATHook::PatchIAT(module, "kernel32.dll", "LoadLibraryExW",
                    reinterpret_cast<void*>(&HookedLoadLibraryExW), &dummy);
}
