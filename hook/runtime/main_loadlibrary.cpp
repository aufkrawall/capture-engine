#include "main_internal.h"
#include "module_replacement.h"
#include "runtime_override_paths.h"

std::atomic<LoadLibraryA_t> OriginalLoadLibraryA{nullptr};

std::atomic<LoadLibraryW_t> OriginalLoadLibraryW{nullptr};

std::atomic<LoadLibraryExA_t> OriginalLoadLibraryExA{nullptr};

std::atomic<LoadLibraryExW_t> OriginalLoadLibraryExW{nullptr};

std::atomic<LdrLoadDll_t> OriginalLdrLoadDll{nullptr};

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

// Loads a configured runtime override DLL through the original (non-hooked)
// loader entry so the runtime preload does not re-enter the redirect hook, and
// notifies CE's module hooks (NVNGX inline hooks, Streamline hook install, FFX
// bridge) before the game can use the module.
HMODULE LoadRuntimeDllViaOriginal(const wchar_t *fullPath, const char *utf8Path) {
  LoadLibraryW_t original = GetOriginalLoadLibraryW();
  if (!original || !fullPath || !utf8Path) {
    return nullptr;
  }

  HMODULE hMod = original(fullPath);
  if (hMod) {
    NotifyHookModuleLoaded(hMod, utf8Path);
  }
  return hMod;
}

namespace {
// The redirect speaks UTF-8 on both sides (see runtime_override_paths.h). An
// ANSI request is code-page text, and an ANSI loader entry cannot carry a
// redirect target outside the code page, so every redirected load goes
// through the wide original.
std::string AnsiRequestToUtf8(LPCSTR path) {
  return path ? ce::runtime_override_path::CodePageToUtf8(path) : std::string();
}

std::string WideRequestToUtf8(LPCWSTR path) {
  return path ? ce::runtime_override_path::WideToUtf8(path) : std::string();
}

// A request for a module a configured custom DLL replaces (module_replacement.h) loads the
// replacement's own path, which returns the image already mapped. Data-file and resource loads
// keep their file: they read it, they do not run it. Only these module-level hooks know the
// caller; LdrLoadDll is shared with Windows' and CE's own loads, so it never substitutes.
HMODULE LoadReplacementFor(LPCWSTR request, const void *callerAddress, DWORD flags) {
  constexpr DWORD kNonExecutableLoads = LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_DATAFILE_EXCLUSIVE |
                                        LOAD_LIBRARY_AS_IMAGE_RESOURCE | DONT_RESOLVE_DLL_REFERENCES;
  if (!request || (flags & kNonExecutableLoads) != 0) {
    return nullptr;
  }
  const wchar_t *replacement = ce::module_replacement::ReplacementPathForLoad(request, callerAddress);
  LoadLibraryW_t originalW = replacement ? GetOriginalLoadLibraryW() : nullptr;
  if (!originalW) {
    return nullptr;
  }
  HMODULE hMod = originalW(replacement);
  NotifyHookModuleLoaded(hMod, WideRequestToUtf8(replacement).c_str());
  return hMod;
}

HMODULE LoadReplacementFor(const std::string &utf8Request, const void *callerAddress, DWORD flags) {
  if (utf8Request.empty() || !ce::module_replacement::IsActive()) {
    return nullptr;
  }
  return LoadReplacementFor(ce::runtime_override_path::Utf8ToWide(utf8Request).c_str(), callerAddress, flags);
}
}  // namespace

// Hooked Functions - Signal Event & Redirect
HMODULE WINAPI HookedLoadLibraryA(LPCSTR lpLibFileName) {
  LoadLibraryA_t original = GetOriginalLoadLibraryA();
  if (!original) {
    return nullptr;
  }
  if (HookIsShuttingDown()) {
    return original(lpLibFileName);
  }

  const std::string requested = AnsiRequestToUtf8(lpLibFileName);
  if (HMODULE replaced = LoadReplacementFor(requested, __builtin_return_address(0), 0)) {
    return replaced;
  }
  if (!requested.empty()) {
    const std::string redirect = GetRedirectedPath(requested);
    const std::wstring redirectW = ce::runtime_override_path::Utf8ToWide(redirect);
    LoadLibraryW_t originalW = GetOriginalLoadLibraryW();
    if (!redirectW.empty() && originalW) {
      HMODULE hMod = originalW(redirectW.c_str());
      NotifyHookModuleLoaded(hMod, redirect.c_str());
      return hMod;
    }
  }
  HMODULE hMod = original(lpLibFileName);
  NotifyHookModuleLoaded(hMod, requested.c_str());
  return hMod;
}

