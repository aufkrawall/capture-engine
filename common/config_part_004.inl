            }
            LogInfo("Config: [%s] app-audio source process='%s' processId=%lu tracks=[%s]", section,
                    appAudio.processName.empty() ? "-" : appAudio.processName.c_str(),
                    (unsigned long)appAudio.processId, trackList.c_str());
            config.audioSources.push_back(appAudio);
        }
    }

    // Desktop overlay (for WGC capture, no injection)
    config.pseudoOverlay.enabled = GetBoolCompat("DesktopOverlay", "enabled", "pseudo-overlay", "enabled", false);
    config.pseudoOverlay.size =
        GetBoundedIntCompat("DesktopOverlay", "size", "pseudo-overlay", "size", 30, 10, 200);
    config.pseudoOverlay.pad =
        GetBoundedIntCompat("DesktopOverlay", "pad", "pseudo-overlay", "pad", 20, 0, 100);
    config.pseudoOverlay.pos = GetBoundedIntCompat("DesktopOverlay", "pos", "pseudo-overlay", "pos", 0, 0, 3);
    config.pseudoOverlay.mode =
        GetBoundedIntCompat("DesktopOverlay", "mode", "pseudo-overlay", "mode", 0, 0, 2);
    config.pseudoOverlay.alwaysRender =
        GetBoolCompat("DesktopOverlay", "always_render", "pseudo-overlay", "always_render", false);
    config.pseudoOverlay.alwaysRenderOnlyWhenGame = GetBoolCompat(
        "DesktopOverlay", "always_render_only_when_game", "pseudo-overlay", "always_render_only_when_game", false);
    config.pseudoOverlay.showEncoderOverloadWarn =
        GetBoolCompat("DesktopOverlay", "show_encoder_overload_warnings", "pseudo-overlay",
                      "show_encoder_overload_warnings", true);
    config.pseudoOverlay.foregroundAcquireGraceMs = GetBoundedIntCompat(
        "DesktopOverlay", "foreground_acquire_grace_ms", "pseudo-overlay", "foreground_acquire_grace_ms", 2000, 0,
        10000);
    {
        if (!pseudoProcessListSet) {
            std::string procList =
                GetStrCompat("DesktopOverlay", "process_list", "pseudo-overlay", "process_list", "");
            if (procList.size() > 2048)
                procList.resize(2048);
            config.pseudoOverlay.processList = NormalizePseudoOverlayProcessList(procList);
        } else if (config.pseudoOverlay.processList.size() > 2048) {
            config.pseudoOverlay.processList.resize(2048);
        }
    }

    // Hotkeys
    // Parse hotkey strings like "F9", "Ctrl+Shift+F10", "Alt+Ctrl+R"
    std::string startStopKey = GetStr("Hotkeys", "start_stop", "F9");
    config.hotkeyStartStop = ParseHotkey(startStopKey);

    // Ensure we have at least one hotkey - fallback to F9 if parsing failed
    if (config.hotkeyStartStop.vkey == 0) {
        config.hotkeyStartStop.vkey = VK_F9;
    }

    std::string toggleFpsKey = GetStr("Hotkeys", "toggle_fps", "");
    if (!toggleFpsKey.empty()) {
        config.hotkeyToggleFPS = ParseHotkey(toggleFpsKey);
    }

    std::string screenshotKey = GetStr("Hotkeys", "screenshot", "");
    if (!screenshotKey.empty()) {
        config.hotkeyScreenshot = ParseHotkey(screenshotKey);
    }

    std::string audioOnlyKey = GetStr("Hotkeys", "audio_only", "");
    if (!audioOnlyKey.empty()) {
        config.hotkeyAudioOnly = ParseHotkey(audioOnlyKey);
    }

    config.screenshotDir = GetStrCompat("Output", "screenshot_dir", "Screenshot", "screenshot_dir", "");
    config.screenshotColorSpace = Lowercase(Trim(GetStr("Screenshot", "color_space", "auto")));
    if (config.screenshotColorSpace != "auto" && config.screenshotColorSpace != "bt709") {
        LogInvalidConfigBoundary("Screenshot", "color_space", config.screenshotColorSpace, "auto");
        config.screenshotColorSpace = "auto";
    }
}

