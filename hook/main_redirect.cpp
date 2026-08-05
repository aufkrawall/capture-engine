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

// Hooked RegQueryValueExW - For DLSS Debug Overlay
LSTATUS WINAPI HookedRegQueryValueExW(HKEY hKey, LPCWSTR lpValueName,
                                      LPDWORD lpReserved, LPDWORD lpType,
                                      LPBYTE lpData, LPDWORD lpcbData) {
  LSTATUS status = OriginalRegQueryValueExW(hKey, lpValueName, lpReserved,
                                            lpType, lpData, lpcbData);

  // Check if probing for DLSS Indicator
  if (lpValueName && _wcsicmp(lpValueName, L"ShowDlssIndicator") == 0) {
    // Only if we have a config override
    if (g_pLocalConfig && !g_pLocalConfig->graphics.dlssDebugOverlay.empty() &&
        g_pLocalConfig->graphics.dlssDebugOverlay != "default") {
      // If caller provided buffer to read data
      if (lpData && lpcbData && *lpcbData >= 4) {
        DWORD *outData = (DWORD *)lpData;
        if (g_pLocalConfig->graphics.dlssDebugOverlay == "on") {
          *outData = 0x400; // Force ON
          // HookLog("RegQueryValueExW: Force-enabled DLSS Indicator");
        } else if (g_pLocalConfig->graphics.dlssDebugOverlay == "off") {
          *outData = 0; // Force OFF
        }
        return ERROR_SUCCESS; // Pretend we succeeded even if registry key
                              // didn't exist
      }
    }
  }
  return status;
}
