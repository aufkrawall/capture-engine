#pragma once

#include <windows.h>
#include <memory>
#include <vector>

#include "common/config/config.h"
#include "common/ipc/shared_defs.h"
#include "runtime/desktop/pseudo_overlay.h"

namespace ce::runtime {

// Process-backed application profiles of `config`, in profile order, each with its resolved
// DesktopOverlay settings. Window-only profiles are not included.
std::vector<PseudoOverlayApplicationConfig> ResolveDesktopOverlayProfiles(const AppConfig& config);

// Whether an overlay must exist: the global DesktopOverlay or any resolved profile enables it.
bool DesktopOverlayRequired(const AppConfig& config, const std::vector<PseudoOverlayApplicationConfig>& profiles);

// Owns the desktop status overlay (PseudoOverlay, self-threaded) for the controller process.
// The overlay is created on first need and then only reconfigured; Stop() is the only teardown.
class DesktopOverlay {
public:
    // `windowModule` is the module whose HINSTANCE registers the overlay window classes.
    explicit DesktopOverlay(HINSTANCE windowModule);
    ~DesktopOverlay();
    DesktopOverlay(const DesktopOverlay&) = delete;
    DesktopOverlay& operator=(const DesktopOverlay&) = delete;

    // Creates the overlay when the global or any process-backed profile setting enables it, otherwise
    // only pushes the config into an existing overlay. Returns false only when creation was needed
    // and the overlay failed to initialize.
    bool Apply(const AppConfig& config, const char* reason);

    // Recording start intent published by the recording owner. Remembered even without an overlay and
    // applied when the overlay is created, so a late overlay shows the intent that is already active.
    void OnStartIntent(RecordingStartIntent intent);
    void OnFinalizing();
    void BeginCapture();
    // Ends a screenshot capture and shows its result notification.
    void EndCapture(bool saved);

    // Shuts the overlay down and releases it. Idempotent. The remembered start intent is kept: it is
    // the recording owner's state, not the overlay's, so a later Apply() reflects it too.
    void Stop();
    bool Active() const;

#ifdef CE_UNIT_TESTS
    PseudoOverlay* OverlayForTesting() const {
        return overlay_.get();
    }
#endif

private:
    HINSTANCE windowModule_ = nullptr;
    std::unique_ptr<PseudoOverlay> overlay_;
    RecordingStartIntent startIntent_ = RecordingStartIntent::Idle;
};

}  // namespace ce::runtime
