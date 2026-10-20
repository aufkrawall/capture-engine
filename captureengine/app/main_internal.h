#pragma once

// clang-format off
#include <windows.h>

#include <shellapi.h>

// clang-format on
#include <algorithm>

#include <atomic>

#include <filesystem>

#include <fstream>

#include <string>

#include <string_view>

#include <vector>

#include "common/config/config.h"

#include "common/crash/crash_handler.h"

#include "common/logging/logging.h"

#include "common/logging/log_privacy.h"

#include "common/platform/monitor_selection.h"

#include "common/ipc/process_ipc.h"

#include "common/ipc/shared_defs.h"

#include "common/platform/strict_integer_parse.h"

#include "common/crash/wer_dump_adoption.h"

#include "captureengine/diagnostics/dump_helper.h"

#include "captureengine/sensors/pawnio_setup.h"

#include "captureengine/media/process_loopback_worker_host.h"

#include "captureengine/media/recording_manifest.h"

#include "captureengine/sensors/sensor_bridge_host.h"

#include "captureengine/app/role_configuration.h"
#include "common/platform/runtime_package_paths.h"

using ce::runtime::RuntimeConfiguration;

// Forward declarations for process entry points
extern int InjectProcessMain(const AppConfig& config);

extern int MediaProcessMain(const AppConfig& config);

extern int LoggerProcessMain(const AppConfig& config);

extern int SensorProcessMain(const AppConfig& config);

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow);

inline bool HasExactCommandLineArgument(const wchar_t* expected) {
    int argumentCount = 0;
    wchar_t** arguments = CommandLineToArgvW(GetCommandLineW(), &argumentCount);
    if (!arguments)
        return false;
    bool found = false;
    for (int index = 1; index < argumentCount; ++index) {
        if (_wcsicmp(arguments[index], expected) == 0) {
            found = true;
            break;
        }
    }
    LocalFree(reinterpret_cast<HLOCAL>(arguments));
    return found;
}
