#include "target_launcher.h"

#include <memory>

#include "captureengine/injection/injection.h"

namespace ce::runtime {

namespace {

// TargetInjector over the role host's InjectionManager. The manager is created on the first Game
// launch (a plain launcher never needs it) and kept for the launcher's lifetime, so every target
// the launcher hooked stays tracked until the launcher is destroyed; its destructor then waits for
// in-flight injection threads and ejects the hook from each of them (see TargetLauncher).
class InjectionManagerInjector final : public TargetInjector {
public:
    explicit InjectionManagerInjector(const AppConfig& snapshot) : snapshot_(snapshot) {}

    bool InjectEarly(uint32_t processId, void* suspendedMainThread) override {
        return Manager().InjectEarly(processId, suspendedMainThread);
    }

    bool InjectResumed(uint32_t processId, const std::string& fileName) override {
        return Manager().Inject(processId, fileName);
    }

private:
    InjectionManager& Manager() {
        if (!manager_) {
            manager_ = std::make_unique<InjectionManager>(snapshot_);
        }
        return *manager_;
    }

    AppConfig snapshot_;
    std::unique_ptr<InjectionManager> manager_;
};

}  // namespace

TargetLauncher::TargetLauncher(const AppConfig& snapshot)
    : TargetLauncher(std::make_unique<InjectionManagerInjector>(snapshot)) {}

}  // namespace ce::runtime
