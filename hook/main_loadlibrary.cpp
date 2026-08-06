#include "main_internal.h"

#include <algorithm>
#include <cstring>

std::atomic<LoadLibraryA_t> OriginalLoadLibraryA{nullptr};

std::atomic<LoadLibraryW_t> OriginalLoadLibraryW{nullptr};

std::atomic<LoadLibraryExA_t> OriginalLoadLibraryExA{nullptr};

std::atomic<LoadLibraryExW_t> OriginalLoadLibraryExW{nullptr};

std::atomic<LdrLoadDll_t> OriginalLdrLoadDll{nullptr};

namespace {

// Writes a bounded, masked copy of the command line into out (always
// NUL-terminated). Values of sensitive "key=value" arguments (including
// quoted values) are replaced with '*' and the excerpt is capped; the full
// length is reported through totalLength so callers can log it separately.
void MaskCommandLineForLog(const char* raw, char* out, size_t outSize,
                           size_t& totalLength) {
  totalLength = raw ? strlen(raw) : 0;
  if (outSize == 0) {
    return;
  }
  out[0] = '\0';
  if (!raw || raw[0] == '\0') {
    return;
  }

  char maskBuffer[2048];
  const size_t copyLength = std::min<size_t>(totalLength, sizeof(maskBuffer) - 1);
  memcpy(maskBuffer, raw, copyLength);
  maskBuffer[copyLength] = '\0';
  for (size_t i = 0; maskBuffer[i]; ++i) {
    const bool boundaryOk =
        i == 0 || maskBuffer[i - 1] == ' ' || maskBuffer[i - 1] == '\t' ||
        maskBuffer[i - 1] == '"' || maskBuffer[i - 1] == '-';
    if (!boundaryOk) {
      continue;
    }
    for (const char* sensitive : {
             "token", "key", "secret", "password", "passwd", "auth", "code",
             "session", "access_token", "refresh_token", "credential"}) {
      const size_t keyLength = strlen(sensitive);
      if (i + keyLength >= copyLength || maskBuffer[i + keyLength] != '=') {
        continue;
      }
      if (_strnicmp(maskBuffer + i, sensitive, keyLength) != 0) {
        continue;
      }
      size_t valueStart = i + keyLength + 1;
      const bool inQuotes = maskBuffer[valueStart] == '"';
      if (inQuotes) {
        ++valueStart;
      }
      size_t valueEnd = valueStart;
      while (maskBuffer[valueEnd] && maskBuffer[valueEnd] != ' ' &&
             maskBuffer[valueEnd] != '\t' &&
             (!inQuotes || maskBuffer[valueEnd] != '"')) {
        maskBuffer[valueEnd++] = '*';
      }
      break;
    }
  }

  const size_t excerptLength = std::min<size_t>(strlen(maskBuffer), outSize - 1);
  memcpy(out, maskBuffer, excerptLength);
  out[excerptLength] = '\0';
}

}  // namespace

namespace {
LoadLibraryA_t GetOriginalLoadLibraryA() {
  return ResolveOriginalProc(OriginalLoadLibraryA, "kernel32.dll",
                             "LoadLibraryA");
}
}

namespace {
LoadLibraryW_t GetOriginalLoadLibraryW() {
  return ResolveOriginalProc(OriginalLoadLibraryW, "kernel32.dll",
                             "LoadLibraryW");
}
}

namespace {
LoadLibraryExA_t GetOriginalLoadLibraryExA() {
  return ResolveOriginalProc(OriginalLoadLibraryExA, "kernel32.dll",
                             "LoadLibraryExA");
}
}

namespace {
LoadLibraryExW_t GetOriginalLoadLibraryExW() {
  return ResolveOriginalProc(OriginalLoadLibraryExW, "kernel32.dll",
                             "LoadLibraryExW");
}
}

namespace {
LdrLoadDll_t GetOriginalLdrLoadDll() {
  return ResolveOriginalProc(OriginalLdrLoadDll, "ntdll.dll", "LdrLoadDll");
}
}

LPSTR WINAPI HookedGetCommandLineA() {
  LPSTR original = OriginalGetCommandLineA.load(std::memory_order_acquire)();

  // Only spoof if config is loaded and feature forced
  if (g_pLocalConfig && g_pLocalConfig->graphics.forceRayReconstruction) {
    static bool s_Logged = false;
    if (!s_Logged) {
      char masked[288];
      size_t totalLength = 0;
      MaskCommandLineForLog(original, masked, sizeof(masked), totalLength);
      HookLog("HookedGetCommandLineA called (full length=%zu). Original "
              "(masked excerpt): %s",
              totalLength, masked);
      s_Logged = true;
    }

    if (main_g_SpoofedCmdLineA.empty()) {
      if (original)
        main_g_SpoofedCmdLineA = original;

      // Check if argument already exists to avoid duplication
      if (main_g_SpoofedCmdLineA.find("r.NGX.DLSS.denoisermode") ==
          std::string::npos) {
        main_g_SpoofedCmdLineA += " -r.NGX.DLSS.denoisermode=1";
        // Also force the RR feature cvar just in case (some plugins use this)
        main_g_SpoofedCmdLineA += " -r.NGX.DLSS.RayReconstruction=1";
        HookLog("HookedGetCommandLineA: Appended CVar flags.");
      }
    }
    return (LPSTR)main_g_SpoofedCmdLineA.c_str();
  }
  return original;
}

