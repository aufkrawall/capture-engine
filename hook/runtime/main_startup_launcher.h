// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall
#pragma once
#include <windows.h>

// Explicit host-selected role, checked before any graphics/crash-handler setup.
bool StartCreationOnlyLauncher(HMODULE module);
bool InstallStartupCreationHook();
