// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall
// Unshipped client-side wrapper. Only the C draft is used across the boundary.
#pragma once

#include "cengine_draft.h"

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cengine {
class Status {
public:
    constexpr explicit Status(ce_status_t code = CE_OK) noexcept : code_(code) {}
    constexpr bool ok() const noexcept {
        return code_ == CE_OK;
    }
    constexpr explicit operator bool() const noexcept {
        return ok();
    }
    constexpr ce_status_t code() const noexcept {
        return code_;
    }
    const char* message() const noexcept {
        return ce_status_string(code_);
    }

private:
    ce_status_t code_;
};

enum class Mode : int32_t { Video = CE_MODE_VIDEO, AudioOnly = CE_MODE_AUDIO_ONLY };
enum class LogLevel : int32_t { Error = CE_LOG_ERROR, Warning = CE_LOG_WARN, Info = CE_LOG_INFO, Debug = CE_LOG_DEBUG };

struct Options {
    std::optional<std::string> packageDir;
    std::optional<std::string> dataDir;
    std::optional<std::string> clientName;
    uint32_t features = 0;
};

struct StatusInfo {
    Status result;
    ce_status_info_t value{};
};

struct Diagnostic {
    int32_t severity;
    std::string section;
    std::string key;
    std::string message;
};

namespace detail {
constexpr std::optional<uint32_t> Timeout(std::chrono::milliseconds budget) noexcept {
    const auto count = budget.count();
    if (count < 0 || count >= UINT32_MAX)
        return std::nullopt;
    return static_cast<uint32_t>(count);
}

constexpr bool HasNull(std::string_view value) noexcept {
    return value.find('\0') != std::string_view::npos;
}

// Events/edits keep the owner alive. Cleanup is bounded and never forces helpers;
// call Runtime::shutdown explicitly to retain the handle and retry a timeout.
struct RuntimeOwner {
    ce_runtime_t* handle = nullptr;
    ~RuntimeOwner() noexcept {
        if (handle) {
            (void)ce_runtime_shutdown(handle, 30000);
            (void)ce_runtime_destroy(handle);
        }
    }
};
}  // namespace detail

class Runtime;

class Event {
public:
    Event() noexcept = default;
    Event(const Event&) = delete;
    Event& operator=(const Event&) = delete;
    Event(Event&& other) noexcept : owner_(std::move(other.owner_)), value_(std::exchange(other.value_, nullptr)) {}
    Event& operator=(Event&& other) noexcept {
        if (this != &other) {
            reset();
            owner_ = std::move(other.owner_);
            value_ = std::exchange(other.value_, nullptr);
        }
        return *this;
    }
    ~Event() noexcept {
        reset();
    }
    explicit operator bool() const noexcept {
        return value_ != nullptr;
    }
    int32_t type() const noexcept {
        return value_ ? value_->type : 0;
    }
    uint64_t sequence() const noexcept {
        return value_ ? value_->sequence : 0;
    }
    ce_request_t request() const noexcept {
        return value_ ? value_->request : 0;
    }
    Status status() const noexcept {
        return Status(value_ ? value_->status : CE_E_INVALID_STATE);
    }
    const ce_event_t* raw() const noexcept {
        return value_;
    }
    void reset() noexcept {
        if (value_)
            (void)ce_runtime_release_event(owner_->handle, std::exchange(value_, nullptr));
        owner_.reset();
    }

private:
    friend class Runtime;
    std::shared_ptr<detail::RuntimeOwner> owner_;
    const ce_event_t* value_ = nullptr;
};

class SettingsEdit {
public:
    SettingsEdit() noexcept = default;
    SettingsEdit(const SettingsEdit&) = delete;
    SettingsEdit& operator=(const SettingsEdit&) = delete;
    SettingsEdit(SettingsEdit&& other) noexcept
        : owner_(std::move(other.owner_)),
          edit_(std::exchange(other.edit_, nullptr)) {}
    SettingsEdit& operator=(SettingsEdit&& other) noexcept {
        if (this != &other) {
            reset();
            owner_ = std::move(other.owner_);
            edit_ = std::exchange(other.edit_, nullptr);
        }
        return *this;
    }
    ~SettingsEdit() noexcept {
        reset();
    }
    explicit operator bool() const noexcept {
        return edit_ != nullptr;
    }
    Status set(std::string_view section, std::string_view key, std::optional<std::string_view> value) {
        if (detail::HasNull(section) || detail::HasNull(key) || (value && detail::HasNull(*value)))
            return Status(CE_E_INVALID_ARGUMENT);
        const std::string sectionText(section), keyText(key);
        const std::optional<std::string> valueText = value ? std::optional<std::string>(*value) : std::nullopt;
        return Status(
            ce_settings_set(edit_, sectionText.c_str(), keyText.c_str(), valueText ? valueText->c_str() : nullptr));
    }
    Status commit(ce_request_t* request = nullptr) noexcept {
        const Status result(ce_settings_commit(edit_, request));
        if (result) {
            edit_ = nullptr;  // Consumed only on successful admission.
            owner_.reset();
        }
        return result;
    }
    Status diagnostics(std::vector<Diagnostic>& out) const {
        uint32_t count = 0;
        Status result(ce_settings_diagnostic_count(edit_, &count));
        if (!result)
            return result;
        std::vector<Diagnostic> items;
        items.reserve(count);
        for (uint32_t index = 0; index < count; ++index) {
            ce_settings_diagnostic_t item{};
            item.struct_size = sizeof(item);
            result = Status(ce_settings_get_diagnostic(edit_, index, &item));
            if (!result)
                return result;
            items.push_back({item.severity, item.section ? item.section : "", item.key ? item.key : "",
                             item.message ? item.message : ""});
        }
        out = std::move(items);
        return Status();
    }
    void reset() noexcept {
        if (edit_)
            ce_settings_discard(std::exchange(edit_, nullptr));
        owner_.reset();
    }

private:
    friend class Runtime;
    std::shared_ptr<detail::RuntimeOwner> owner_;
    ce_settings_edit_t* edit_ = nullptr;
};

class Runtime {
public:
    Runtime() noexcept = default;
    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;
    Runtime(Runtime&&) noexcept = default;
    Runtime& operator=(Runtime&&) noexcept = default;
    ~Runtime() = default;
    explicit operator bool() const noexcept {
        return raw() != nullptr;
    }
    ce_runtime_t* raw() const noexcept {
        return owner_ ? owner_->handle : nullptr;
    }
    static Status create(const Options& options, Runtime& out) {
        if (out.owner_)
            return Status(CE_E_INVALID_STATE);
        for (const auto* text : {&options.packageDir, &options.dataDir, &options.clientName}) {
            if (*text && detail::HasNull(**text))
                return Status(CE_E_INVALID_ARGUMENT);
        }
        auto owner = std::make_shared<detail::RuntimeOwner>();
        ce_runtime_desc_t desc{};
        const Status initialized(ce_runtime_desc_init(&desc, sizeof(desc), CE_API_VERSION));
        if (!initialized)
            return initialized;
        desc.package_dir = options.packageDir ? options.packageDir->c_str() : nullptr;
        desc.data_dir = options.dataDir ? options.dataDir->c_str() : nullptr;
        desc.client_name = options.clientName ? options.clientName->c_str() : nullptr;
        desc.features = options.features;
        const Status result(ce_runtime_create(&desc, &owner->handle));
        if (result)
            out.owner_ = std::move(owner);
        return result;
    }
    Status requestShutdown() noexcept {
        return Status(ce_runtime_request_shutdown(raw()));
    }
    Status shutdown(std::chrono::milliseconds budget) noexcept {
        const auto timeout = detail::Timeout(budget);
        return timeout ? Status(ce_runtime_shutdown(raw(), *timeout)) : Status(CE_E_INVALID_ARGUMENT);
    }
    Status destroy() noexcept {
        if (!owner_ || !owner_->handle)
            return Status(CE_E_INVALID_ARGUMENT);
        if (owner_ && owner_.use_count() != 1)
            return Status(CE_E_INVALID_STATE);
        const Status result(ce_runtime_destroy(raw()));
        if (result) {
            owner_->handle = nullptr;
            owner_.reset();
        }
        return result;
    }
    StatusInfo status() const noexcept {
        StatusInfo out;
        out.value.struct_size = sizeof(out.value);
        out.result = Status(ce_runtime_get_status(raw(), &out.value));
        return out;
    }
    Status startRecording(Mode mode, std::string_view reason, ce_request_t* request = nullptr) {
        return recording(ce_recording_start, mode, reason, request);
    }
    Status stopRecording(std::string_view reason, ce_request_t* request = nullptr) {
        if (detail::HasNull(reason))
            return Status(CE_E_INVALID_ARGUMENT);
        const std::string text(reason);
        return Status(ce_recording_stop(raw(), text.c_str(), request));
    }
    Status toggleRecording(Mode mode, std::string_view reason, ce_request_t* request = nullptr) {
        return recording(ce_recording_toggle, mode, reason, request);
    }
    Status toggleOverlay(ce_request_t* request = nullptr) noexcept {
        return Status(ce_overlay_toggle(raw(), request));
    }
    Status toggleBenchmark(ce_request_t* request = nullptr) noexcept {
        return Status(ce_benchmark_toggle(raw(), request));
    }
    Status takeScreenshot(ce_request_t* request = nullptr) noexcept {
        return Status(ce_screenshot_take(raw(), request));
    }
    Status launch(std::string_view commandLine, ce_request_t* request = nullptr) {
        if (detail::HasNull(commandLine))
            return Status(CE_E_INVALID_ARGUMENT);
        const std::string text(commandLine);
        return Status(ce_target_launch(raw(), text.c_str(), request));
    }
    Status nextEvent(std::chrono::milliseconds budget, Event& out) {
        if (out)
            return Status(CE_E_INVALID_STATE);
        const auto timeout = detail::Timeout(budget);
        if (!timeout)
            return Status(CE_E_INVALID_ARGUMENT);
        const ce_event_t* value = nullptr;
        const Status result(ce_runtime_next_event(raw(), *timeout, &value));
        if (result) {
            out.owner_ = owner_;
            out.value_ = value;
        }
        return result;
    }
    void* eventHandle() const noexcept {
        return ce_runtime_event_handle(raw());
    }
    Status editSettings(SettingsEdit& out) {
        if (out)
            return Status(CE_E_INVALID_STATE);
        ce_settings_edit_t* edit = nullptr;
        const Status result(ce_settings_begin(raw(), &edit));
        if (result) {
            out.owner_ = owner_;
            out.edit_ = edit;
        }
        return result;
    }
    Status setting(std::string_view section, std::string_view key, std::string& out) const {
        if (detail::HasNull(section) || detail::HasNull(key))
            return Status(CE_E_INVALID_ARGUMENT);
        const std::string sectionText(section), keyText(key);
        size_t needed = 0;
        Status result(ce_settings_get(raw(), sectionText.c_str(), keyText.c_str(), nullptr, 0, &needed));
        if (!result && result.code() != CE_E_BUFFER_TOO_SMALL)
            return result;
        if (!needed)
            return Status(CE_E_INTERNAL);
        std::string value(needed, '\0');
        result =
            Status(ce_settings_get(raw(), sectionText.c_str(), keyText.c_str(), value.data(), value.size(), &needed));
        if (result) {
            const auto end = value.find('\0');
            if (end == std::string::npos)
                return Status(CE_E_INTERNAL);
            value.resize(end);
            out = std::move(value);
        }
        return result;
    }
    Status log(LogLevel level, std::string_view message) {
        if (detail::HasNull(message))
            return Status(CE_E_INVALID_ARGUMENT);
        const std::string text(message);
        return Status(ce_runtime_log(raw(), static_cast<int32_t>(level), text.c_str()));
    }

private:
    using RecordingCommand = ce_status_t (*)(ce_runtime_t*, int32_t, const char*, ce_request_t*);
    Status recording(RecordingCommand command, Mode mode, std::string_view reason, ce_request_t* request) {
        if (detail::HasNull(reason))
            return Status(CE_E_INVALID_ARGUMENT);
        const std::string text(reason);
        return Status(command(raw(), static_cast<int32_t>(mode), text.c_str(), request));
    }
    std::shared_ptr<detail::RuntimeOwner> owner_;
};
}  // namespace cengine