LPWSTR WINAPI HookedGetCommandLineW() {
  LPWSTR original = OriginalGetCommandLineW.load(std::memory_order_acquire)();

  if (g_pLocalConfig && g_pLocalConfig->graphics.forceRayReconstruction) {
    static bool s_Logged = false;
    if (!s_Logged) {
      // wchar conversion for logging
      char buf[2048];
      WideCharToMultiByte(CP_UTF8, 0, original, -1, buf, 2048, NULL, NULL);
      char masked[288];
      size_t totalLength = 0;
      MaskCommandLineForLog(buf, masked, sizeof(masked), totalLength);
      HookLog("HookedGetCommandLineW called (full length=%zu). Original "
              "(masked excerpt): %s",
              totalLength, masked);
      s_Logged = true;
    }

    if (main_g_SpoofedCmdLineW.empty()) {
      if (original)
        main_g_SpoofedCmdLineW = original;

      if (main_g_SpoofedCmdLineW.find(L"r.NGX.DLSS.denoisermode") ==
          std::wstring::npos) {
        main_g_SpoofedCmdLineW += L" -r.NGX.DLSS.denoisermode=1";
        main_g_SpoofedCmdLineW += L" -r.NGX.DLSS.RayReconstruction=1";
        HookLog("HookedGetCommandLineW: Appended CVar flags.");
      }
    }
    return (LPWSTR)main_g_SpoofedCmdLineW.c_str();
  }
  return original;
}

// Hooked Functions - Signal Event & Redirect
HMODULE WINAPI HookedLoadLibraryA(LPCSTR lpLibFileName) {
  LoadLibraryA_t original = GetOriginalLoadLibraryA();
  if (!original) {
    return nullptr;
  }

  if (lpLibFileName) {
    std::string redirect = GetRedirectedPath(lpLibFileName);
    if (!redirect.empty()) {
      HMODULE hMod = original(redirect.c_str());
      NotifyHookModuleLoaded(hMod, redirect.c_str());
      return hMod;
    }
  }
  HMODULE hMod = original(lpLibFileName);
  NotifyHookModuleLoaded(hMod, lpLibFileName);
  return hMod;
}

HMODULE WINAPI HookedLoadLibraryW(LPCWSTR lpLibFileName) {
  LoadLibraryW_t original = GetOriginalLoadLibraryW();
  if (!original) {
    return nullptr;
  }

  char pathUtf8[MAX_PATH] = {};
  if (lpLibFileName) {
    // Convert to UTF-8 for check
    WideCharToMultiByte(CP_UTF8, 0, lpLibFileName, -1, pathUtf8, MAX_PATH, NULL,
                        NULL);
    std::string redirect = GetRedirectedPath(pathUtf8);

    if (!redirect.empty()) {
      // Convert back to Wide for LoadLibraryW if needed, or just use A?
      // Safer to use W with W
      std::wstring redirectW;
      int len = MultiByteToWideChar(CP_UTF8, 0, redirect.c_str(), -1, NULL, 0);
      if (len > 0) {
        redirectW.resize(len);
        MultiByteToWideChar(CP_UTF8, 0, redirect.c_str(), -1, &redirectW[0],
                            len);
        // Remove null terminator added by resize if strictly needed, but
        // usually LoadLibraryW handles it
        if (redirectW.back() == L'\0')
          redirectW.pop_back();

        HMODULE hMod = original(redirectW.c_str());
        NotifyHookModuleLoaded(hMod, redirect.c_str());
        return hMod;
      }
    }
  }
  HMODULE hMod = original(lpLibFileName);
  NotifyHookModuleLoaded(hMod, pathUtf8);
  return hMod;
}

