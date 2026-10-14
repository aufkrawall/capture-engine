#pragma once

#include <windows.h>

#include <cstddef>
#include <string>
#include <vector>

#include "hook/streamline/streamline_takeover_import_coverage.h"

// The import-table side of the sl.interposer.dll takeover: which modules import
// the interposer, whether every imported name can move to the configured copy,
// and moving the names the thunks do not gate.
namespace ce::streamline_interposer_takeover {

bool ModuleImportsDll(HMODULE module, const char* sourceModule);

// Whether `name` is one of kTakeoverExportNames.
bool IsThunkedExportName(const char* name);

// Measures what `modules` import from sl.interposer.dll against the thunk
// table and the export table of `overridePathUtf8` (read from disk, loader-free).
ImportCoverage MeasureImportCoverage(const std::vector<HMODULE>& modules, const std::string& overridePathUtf8);

struct DirectRebindResult {
    size_t rebound = 0;
    size_t failed = 0;
    std::string firstFailed;
};

// Rebinds every sl.interposer.dll import of `module` that has no thunk from the
// superseded interposer's export to the custom interposer's export of the same
// name. Thunked names are left to the caller.
DirectRebindResult RebindUnthunkedImports(HMODULE module, HMODULE superseded, HMODULE custom);

}  // namespace ce::streamline_interposer_takeover
