#include "config_load_internal.h"

void LoadDesktopOverlayAndHotkeys(ConfigReader& reader, AppConfig& config, bool pseudoProcessListSet) {
    // Desktop overlay (for WGC capture, no injection)
    config.pseudoOverlay.enabled = reader.GetBoolCompat("DesktopOverlay", "enabled", "pseudo-overlay", "enabled", false);
    config.pseudoOverlay.size =
        reader.GetBoundedIntCompat("DesktopOverlay", "size", "pseudo-overlay", "size", 30, 10, 200);
    config.pseudoOverlay.pad =
        reader.GetBoundedIntCompat("DesktopOverlay", "pad", "pseudo-overlay", "pad", 20, 0, 100);
    config.pseudoOverlay.pos = reader.GetBoundedIntCompat("DesktopOverlay", "pos", "pseudo-overlay", "pos", 0, 0, 3);
    config.pseudoOverlay.mode =
        reader.GetBoundedIntCompat("DesktopOverlay", "mode", "pseudo-overlay", "mode", 0, 0, 2);
    config.pseudoOverlay.alwaysRender =
        reader.GetBoolCompat("DesktopOverlay", "always_render", "pseudo-overlay", "always_render", false);
    config.pseudoOverlay.alwaysRenderOnlyWhenGame = reader.GetBoolCompat(
        "DesktopOverlay", "always_render_only_when_game", "pseudo-overlay", "always_render_only_when_game", false);
    config.pseudoOverlay.showEncoderOverloadWarn =
        reader.GetBoolCompat("DesktopOverlay", "show_encoder_overload_warnings", "pseudo-overlay",
                      "show_encoder_overload_warnings", true);
    config.pseudoOverlay.foregroundAcquireGraceMs = reader.GetBoundedIntCompat(
        "DesktopOverlay", "foreground_acquire_grace_ms", "pseudo-overlay", "foreground_acquire_grace_ms", 2000, 0,
        10000);
    {
        if (!pseudoProcessListSet) {
            std::string procList =
                reader.GetStrCompat("DesktopOverlay", "process_list", "pseudo-overlay", "process_list", "");
            if (procList.size() > 2048)
                procList.resize(2048);
            config.pseudoOverlay.processList = NormalizePseudoOverlayProcessList(procList);
        } else if (config.pseudoOverlay.processList.size() > 2048) {
            config.pseudoOverlay.processList.resize(2048);
        }
    }

    // Hotkeys
    // Parse hotkey strings like "F9", "Ctrl+Shift+F10", "Alt+Ctrl+R"
    std::string startStopKey = reader.GetStr("Hotkeys", "start_stop", "F9");
    config.hotkeyStartStop = ParseHotkey(startStopKey);

    // Ensure we have at least one hotkey - fallback to F9 if parsing failed
    if (config.hotkeyStartStop.vkey == 0) {
        config.hotkeyStartStop.vkey = VK_F9;
    }

    std::string toggleFpsKey = reader.GetStr("Hotkeys", "toggle_fps", "");
    if (!toggleFpsKey.empty()) {
        config.hotkeyToggleFPS = ParseHotkey(toggleFpsKey);
    }

    std::string screenshotKey = reader.GetStr("Hotkeys", "screenshot", "");
    if (!screenshotKey.empty()) {
        config.hotkeyScreenshot = ParseHotkey(screenshotKey);
    }

    std::string audioOnlyKey = reader.GetStr("Hotkeys", "audio_only", "");
    if (!audioOnlyKey.empty()) {
        config.hotkeyAudioOnly = ParseHotkey(audioOnlyKey);
    }

    config.screenshotDir = reader.GetStrCompat("Output", "screenshot_dir", "Screenshot", "screenshot_dir", "");
    config.screenshotColorSpace = Lowercase(Trim(reader.GetStr("Screenshot", "color_space", "auto")));
    if (config.screenshotColorSpace != "auto" && config.screenshotColorSpace != "bt709") {
        LogInvalidConfigBoundary("Screenshot", "color_space", config.screenshotColorSpace, "auto");
        config.screenshotColorSpace = "auto";
    }
}