HMODULE WINAPI HookedLoadLibraryExA(LPCSTR lpLibFileName, HANDLE hFile,
                                    DWORD dwFlags) {
  LoadLibraryExA_t original = GetOriginalLoadLibraryExA();
  if (!original) {
    return nullptr;
  }

  if (lpLibFileName) {
    std::string redirect = GetRedirectedPath(lpLibFileName);
    if (!redirect.empty()) {
      // Use OriginalLoadLibraryA for the redirect to simplify (Ex flags might
      // conflict with absolute path? usually ok) But let's stick to ExA to
      // respect flags if possible, filtering flags that shouldn't apply to
      // absolute path? Actually, usually users just want to load the DLL.
      // LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR might be an issue. Let's try to trust
      // the user path is absolute.
      HMODULE hMod = original(redirect.c_str(), hFile, dwFlags);
      NotifyHookModuleLoaded(hMod, redirect.c_str());
      return hMod;
    }
  }
  HMODULE hMod = original(lpLibFileName, hFile, dwFlags);
  NotifyHookModuleLoaded(hMod, lpLibFileName);
  return hMod;
}

HMODULE WINAPI HookedLoadLibraryExW(LPCWSTR lpLibFileName, HANDLE hFile,
                                    DWORD dwFlags) {
  LoadLibraryExW_t original = GetOriginalLoadLibraryExW();
  if (!original) {
    return nullptr;
  }

  char pathUtf8[MAX_PATH] = {};
  if (lpLibFileName) {
    WideCharToMultiByte(CP_UTF8, 0, lpLibFileName, -1, pathUtf8, MAX_PATH, NULL,
                        NULL);
    std::string redirect = GetRedirectedPath(pathUtf8);
    if (!redirect.empty()) {
      std::wstring redirectW;
      int len = MultiByteToWideChar(CP_UTF8, 0, redirect.c_str(), -1, NULL, 0);
      if (len > 0) {
        redirectW.resize(len);
        MultiByteToWideChar(CP_UTF8, 0, redirect.c_str(), -1, &redirectW[0],
                            len);
        if (redirectW.back() == L'\0')
          redirectW.pop_back();

        HMODULE hMod = original(redirectW.c_str(), hFile, dwFlags);
        NotifyHookModuleLoaded(hMod, redirect.c_str());
        return hMod;
      }
    }
  }
  HMODULE hMod = original(lpLibFileName, hFile, dwFlags);
  NotifyHookModuleLoaded(hMod, pathUtf8);
  return hMod;
}

NTSTATUS NTAPI HookedLdrLoadDll(PWSTR SearchPath, PULONG DllCharacteristics,
                                PUNICODE_STRING DllName, PVOID *BaseAddress) {
  LdrLoadDll_t original = GetOriginalLdrLoadDll();
  std::string requestedPath;
  if (DllName && DllName->Buffer && DllName->Length > 0 && original) {
    std::wstring requestedW(DllName->Buffer, DllName->Length / sizeof(wchar_t));

    if (!requestedW.empty()) {
      int utf8Len =
          WideCharToMultiByte(CP_UTF8, 0, requestedW.c_str(), -1, nullptr, 0, nullptr, nullptr);
      if (utf8Len > 0) {
        requestedPath.resize(static_cast<size_t>(utf8Len));
        WideCharToMultiByte(CP_UTF8, 0, requestedW.c_str(), -1, requestedPath.data(), utf8Len, nullptr, nullptr);
        if (!requestedPath.empty() && requestedPath.back() == '\0') {
          requestedPath.pop_back();
        }

        std::string redirect = GetRedirectedPath(requestedPath);
        if (!redirect.empty()) {
          std::wstring redirectW;
          int wLen = MultiByteToWideChar(CP_UTF8, 0, redirect.c_str(), -1, NULL, 0);
          if (wLen > 0) {
            redirectW.resize(static_cast<size_t>(wLen));
            MultiByteToWideChar(CP_UTF8, 0, redirect.c_str(), -1, redirectW.data(), wLen);
            if (!redirectW.empty() && redirectW.back() == L'\0') {
              redirectW.pop_back();
            }

            UNICODE_STRING redirectName{};
            redirectName.Buffer = const_cast<PWSTR>(redirectW.c_str());
            redirectName.Length = static_cast<USHORT>(redirectW.size() * sizeof(wchar_t));
            redirectName.MaximumLength = redirectName.Length + sizeof(wchar_t);

            NTSTATUS status = original(SearchPath, DllCharacteristics,
                                       &redirectName, BaseAddress);
            if (NT_SUCCESS(status)) {
              NotifyHookModuleLoaded(BaseAddress ? (HMODULE)*BaseAddress : nullptr,
                                     redirect.c_str());
              return status;
            }
          }
        }
      }
    }
  }

  if (!original)
    return STATUS_DLL_NOT_FOUND;

  NTSTATUS status = original(SearchPath, DllCharacteristics, DllName,
                             BaseAddress);
  if (NT_SUCCESS(status))
    NotifyHookModuleLoaded(BaseAddress ? (HMODULE)*BaseAddress : nullptr,
                           requestedPath.c_str());
  return status;
}