HMODULE WINAPI HookedLoadLibraryW(LPCWSTR lpLibFileName) {
  LoadLibraryW_t original = GetOriginalLoadLibraryW();
  if (!original) {
    return nullptr;
  }
  if (HookIsShuttingDown()) {
    return original(lpLibFileName);
  }
  if (HMODULE replaced = LoadReplacementFor(lpLibFileName, __builtin_return_address(0), 0)) {
    return replaced;
  }

  const std::string requested = WideRequestToUtf8(lpLibFileName);
  if (!requested.empty()) {
    const std::string redirect = GetRedirectedPath(requested);
    const std::wstring redirectW = ce::runtime_override_path::Utf8ToWide(redirect);
    if (!redirectW.empty()) {
      HMODULE hMod = original(redirectW.c_str());
      NotifyHookModuleLoaded(hMod, redirect.c_str());
      return hMod;
    }
  }
  HMODULE hMod = original(lpLibFileName);
  NotifyHookModuleLoaded(hMod, requested.c_str());
  return hMod;
}

HMODULE WINAPI HookedLoadLibraryExA(LPCSTR lpLibFileName, HANDLE hFile,
                                    DWORD dwFlags) {
  LoadLibraryExA_t original = GetOriginalLoadLibraryExA();
  if (!original) {
    return nullptr;
  }
  if (HookIsShuttingDown()) {
    return original(lpLibFileName, hFile, dwFlags);
  }

  const std::string requested = AnsiRequestToUtf8(lpLibFileName);
  if (HMODULE replaced = LoadReplacementFor(requested, __builtin_return_address(0), dwFlags)) {
    return replaced;
  }
  if (!requested.empty()) {
    // The redirect target is absolute; the caller's flags still apply.
    const std::string redirect = GetRedirectedPath(requested);
    const std::wstring redirectW = ce::runtime_override_path::Utf8ToWide(redirect);
    LoadLibraryExW_t originalW = GetOriginalLoadLibraryExW();
    if (!redirectW.empty() && originalW) {
      HMODULE hMod = originalW(redirectW.c_str(), hFile, dwFlags);
      NotifyHookModuleLoaded(hMod, redirect.c_str());
      return hMod;
    }
  }
  HMODULE hMod = original(lpLibFileName, hFile, dwFlags);
  NotifyHookModuleLoaded(hMod, requested.c_str());
  return hMod;
}

HMODULE WINAPI HookedLoadLibraryExW(LPCWSTR lpLibFileName, HANDLE hFile,
                                    DWORD dwFlags) {
  LoadLibraryExW_t original = GetOriginalLoadLibraryExW();
  if (!original) {
    return nullptr;
  }
  if (HookIsShuttingDown()) {
    return original(lpLibFileName, hFile, dwFlags);
  }
  if (HMODULE replaced = LoadReplacementFor(lpLibFileName, __builtin_return_address(0), dwFlags)) {
    return replaced;
  }

  const std::string requested = WideRequestToUtf8(lpLibFileName);
  if (!requested.empty()) {
    const std::string redirect = GetRedirectedPath(requested);
    const std::wstring redirectW = ce::runtime_override_path::Utf8ToWide(redirect);
    if (!redirectW.empty()) {
      HMODULE hMod = original(redirectW.c_str(), hFile, dwFlags);
      NotifyHookModuleLoaded(hMod, redirect.c_str());
      return hMod;
    }
  }
  HMODULE hMod = original(lpLibFileName, hFile, dwFlags);
  NotifyHookModuleLoaded(hMod, requested.c_str());
  return hMod;
}

NTSTATUS NTAPI HookedLdrLoadDll(PWSTR SearchPath, PULONG DllCharacteristics,
                                PUNICODE_STRING DllName, PVOID *BaseAddress) {
  LdrLoadDll_t original = GetOriginalLdrLoadDll();
  if (!original)
    return STATUS_DLL_NOT_FOUND;
  if (HookIsShuttingDown())
    return original(SearchPath, DllCharacteristics, DllName, BaseAddress);

  std::string requestedPath;
  if (DllName && DllName->Buffer && DllName->Length > 0) {
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

  NTSTATUS status = original(SearchPath, DllCharacteristics, DllName,
                             BaseAddress);
  if (NT_SUCCESS(status))
    NotifyHookModuleLoaded(BaseAddress ? (HMODULE)*BaseAddress : nullptr,
                           requestedPath.c_str());
  return status;
}
