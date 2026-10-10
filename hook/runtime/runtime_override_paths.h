// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall
#pragma once
#include <string>
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
