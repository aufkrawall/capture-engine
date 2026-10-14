// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall
#pragma once
#include <windows.h>

#include <string>
#include <string_view>
#include "hook/runtime/graphics_runtime_module_policy.h"

// A configured runtime path may name a directory or any DLL within the set.
inline std::string BuildOverridePath(const std::string &overridePath, const std::string &filename) {
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

// Runtime-override paths are UTF-8 inside the hook. The loader hooks, the DLL
// notification and every path comparison already work in UTF-8, while
// config.ini values arrive as active-code-page text. Mixing the two made a
// configured folder below a non-ASCII name (a user folder with an umlaut,
// U+00FC encoded as 0xFC in cp1252 but 0xC3 0xBC in UTF-8) resolve to a different file on
// each side: the A-API existence check found it, the CP_UTF8 decode for the
// actual load named a file that does not exist, and CE's own sl.common copy
// looked foreign to the loader notification. Convert once at the config
// boundary, and touch the file system only through the wide APIs below.
namespace ce::runtime_override_path {

// Strict: invalid input yields "" rather than U+FFFD, so two different
// malformed paths can never compare or resolve as the same file.
inline std::wstring Utf8ToWide(std::string_view utf8) {
  if (utf8.empty()) {
    return {};
  }
  const int needed = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), static_cast<int>(utf8.size()),
                                         nullptr, 0);
  if (needed <= 0) {
    return {};
  }
  std::wstring wide(static_cast<size_t>(needed), L'\0');
  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), static_cast<int>(utf8.size()), wide.data(),
                      needed);
  return wide;
}

inline std::string WideToUtf8(std::wstring_view wide) {
  if (wide.empty()) {
    return {};
  }
  const int needed =
      WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
  if (needed <= 0) {
    return {};
  }
  std::string utf8(static_cast<size_t>(needed), '\0');
  WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), utf8.data(), needed, nullptr, nullptr);
  return utf8;
}

// Config text (code page `codePage`, the active code page in production) to UTF-8.
inline std::string CodePageToUtf8(std::string_view text, UINT codePage = CP_ACP) {
  if (text.empty()) {
    return {};
  }
  const int needed = MultiByteToWideChar(codePage, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
  if (needed <= 0) {
    return {};
  }
  std::wstring wide(static_cast<size_t>(needed), L'\0');
  MultiByteToWideChar(codePage, 0, text.data(), static_cast<int>(text.size()), wide.data(), needed);
  return WideToUtf8(wide);
}

inline bool Exists(const std::string &utf8Path) {
  const std::wstring wide = Utf8ToWide(utf8Path);
  return !wide.empty() && GetFileAttributesW(wide.c_str()) != INVALID_FILE_ATTRIBUTES;
}

// GetFullPathNameW is pure string work. The loader reports canonical paths, so
// a relative or ".."-carrying configuration must not make CE's copy look foreign.
inline std::string Canonical(const std::string &utf8Path) {
  const std::wstring wide = Utf8ToWide(utf8Path);
  if (wide.empty()) {
    return utf8Path;
  }
  const DWORD needed = GetFullPathNameW(wide.c_str(), 0, nullptr, nullptr);
  if (needed == 0) {
    return utf8Path;
  }
  std::wstring full(static_cast<size_t>(needed), L'\0');
  const DWORD written = GetFullPathNameW(wide.c_str(), needed, full.data(), nullptr);
  if (written == 0 || written >= needed) {
    return utf8Path;
  }
  full.resize(written);
  return WideToUtf8(full);
}

// The module's loader path in UTF-8, or "" when it cannot be read. No MAX_PATH
// truncation: a truncated path would name a different file.
inline std::string ModulePath(HMODULE module) {
  if (!module) {
    return {};
  }
  std::wstring path(MAX_PATH, L'\0');
  for (int attempt = 0; attempt < 4; ++attempt) {
    const DWORD length = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0) {
      return {};
    }
    if (length < path.size()) {
      path.resize(length);
      return WideToUtf8(path);
    }
    path.resize(path.size() * 4);
  }
  return {};
}

}  // namespace ce::runtime_override_path

// BuildOverridePath, canonicalized; both the input and the result are UTF-8.
inline std::string CanonicalOverridePath(const std::string &overridePath, const std::string &filename) {
  const std::string path = BuildOverridePath(overridePath, filename);
  return path.empty() ? path : ce::runtime_override_path::Canonical(path);
}
