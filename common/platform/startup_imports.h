// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall
#pragma once

#include <windows.h>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace ce::startup_imports {

enum class Status { Unchanged, Redirected, InvalidImage, UnsupportedImage, Failed, RollbackFailed };

struct Import {
    DWORD descriptorRva = 0;
    IMAGE_IMPORT_DESCRIPTOR descriptor{};
    std::string name;
};

struct Image {
    WORD machine = 0;
    DWORD size = 0;
    DWORD boundDirectoryRva = 0;
    IMAGE_DATA_DIRECTORY boundDirectory{};
    std::vector<Import> imports;
};

// Reads mapped-image RVAs, never raw-file offsets. Bounded and shared by the
// remote reader, regression fixtures and fuzz harness.
using Reader = std::function<bool(DWORD, std::span<std::byte>)>;
Status Inspect(const Reader& read, Image& image);

struct Result {
    Status status = Status::Unchanged;
    DWORD error = ERROR_SUCCESS;
    WORD machine = 0;
    unsigned redirected = 0;
};

// Caller owns a newly-created process whose primary thread has never run.
// No remote thread/APC may be started until this transaction has completed.
// Replaces an existing import's name, retaining its lookup and address tables;
// the on-disk executable and DLLs are never modified. RollbackFailed requires
// terminating the child, rather than allowing a damaged image to execute.
Result Redirect(HANDLE process, const std::string& module, const std::string& absolutePath);

}  // namespace ce::startup_imports
