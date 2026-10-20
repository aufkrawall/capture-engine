// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall
// ce_runtime_log: one client line in the runtime's log (llm-wiki/library/03 "Log").
#include "include/cengine/cengine.h"

#include "common/logging/logging.h"
#include "runtime/api/capi_text.h"
#include "runtime/api/handle_registry.h"
#include "runtime/api/runtime_desc.h"
#include "runtime/api/runtime_handle.h"

#include <string>
#include <string_view>

namespace ce::capi {
namespace {

// CE_LOG_* to the logger's levels. False for a level the API does not define.
bool MapLogLevel(int32_t level, LogLevel& out) {
    switch (level) {
        case CE_LOG_ERROR:
            out = LogLevel::Error;
            return true;
        case CE_LOG_WARN:
            out = LogLevel::Warn;
            return true;
        case CE_LOG_INFO:
            out = LogLevel::Info;
            return true;
        case CE_LOG_DEBUG:
            out = LogLevel::Debug;
            return true;
        default:
            return false;
    }
}

}  // namespace
}  // namespace ce::capi

extern "C" ce_status_t ce_runtime_log(ce_runtime_t* rt, int32_t level, const char* message) {
    return ce::capi::RunGuarded("ce_runtime_log", [&]() -> ce_status_t {
        ce_runtime* runtime = ce::capi::ResolveRuntime(rt, "ce_runtime_log");
        if (!runtime)
            return CE_E_INVALID_ARGUMENT;
        LogLevel mapped = LogLevel::Off;
        if (!message || !ce::capi::MapLogLevel(level, mapped))
            return CE_E_INVALID_ARGUMENT;
        const std::string_view whole(message);
        if (!ce::capi::IsValidUtf8(whole))
            return CE_E_INVALID_ARGUMENT;
        // Shortened, never refused: a truncated line still says what happened. Passed as an argument, not as the
        // format, so a '%' in the client's text is only text.
        const std::string_view shortened = ce::capi::TruncateUtf8(whole, ce::capi::kMaxLogMessageBytes);
        const std::string text = ce::capi::SanitizeLogText(shortened);
        Log(mapped, "[Client:%s] %s", runtime->clientName.c_str(), text.c_str());
        return CE_OK;
    });
}
