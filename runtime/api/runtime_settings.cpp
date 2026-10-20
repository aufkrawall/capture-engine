// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall
// The settings functions of the C ABI (llm-wiki/library/03 "Settings"). Reads go through the runtime's settings reader.
// An edit is a client-side list of changes: ce_settings_commit checks it on the calling thread and admits it as one
// engine command, which the engine thread writes and fans out. The edit keeps its runtime handle and resolves it on
// every use, so an edit whose runtime was destroyed is refused instead of touching freed memory.
#include "include/cengine/cengine.h"

#include "common/logging/logging.h"
#include "runtime/api/capi_text.h"
#include "runtime/api/handle_registry.h"
#include "runtime/api/runtime_handle.h"
#include "runtime/core/core.h"

#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// What ce_settings_begin hands out. Owned by the client until ce_settings_discard or a successful ce_settings_commit.
// Like every handle of the ABI it is used by one thread at a time.
struct ce_settings_edit {
    ce_runtime* runtime = nullptr;  // resolved on every use (a destroyed runtime is refused)
    uint64_t baseRevision = 0;      // the settings revision the edit was begun against
    std::vector<ce::runtime::SettingsEdit> edits;
    ConfigDiagnostics diagnostics;  // of the last commit attempt; replaced by each attempt
};

namespace ce::capi {
namespace {

// A section or key names one line of config.ini; a value is one line of text. Longer text is refused, not truncated.
constexpr size_t kMaxSettingNameBytes = 256;
constexpr size_t kMaxSettingValueBytes = 4096;

int32_t DiagnosticSeverity(ConfigDiagnosticSeverity severity) {
    return severity == ConfigDiagnosticSeverity::kError ? CE_DIAG_ERROR : CE_DIAG_WARNING;
}

}  // namespace
}  // namespace ce::capi