// Parse hotkey string (e.g., "Ctrl+Shift+F9", "Alt+R", "F10")
AppConfig::HotkeyConfig ParseHotkey(const std::string& val) {
    AppConfig::HotkeyConfig hk;
    if (val.empty())
        return hk;

    std::string upper = val;
    std::transform(upper.begin(), upper.end(), upper.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });

    // Check for modifiers
    if (upper.find("CTRL+") != std::string::npos || upper.find("CONTROL+") != std::string::npos) {
        hk.ctrl = true;
    }
    if (upper.find("SHIFT+") != std::string::npos) {
        hk.shift = true;
    }
    if (upper.find("ALT+") != std::string::npos) {
        hk.alt = true;
    }
    if (upper.find("WIN+") != std::string::npos || upper.find("WINDOWS+") != std::string::npos) {
        hk.win = true;
    }

    // Extract the key part (after last +)
    size_t lastPlus = upper.rfind('+');
    std::string key = (lastPlus != std::string::npos) ? upper.substr(lastPlus + 1) : upper;

    // Parse function keys F1-F24
    if (key.length() >= 2 && key[0] == 'F') {
        int fnum = 0;
        if (TryParseInt(key.substr(1), fnum) && fnum >= 1 && fnum <= 24) {
            hk.vkey = VK_F1 + (fnum - 1);
        }
    }
    // Parse number keys 0-9
    else if (key.length() == 1 && key[0] >= '0' && key[0] <= '9') {
        hk.vkey = key[0];  // '0'-'9' match their VK codes
    }
    // Parse letter keys A-Z
    else if (key.length() == 1 && key[0] >= 'A' && key[0] <= 'Z') {
        hk.vkey = key[0];  // 'A'-'Z' match their VK codes
    }
    // Named keys
    else if (key == "SPACE" || key == "SPACEBAR") {
        hk.vkey = VK_SPACE;
    } else if (key == "ENTER" || key == "RETURN") {
        hk.vkey = VK_RETURN;
    } else if (key == "ESC" || key == "ESCAPE") {
        hk.vkey = VK_ESCAPE;
    } else if (key == "TAB") {
        hk.vkey = VK_TAB;
    } else if (key == "BACKSPACE" || key == "BACK") {
        hk.vkey = VK_BACK;
    } else if (key == "DELETE" || key == "DEL") {
        hk.vkey = VK_DELETE;
    } else if (key == "INSERT" || key == "INS") {
        hk.vkey = VK_INSERT;
    } else if (key == "HOME") {
        hk.vkey = VK_HOME;
    } else if (key == "END") {
        hk.vkey = VK_END;
    } else if (key == "PAGEUP" || key == "PGUP") {
        hk.vkey = VK_PRIOR;
    } else if (key == "PAGEDOWN" || key == "PGDN") {
        hk.vkey = VK_NEXT;
    } else if (key == "UP") {
        hk.vkey = VK_UP;
    } else if (key == "DOWN") {
        hk.vkey = VK_DOWN;
    } else if (key == "LEFT") {
        hk.vkey = VK_LEFT;
    } else if (key == "RIGHT") {
        hk.vkey = VK_RIGHT;
    } else if (key == "PRINTSCREEN" || key == "PRTSC") {
        hk.vkey = VK_SNAPSHOT;
    } else if (key == "SCROLLLOCK" || key == "SCRLOCK") {
        hk.vkey = VK_SCROLL;
    } else if (key == "PAUSE" || key == "BREAK") {
        hk.vkey = VK_PAUSE;
    } else if (key == "NUMPAD0" || key == "NUM0") {
        hk.vkey = VK_NUMPAD0;
    } else if (key == "NUMPAD1" || key == "NUM1") {
        hk.vkey = VK_NUMPAD1;
    } else if (key == "NUMPAD2" || key == "NUM2") {
        hk.vkey = VK_NUMPAD2;
    } else if (key == "NUMPAD3" || key == "NUM3") {
        hk.vkey = VK_NUMPAD3;
    } else if (key == "NUMPAD4" || key == "NUM4") {
        hk.vkey = VK_NUMPAD4;
    } else if (key == "NUMPAD5" || key == "NUM5") {
        hk.vkey = VK_NUMPAD5;
    } else if (key == "NUMPAD6" || key == "NUM6") {
        hk.vkey = VK_NUMPAD6;
    } else if (key == "NUMPAD7" || key == "NUM7") {
        hk.vkey = VK_NUMPAD7;
    } else if (key == "NUMPAD8" || key == "NUM8") {
        hk.vkey = VK_NUMPAD8;
    } else if (key == "NUMPAD9" || key == "NUM9") {
        hk.vkey = VK_NUMPAD9;
    } else if (key == "MULTIPLY" || key == "NUMMULT") {
        hk.vkey = VK_MULTIPLY;
    } else if (key == "ADD" || key == "NUMPLUS") {
        hk.vkey = VK_ADD;
    } else if (key == "SUBTRACT" || key == "NUMMINUS") {
        hk.vkey = VK_SUBTRACT;
    } else if (key == "MINUS" || key == "DASH" || key == "HYPHEN") {
        hk.vkey = VK_OEM_MINUS;   // physical - key (between 0 and =)
    } else if (key == "PLUS" || key == "EQUALS") {
        hk.vkey = VK_OEM_PLUS;    // physical = key (between - and Backspace)
    } else if (key == "DECIMAL" || key == "NUMDOT") {
        hk.vkey = VK_DECIMAL;
    } else if (key == "DIVIDE" || key == "NUMDIV") {
        hk.vkey = VK_DIVIDE;
    }

    return hk;
}
