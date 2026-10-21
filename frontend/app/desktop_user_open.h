// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall
#pragma once

// Opening a file the person at the desktop edits by hand (config.ini) in their own editor, as themselves.

#include <windows.h>

#include <string>

namespace ce::frontend {

// True when this process runs with an elevated (administrator) token.
bool IsProcessElevated();

struct OpenResult {
    bool opened = false;
    std::string description;  // one log line's worth: the route taken and, on failure, why
};

// Opens `path` with its file association. An elevated app hands the open to the desktop shell, so the editor runs
// unelevated as the person at the desktop: an elevated editor that saves through a temporary file and a rename
// replaces config.ini with a copy only administrators can write, and every later edit then asks for UAC. Without a
// reachable desktop shell the file is opened directly, and the editor is elevated.
OpenResult OpenForDesktopUser(const std::wstring& path);

}  // namespace ce::frontend
