#include "config_load_internal.h"

void LoadWhitelist(ConfigReader& reader, AppConfig& config, const std::string& path, bool& pseudoProcessListSet) {
    // Whitelist
    config.gameWhitelist.clear();
    config.overlayWhitelist.clear();
    config.wgcWindowTitles.clear();
    config.profileWgcTargets.clear();
    config.profileDxgiDupTargets.clear();
    // We use a manual pass to support both comma-separated (legacy) and
    // newline-separated entries
    std::string cfgText;
    if (ReadTextFile(path, cfgText)) {
        std::stringstream cfgFile(cfgText);
        std::string line;
        bool inInjection = false;
        bool inWhitelist = false;
        bool inOverlayWhitelist = false;
        bool inWgcWindowDetection = false;
        bool inPseudoOverlay = false;
        bool inPseudoProcessList = false;
        std::string pseudoProcessList;

        auto AddEntry = [&](const std::string& raw, std::vector<WhitelistEntry>& targetList) {
            WhitelistEntry entry = ParseEntry(raw);
            if (!entry.pattern.empty() || !entry.windowName.empty()) {
                // Check for duplicates
                if (std::find(targetList.begin(), targetList.end(), entry) == targetList.end()) {
                    targetList.push_back(entry);
                }
            }
        };

        while (std::getline(cfgFile, line)) {
            // trim whitespace only for section check
            std::string trimmed = line;
            trimmed.erase(0, trimmed.find_first_not_of(" \t\r\n"));
            trimmed.erase(trimmed.find_last_not_of(" \t\r\n") + 1);

            if (trimmed.empty()) {
                if (inWhitelist)
                    inWhitelist = false;  // End of whitelist block on empty line
                continue;
            }

            if (trimmed[0] == ';')
                continue;

            if (trimmed[0] == '[') {
                std::string sectionName = trimmed;
                std::transform(sectionName.begin(), sectionName.end(), sectionName.begin(),
                               [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
                inInjection = (sectionName == "[injection]");
                inPseudoOverlay = (sectionName == "[desktopoverlay]" || sectionName == "[pseudo-overlay]");
                inWhitelist = false;
                inOverlayWhitelist = false;
                inWgcWindowDetection = false;
                inPseudoProcessList = false;
                continue;
            }

            // This list is parsed manually so its parenthesized multi-line form works.
            if (trimmed.find("wgc-window-detection=") == 0 || trimmed.find("wgc_window_detection=") == 0) {
                size_t eqPos = trimmed.find('=');
                std::string rest = trimmed.substr(eqPos + 1);
                rest = Trim(rest);
                if (!rest.empty() && rest != "(" && rest != ")") {
                    std::stringstream ss(rest);
                    std::string item;
                    while (std::getline(ss, item, ',')) {
                        AddEntry(item, config.wgcWindowTitles);
                    }
                }
                inWhitelist = false;
                inOverlayWhitelist = false;
                inWgcWindowDetection = true;
            } else if (inWgcWindowDetection) {
                if (trimmed.find('=') != std::string::npos || trimmed == ")") {
                    inWgcWindowDetection = false;
                } else if (trimmed != "(") {
                    AddEntry(trimmed, config.wgcWindowTitles);
                }
            }

            // Desktop overlay process_list supports a multi-line parenthesized format.
            if (inPseudoOverlay && trimmed.find("process_list=") == 0) {
                std::string rest = trimmed.substr(trimmed.find('=') + 1);
                rest = Trim(rest, " \t\r\n\"");
                if (rest == "(") {
                    inPseudoProcessList = true;
                    pseudoProcessList.clear();
                    pseudoProcessListSet = true;
                } else if (!rest.empty() && rest != ")") {
                    config.pseudoOverlay.processList = NormalizePseudoOverlayProcessList(rest);
                    pseudoProcessListSet = true;
                }
            } else if (inPseudoProcessList) {
                if (trimmed == ")" || trimmed.empty()) {
                    inPseudoProcessList = false;
                    if (!pseudoProcessList.empty()) {
                        config.pseudoOverlay.processList = NormalizePseudoOverlayProcessList(pseudoProcessList);
                    }

                } else if (trimmed.find('=') != std::string::npos) {
                    inPseudoProcessList = false;
                } else if (trimmed != "(") {
                    if (!pseudoProcessList.empty())
                        pseudoProcessList += "|";
                    pseudoProcessList += trimmed;
                }
            }

            if (inInjection) {
                if (trimmed.find("whitelist=") == 0) {
                    std::string rest = trimmed.substr(10);
                    rest = Trim(rest);
                    if (!rest.empty() && rest != "(" && rest != ")") {
                        // Parse comma-separated
                        std::stringstream ss(rest);
                        std::string item;
                        while (std::getline(ss, item, ',')) {
                            AddEntry(item, config.gameWhitelist);
                        }
                    }
                    inWhitelist = true;
                    inOverlayWhitelist = false;
                } else if (trimmed.find("overlay_whitelist=") == 0 || trimmed.find("overlay-whitelist=") == 0) {
                    size_t eqPos = trimmed.find('=');
                    std::string rest = trimmed.substr(eqPos + 1);
                    rest = Trim(rest);
                    if (!rest.empty() && rest != "(" && rest != ")") {
                        std::stringstream ss(rest);
                        std::string item;
                        while (std::getline(ss, item, ',')) {
                            AddEntry(item, config.overlayWhitelist);
                        }
                    }
                    inWhitelist = false;
                    inOverlayWhitelist = true;
                    inWgcWindowDetection = false;
                } else if (inWhitelist) {
                    if (trimmed.find('=') != std::string::npos || trimmed == ")") {
                        inWhitelist = false;
                    } else if (trimmed != "(") {
                        AddEntry(trimmed, config.gameWhitelist);
                    }
                } else if (inOverlayWhitelist) {
                    if (trimmed.find('=') != std::string::npos || trimmed == ")") {
                        inOverlayWhitelist = false;
                    } else if (trimmed != "(") {
                        AddEntry(trimmed, config.overlayWhitelist);
                    }
                }
            }
        }
    }

    // Canonical profiles win over an old list entry for the same process or
    // window, including an explicit "none" route. This makes the old lists
    // compatibility inputs rather than a second, conflicting policy layer.
    for (const ApplicationProfile& profile : config.applicationProfiles) {
        if (profile.legacy)
            continue;
        RemoveOverlappingLegacyEntry(profile.target, config.gameWhitelist);
        RemoveOverlappingLegacyEntry(profile.target, config.overlayWhitelist);
        RemoveOverlappingLegacyEntry(profile.target, config.wgcWindowTitles);
    }

    for (const ApplicationProfile& profile : config.applicationProfiles) {
        WhitelistEntry injectionTarget = profile.target;
        if (!profile.legacy)
            injectionTarget.mode = MatchMode::kExact;
        if (profile.injectionMode == ApplicationInjectionMode::kCapture) {
            AddUniqueEntry(injectionTarget, config.gameWhitelist);
        } else if (profile.injectionMode == ApplicationInjectionMode::kOverlay) {
            AddUniqueEntry(injectionTarget, config.overlayWhitelist);
        }

        if (profile.videoCaptureExplicit && profile.resolvedVideoCapture == ApplicationVideoCapture::kWgc) {
            AddUniqueEntry(profile.target, config.profileWgcTargets);
            AddUniqueEntry(profile.target, config.wgcWindowTitles);
        } else if (profile.videoCaptureExplicit &&
                   profile.resolvedVideoCapture == ApplicationVideoCapture::kDxgiDup) {
            AddUniqueEntry(profile.target, config.profileDxgiDupTargets);
        }
    }
}
