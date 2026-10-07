#pragma once

#include "common/config/config.h"
#include "common/config/config_reload_policy.h"

#include <optional>
#include <type_traits>
#include <utility>

namespace ce::runtime::detail {
class ConfigurationFiles {
public:
    virtual ~ConfigurationFiles() = default;
    virtual uint32_t NowMs() const noexcept = 0;
    virtual ce::config_reload::FileIdentity Identity() const = 0;
    virtual ce::config_reload::LoadEvidence Load(AppConfig& candidate, ce::config_reload::FileIdentity observed) = 0;
    virtual void Pending(ce::config_reload::FileIdentity identity) = 0;
    virtual void Deferred(const ce::config_reload::LoadEvidence& evidence) = 0;
};

// Owns both the published settings and their committed file identity.
// Consumers cannot commit an identity or publish a partially read candidate.
class ConfigurationState {
public:
    ConfigurationState(AppConfig initial, ce::config_reload::FileIdentity loaded, uint32_t now)
        : current_(std::move(initial)),
          lastCheckMs_(now) {
        state_.initialized = true;
        state_.applied = loaded;
    }
    ConfigurationState(const ConfigurationState&) = delete;
    ConfigurationState& operator=(const ConfigurationState&) = delete;

    const AppConfig& Current() const {
        return current_;
    }

    // The old snapshot is returned only after a complete replacement. Frontend
    // effects compare it with Current(); they never decide whether a load commits.
    std::optional<AppConfig> Poll(ConfigurationFiles& files) {
        if (loading_ || WaitMs(files.NowMs()) != 0)
            return std::nullopt;
        struct Loading {
            explicit Loading(bool& value) : value_(value) {
                value_ = true;
            }
            ~Loading() {
                value_ = false;
            }
            bool& value_;
        } loading(loading_);
        struct Checked {
            Checked(uint32_t& last, ConfigurationFiles& files) : last_(last), files_(files) {}
            ~Checked() {
                last_ = files_.NowMs();
            }
            uint32_t& last_;
            ConfigurationFiles& files_;
        } checked(lastCheckMs_, files);

        const auto identity = files.Identity();
        const auto decision = ce::config_reload::Observe(state_, identity);
        if (decision != ce::config_reload::Decision::kReload) {
            if (decision == ce::config_reload::Decision::kWait)
                files.Pending(identity);
            return std::nullopt;
        }
        AppConfig candidate = current_;
        const auto evidence = files.Load(candidate, identity);
        if (evidence.identityBeforeLoad != identity || !ce::config_reload::IsCoherentLoad(evidence)) {
            ce::config_reload::DeferReload(state_);
            files.Deferred(evidence);
            return std::nullopt;
        }
        static_assert(std::is_nothrow_move_constructible_v<AppConfig> && std::is_nothrow_move_assignable_v<AppConfig>);
        std::optional<AppConfig> previous(std::move(current_));
        current_ = std::move(candidate);
        ce::config_reload::CommitReload(state_, identity);
        return previous;
    }

    uint32_t WaitMs(uint32_t now) const {
        const uint32_t interval = ce::config_reload::CheckIntervalMs(state_);
        const uint32_t elapsed = now - lastCheckMs_;
        return elapsed >= interval ? 0 : interval - elapsed;
    }

    // Executable logging is a host effect, not a programmatic settings producer.
    // Keep its destination across INI reloads without exposing mutable settings.
    void SetProcessLogPath(std::string path) {
        current_.logFilePath = std::move(path);
    }

private:
    AppConfig current_;
    ce::config_reload::State state_;
    uint32_t lastCheckMs_;
    bool loading_ = false;
};
}  // namespace ce::runtime::detail
