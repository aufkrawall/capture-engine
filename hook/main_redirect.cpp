#include "main_internal.h"

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
      const char* segment = nullptr;
      size_t segmentLength = 0;
      const char* cursor = requestedPath.c_str();
      while (*cursor) {
        if ((*cursor == 'm' || *cursor == 'M') &&
            (cursor[1] == 'o' || cursor[1] == 'O') &&
            (cursor[2] == 'd' || cursor[2] == 'D') &&
            (cursor[3] == 'e' || cursor[3] == 'E') &&
            (cursor[4] == 'l' || cursor[4] == 'L') &&
            (cursor[5] == 's' || cursor[5] == 'S') &&
            (cursor[6] == '\\' || cursor[6] == '/')) {
          segment = cursor + 7;
          break;
        }
        ++cursor;
      }
      if (segment) {
        const char* segmentEnd = segment;
        while (*segmentEnd && *segmentEnd != '\\' && *segmentEnd != '/') {
          ++segmentEnd;
        }
        segmentLength = static_cast<size_t>(segmentEnd - segment);
        if (segmentLength > 0 && segmentLength < MAX_PATH) {
          char segmentBuf[MAX_PATH] = {};
          memcpy(segmentBuf, segment, segmentLength);
          if (ce::graphics_runtime::ModelSegmentToDllName(segmentBuf, modelDllName,
                                                          sizeof(modelDllName))) {
            std::string modelFinal = g_pLocalConfig->graphics.streamlineDllPath;
            if (!modelFinal.empty() && modelFinal.back() != '\\' && modelFinal.back() != '/') {
              modelFinal += '\\';
            }
            modelFinal += modelDllName;
            if (GetFileAttributesA(modelFinal.c_str()) != INVALID_FILE_ATTRIBUTES) {
              HookLog("Redirecting %s (NGX model %s) to: %s", filename.c_str(), segmentBuf,
                      modelFinal.c_str());
              return modelFinal;
            }
            HookLog("Streamline model DLL %s not found at redirect path %s - "
                    "falling back to default load path",
                    modelDllName, modelFinal.c_str());
          }
        }
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
      std::string finalPath;

      // Check if overridePath has an extension (heuristic for file vs dir)
      size_t overrideLastSlash = overridePath.find_last_of("\\/");
      size_t overrideLastDot = overridePath.find_last_of('.');
      bool hasExtension = (overrideLastDot != std::string::npos &&
                           (overrideLastSlash == std::string::npos ||
                            overrideLastDot > overrideLastSlash));

      if (hasExtension) {
        // It looks like a file.
        // If it ends with the SAME filename as requested, just use it.
        std::string cfgFilename;
        size_t cfgLastSlash = overridePath.find_last_of("\\/");
        if (cfgLastSlash != std::string::npos) {
          cfgFilename = overridePath.substr(cfgLastSlash + 1);
        } else {
          cfgFilename = overridePath;
        }

        std::string cfgFilenameLower = cfgFilename;
        std::transform(cfgFilenameLower.begin(), cfgFilenameLower.end(),
                       cfgFilenameLower.begin(),
                       [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });

        if (cfgFilenameLower == filenameLower) {
          finalPath = overridePath;
        } else {
          // Config points to a file, but we want a potentially different file
          // Take parent folder, then append requested filename.
          if (cfgLastSlash != std::string::npos) {
            finalPath = overridePath.substr(0, cfgLastSlash) + "\\" + filename;
          } else {
            finalPath = filename; // Should not happen if full path
          }
        }
      } else {
        // It looks like a directory. Append the requested filename.
        if (overridePath.back() == '\\' || overridePath.back() == '/') {
          finalPath = overridePath + filename;
        } else {
          finalPath = overridePath + "\\" + filename;
        }
      }

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

      HookLog("Redirecting %s to: %s", filename.c_str(), finalPath.c_str());
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

  // Streamline stack first: sl.interposer pulls sl.common as a dependent from
  // the same directory; then the feature plugins and the NGX snippets. Once a
  // name is registered, every later name-based load (including Streamline's
  // own internal loads) resolves to these override copies.
  PreloadOverrideDll(gfx.streamlineDllPath, "sl.interposer.dll");
  PreloadOverrideDll(gfx.streamlineDllPath, "sl.common.dll");
  PreloadOverrideDll(gfx.streamlineDllPath, "sl.dlss.dll");
  PreloadOverrideDll(gfx.streamlineDllPath, "sl.dlss_g.dll");
  PreloadOverrideDll(gfx.streamlineDllPath, "sl.dlss_d.dll");
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
