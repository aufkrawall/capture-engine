// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall
// The production assembly of a Runtime: real helper processes, the process's settings owner, the desktop
// overlay, the system hotkeys, desktop screenshots and the injecting launcher. Product image only; the
// core tests assemble the engine from fakes (runtime_internal.h).
#include "runtime/core/runtime_internal.h"

#include "runtime/desktop/desktop_overlay.h"
#include "runtime/desktop/global_hotkeys.h"
#include "runtime/screenshot/screenshot_capture.h"

namespace ce::engine {
namespace {
class DesktopOverlayPresence final : public DesktopPresence {
public:
    explicit DesktopOverlayPresence(HINSTANCE windowModule) : overlay_(windowModule) {}
    bool Apply(const AppConfig& config, const char* reason) override { return overlay_.Apply(config, reason); }
    void OnStartIntent(RecordingStartIntent intent) override { overlay_.OnStartIntent(intent); }
    void OnFinalizing() override { overlay_.OnFinalizing(); }
    void BeginCapture() override { overlay_.BeginCapture(); }
    void EndCapture(bool saved) override { overlay_.EndCapture(saved); }
    void Stop() override { overlay_.Stop(); }

private:
    ce::runtime::DesktopOverlay overlay_;
};
}  // namespace

std::unique_ptr<Runtime> Runtime::Create(const RuntimeOptions& options, ce_status_t* status) {
    RuntimeParts parts;
    parts.options = options;
    const std::string configPath = options.configPath;
    const std::wstring roleHost = options.roleHost;
    parts.makeChildren = [configPath, roleHost](void (*pump)(), bool (*accepting)()) {
        return std::unique_ptr<ce::runtime::HostChildren>(std::make_unique<ce::runtime::ChildSupervisor>(
            configPath.c_str(), pump, accepting, roleHost.empty() ? nullptr : roleHost.c_str()));
    };
    parts.settings = MakeRuntimeConfigurationSettings();
    const HINSTANCE windowModule = options.windowModule ? options.windowModule : GetModuleHandleW(nullptr);
    parts.makeDesktop = [windowModule]() {
        return std::unique_ptr<DesktopPresence>(std::make_unique<DesktopOverlayPresence>(windowModule));
    };
    parts.hotkeys = &SystemHotkeys();
    parts.capture = &ce::runtime::CaptureDesktopScreenshot;
    parts.makeLauncher = [](const AppConfig& snapshot) { return std::make_unique<ce::runtime::TargetLauncher>(snapshot); };
    parts.clock = SystemClock();
    parts.vulkanResidency = true;
    parts.heartbeat = true;
    return RuntimeFactory::Assemble(std::move(parts), status);
}

}  // namespace ce::engine
