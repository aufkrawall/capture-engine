#include "runtime_configuration.h"
#include "configuration_state.h"
#include "common/config/config_text_encoding.h"
#include "common/logging/log_meter.h"
#include "common/logging/logging.h"

#include <windows.h>
#include <stdexcept>

namespace ce::runtime {
namespace {
class NativeConfigurationFiles final : public detail::ConfigurationFiles {
public:
    NativeConfigurationFiles(std::string path, uint32_t (*nowMs)() noexcept) : path_(std::move(path)), nowMs_(nowMs) {}
    uint32_t NowMs() const noexcept override {
        return nowMs_ ? nowMs_() : GetTickCount();
    }
    ce::config_reload::FileIdentity Identity() const override {
        WIN32_FILE_ATTRIBUTE_DATA info{};
        ce::config_reload::FileIdentity identity;
        identity.exists = GetFileAttributesExA(path_.c_str(), GetFileExInfoStandard, &info) != FALSE;
        if (identity.exists) {
            identity.lastWriteTime =
                (static_cast<uint64_t>(info.ftLastWriteTime.dwHighDateTime) << 32) | info.ftLastWriteTime.dwLowDateTime;
            identity.size = (static_cast<uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
        }
        return identity;
    }
    detail::ConfigurationState Initial() {
        // Remember the identity around startup loading, not the first later poll.
        // A file changed during startup must still be eligible for replacement.
        const auto identity = Identity();
        AppConfig initial;
        LoadConfig(path_, initial);
        return detail::ConfigurationState(std::move(initial), identity, NowMs());
    }
    ce::config_reload::LoadEvidence Load(AppConfig& candidate, ce::config_reload::FileIdentity observed) override {
        ce::config_reload::LoadEvidence evidence;
        evidence.identityBeforeLoad = observed;
        evidence.fileReadBeforeLoad = ce::config_text::PrimeConfigDocument(path_);
        const uint64_t failures = ce::config_text::ConfigReadFailureCount();
        if (evidence.fileReadBeforeLoad)
            LoadConfig(path_, candidate);
        evidence.readFailuresDuringLoad = ce::config_text::ConfigReadFailureCount() - failures;
        evidence.identityAfterLoad = Identity();
        return evidence;
    }
    void Pending(ce::config_reload::FileIdentity identity) override {
        const auto verdict =
            pending_.Observe(ce::log_meter::FieldKey(identity.exists, identity.lastWriteTime, identity.size));
        if (verdict)
            LogDebug("[RuntimeConfig] file change pending (exists=%d size=%llu repeats=%llu)", identity.exists,
                     static_cast<unsigned long long>(identity.size),
                     static_cast<unsigned long long>(verdict.suppressed));
    }
    void Deferred(const ce::config_reload::LoadEvidence& evidence) override {
        const bool changed = evidence.identityBeforeLoad != evidence.identityAfterLoad;
        const auto verdict = deferred_.Observe(
            ce::log_meter::FieldKey(evidence.fileReadBeforeLoad, evidence.readFailuresDuringLoad, changed,
                                    evidence.identityBeforeLoad.size, evidence.identityAfterLoad.size));
        if (verdict)
            LogWarn(
                "[RuntimeConfig] keeping settings; reload deferred (readable=%d readFailures=%llu "
                "changedDuringLoad=%d size=%llu->%llu repeats=%llu)",
                evidence.fileReadBeforeLoad, static_cast<unsigned long long>(evidence.readFailuresDuringLoad), changed,
                static_cast<unsigned long long>(evidence.identityBeforeLoad.size),
                static_cast<unsigned long long>(evidence.identityAfterLoad.size),
                static_cast<unsigned long long>(verdict.suppressed));
    }

private:
    std::string path_;
    uint32_t (*nowMs_)() noexcept;
    ce::log_meter::ChangeGate pending_;
    ce::log_meter::ChangeGate deferred_;
};

class NativeConfiguration {
public:
    NativeConfiguration(std::string path, uint32_t (*nowMs)() noexcept)
        : files_(std::move(path), nowMs),
          state_(files_.Initial()) {}
    const AppConfig& Current() const {
        return state_.Current();
    }
    std::optional<AppConfig> Poll() {
        return state_.Poll(files_);
    }
    uint32_t WaitMs() const {
        return state_.WaitMs(files_.NowMs());
    }
    void SetProcessLogPath(std::string path) {
        state_.SetProcessLogPath(std::move(path));
    }

private:
    NativeConfigurationFiles files_;
    detail::ConfigurationState state_;
};
NativeConfiguration* activeConfiguration = nullptr;
bool creatingConfiguration = false;
NativeConfiguration& Active() {
    if (!activeConfiguration)
        throw std::logic_error("runtime configuration requires a ready owning session");
    return *activeConfiguration;
}
}  // namespace

class RuntimeConfigurationSession::Impl : public NativeConfiguration {
public:
    using NativeConfiguration::NativeConfiguration;
};
RuntimeConfigurationSession::RuntimeConfigurationSession(std::string path, uint32_t (*nowMs)() noexcept) {
    if (activeConfiguration || creatingConfiguration) {
        OutputDebugStringA("[RuntimeConfig] refusing a second settings owner\n");
        return;
    }
    struct Creating {
        Creating() {
            creatingConfiguration = true;
        }
        ~Creating() {
            creatingConfiguration = false;
        }
    } creating;
    impl_ = std::make_unique<Impl>(std::move(path), nowMs);
    activeConfiguration = impl_.get();
}
RuntimeConfigurationSession::~RuntimeConfigurationSession() {
    if (impl_)
        activeConfiguration = nullptr;
}
bool RuntimeConfigurationSession::IsReady() const {
    return impl_ != nullptr;
}
const AppConfig& RuntimeConfiguration() {
    return Active().Current();
}
void SetRuntimeProcessLogPath(std::string path) {
    Active().SetProcessLogPath(std::move(path));
}
std::optional<AppConfig> PollRuntimeConfiguration() {
    return Active().Poll();
}
uint32_t RuntimeConfigurationWaitMs() {
    return Active().WaitMs();
}
}  // namespace ce::runtime