extern "C" {

ce_status_t ce_settings_get(ce_runtime_t* rt, const char* section, const char* key, char* buf, size_t capacity,
                            size_t* needed) {
    if (needed)
        *needed = 0;
    return ce::capi::RunGuarded("ce_settings_get", [&]() -> ce_status_t {
        if (!section || !key)
            return CE_E_INVALID_ARGUMENT;
        ce_runtime* runtime = ce::capi::ResolveRuntime(rt, "ce_settings_get");
        if (!runtime)
            return CE_E_INVALID_ARGUMENT;
        std::string sectionText;
        std::string keyText;
        if (ce::capi::ReadClientText(section, ce::capi::kMaxSettingNameBytes, sectionText) != CE_OK ||
            ce::capi::ReadClientText(key, ce::capi::kMaxSettingNameBytes, keyText) != CE_OK)
            return CE_E_INVALID_ARGUMENT;
        const std::optional<std::string> value = runtime->engine->Settings().Value(sectionText, keyText);
        if (!value)
            return CE_E_NOT_FOUND;
        const size_t required = value->size() + 1;  // the terminator
        if (needed)
            *needed = required;
        if (!buf)
            return CE_OK;
        if (capacity < required)
            return CE_E_BUFFER_TOO_SMALL;
        std::memcpy(buf, value->c_str(), required);
        return CE_OK;
    });
}

ce_status_t ce_settings_begin(ce_runtime_t* rt, ce_settings_edit_t** out_edit) {
    if (!out_edit)
        return CE_E_INVALID_ARGUMENT;
    *out_edit = nullptr;
    return ce::capi::RunGuarded("ce_settings_begin", [&]() -> ce_status_t {
        ce_runtime* runtime = ce::capi::ResolveRuntime(rt, "ce_settings_begin");
        if (!runtime)
            return CE_E_INVALID_ARGUMENT;
        auto edit = std::make_unique<ce_settings_edit>();
        edit->runtime = runtime;
        edit->baseRevision = runtime->engine->Settings().Revision();
        *out_edit = edit.release();
        return CE_OK;
    });
}

ce_status_t ce_settings_set(ce_settings_edit_t* edit, const char* section, const char* key, const char* value) {
    return ce::capi::RunGuarded("ce_settings_set", [&]() -> ce_status_t {
        if (!edit || !section || !key)
            return CE_E_INVALID_ARGUMENT;
        ce::runtime::SettingsEdit item;
        if (ce::capi::ReadClientText(section, ce::capi::kMaxSettingNameBytes, item.section) != CE_OK ||
            ce::capi::ReadClientText(key, ce::capi::kMaxSettingNameBytes, item.key) != CE_OK)
            return CE_E_INVALID_ARGUMENT;
        if (value) {
            std::string text;
            const ce_status_t status = ce::capi::ReadClientText(value, ce::capi::kMaxSettingValueBytes, text);
            if (status != CE_OK)
                return status;
            item.value = std::move(text);
        }
        edit->edits.push_back(std::move(item));
        return CE_OK;
    });
}

ce_status_t ce_settings_commit(ce_settings_edit_t* edit, ce_request_t* out) {
    if (out)
        *out = 0;
    if (!edit)
        return CE_E_INVALID_ARGUMENT;
    return ce::capi::RunGuarded("ce_settings_commit", [&]() -> ce_status_t {
        ce_runtime* runtime = ce::capi::ResolveRuntime(edit->runtime, "ce_settings_commit");
        if (!runtime)
            return CE_E_INVALID_ARGUMENT;
        // The check runs here, on the calling thread: a refused edit stays open with its diagnostics.
        const ce::runtime::CommitResult check = runtime->engine->Settings().Check(edit->edits, edit->baseRevision);
        edit->diagnostics = check.diagnostics;
        switch (check.status) {
            case ce::runtime::CommitStatus::Committed:
                break;
            case ce::runtime::CommitStatus::Invalid:
                LogDebug("[CApi] ce_settings_commit refused by the check: invalid (%zu diagnostic(s))",
                         check.diagnostics.entries.size());
                return CE_E_SETTINGS;
            case ce::runtime::CommitStatus::Conflict:
                LogDebug("[CApi] ce_settings_commit refused by the check: conflict at revision %llu",
                         static_cast<unsigned long long>(edit->baseRevision));
                return CE_E_CONFLICT;
            case ce::runtime::CommitStatus::IoError:
                LogDebug("[CApi] ce_settings_commit refused by the check: config.ini unavailable");
                return CE_E_IO;
        }
        ce::engine::Command command;
        command.kind = ce::engine::CommandKind::CommitSettings;
        command.edits = edit->edits;
        command.baseRevision = edit->baseRevision;
        const ce::engine::Admission admission = runtime->engine->Submit(std::move(command));
        if (admission.status != CE_OK)
            return admission.status;  // not admitted: the edit stays open
        if (out)
            *out = admission.request;
        delete edit;  // consumed: the engine thread owns the change now
        return CE_OK;
    });
}

ce_status_t ce_settings_diagnostic_count(const ce_settings_edit_t* edit, uint32_t* count) {
    if (count)
        *count = 0;
    if (!edit || !count)
        return CE_E_INVALID_ARGUMENT;
    *count = static_cast<uint32_t>(edit->diagnostics.entries.size());
    return CE_OK;
}

ce_status_t ce_settings_get_diagnostic(const ce_settings_edit_t* edit, uint32_t index, ce_settings_diagnostic_t* out) {
    if (!edit || !out || index >= edit->diagnostics.entries.size())
        return CE_E_INVALID_ARGUMENT;
    const ConfigDiagnostic& entry = edit->diagnostics.entries[index];
    out->struct_size = sizeof(*out);
    out->severity = ce::capi::DiagnosticSeverity(entry.severity);
    out->section = entry.section.c_str();
    out->key = entry.key.c_str();
    out->message = entry.message.c_str();
    return CE_OK;
}

void ce_settings_discard(ce_settings_edit_t* edit) {
    delete edit;
}

}  // extern "C"
