#include "media_main_internal.h"
#include "profile_window_policy.h"

static BOOL CALLBACK EnumWindowsCallback(HWND hwnd, LPARAM lParam) {
    WindowSearch* search = (WindowSearch*)lParam;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == search->pid) {
        // Look for the main visible window
        // Checks: Visible, not child
        if (IsWindowVisible(hwnd) && GetWindow(hwnd, GW_OWNER) == 0) {
            // Check styles to avoid tool windows
            LONG_PTR styles = GetWindowLongPtr(hwnd, GWL_EXSTYLE);
            if (!(styles & WS_EX_TOOLWINDOW)) {
                search->hwnd = hwnd;
                return FALSE;  // Found, stop
            }
        }
    }
    return TRUE;
}

HWND GetMainWindowForProcess(DWORD pid) {
    WindowSearch search = {pid, NULL};
    EnumWindows(EnumWindowsCallback, (LPARAM)&search);
    return search.hwnd;
}

std::string ToLowerAscii(std::string value) {
    return ce::utf8::AsciiLowerCopy(value);
}

bool IsIgnoredForegroundWgcClass(HWND hwnd) {
    char className[128] = {};
    if (GetClassNameA(hwnd, className, static_cast<int>(sizeof(className))) <= 0) {
        return false;
    }

    const std::string lowerClass = ToLowerAscii(className);
    return lowerClass == "progman" || lowerClass == "workerw" || lowerClass == "shell_traywnd";
}

bool IsIgnoredForegroundWgcProcess(const std::string& processName) {
    const std::string lowerName = ToLowerAscii(processName);
    return lowerName.empty() || lowerName == "unknown" || lowerName == "explorer.exe" ||
           lowerName == "applicationframehost.exe" || lowerName == "shellexperiencehost.exe" ||
           lowerName == "searchhost.exe" || lowerName == "startmenuexperiencehost.exe" ||
           lowerName == "textinputhost.exe" || lowerName == "captureengine.exe";
}

ForegroundWgcWindowCandidate GetForegroundWgcWindowCandidate() {
    ForegroundWgcWindowCandidate candidate;
    HWND hwnd = GetForegroundWindow();
    if (!hwnd) {
        return candidate;
    }

    HWND root = GetAncestor(hwnd, GA_ROOT);
    if (root) {
        hwnd = root;
    }

    if (!IsWindow(hwnd) || !IsWindowVisible(hwnd) || IsIconic(hwnd) || hwnd == GetDesktopWindow() ||
        GetWindow(hwnd, GW_OWNER) != 0) {
        return candidate;
    }

    const LONG_PTR style = GetWindowLongPtr(hwnd, GWL_STYLE);
    const LONG_PTR exStyle = GetWindowLongPtr(hwnd, GWL_EXSTYLE);
    if ((style & WS_CHILD) || (exStyle & WS_EX_TOOLWINDOW) || IsIgnoredForegroundWgcClass(hwnd)) {
        return candidate;
    }

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == 0 || pid == GetCurrentProcessId()) {
        return candidate;
    }

    std::string processName = GetProcessNameFromPID(pid);
    if (IsIgnoredForegroundWgcProcess(processName)) {
        return candidate;
    }

    candidate.hwnd = hwnd;
    candidate.pid = pid;
    candidate.processName = processName;
    candidate.usable = true;
    candidate.fullscreenLike = IsWindowFullscreenLike(hwnd);
    return candidate;
}

bool MatchesProcessEntries(const std::vector<WhitelistEntry>& entries, const std::string& processName,
                           const std::string& processPath) {
    if (processName.empty()) {
        return false;
    }

    const ProcessTargetIdentity identity{processName, processPath};
    for (const auto& entry : entries) {
        if (MatchesProcessTarget(entry, identity)) {
            return true;
        }
    }

    return false;
}

const ApplicationProfile* FindApplicationProfileForProcess(const AppConfig& config, const std::string& processName,
                                                           const std::string& processPath) {
    if (processName.empty())
        return nullptr;

    return FindApplicationProfileForProcessIdentity(config.applicationProfiles,
                                                    ProcessTargetIdentity{processName, processPath});
}

const ApplicationProfile* FindApplicationProfileForTarget(const AppConfig& config,
                                                                 const WhitelistEntry& target) {
    auto found = std::find_if(config.applicationProfiles.begin(), config.applicationProfiles.end(),
                              [&](const ApplicationProfile& profile) { return profile.target == target; });
    return found == config.applicationProfiles.end() ? nullptr : &*found;
}

int64_t RectArea(const RECT& rect) {
    const int64_t width = std::max<LONG>(0, rect.right - rect.left);
    const int64_t height = std::max<LONG>(0, rect.bottom - rect.top);
    return width * height;
}

HWND FindMatchingWgcWindow(const std::vector<WhitelistEntry>& targets, int* selectedScore ,
                                  bool requireExactProcessNames , uint32_t* selectedPid ,
                                  std::string* selectedProcessName ,
                                  WhitelistEntry* selectedTarget, const AppConfig* profileConfig) {
    struct WgcSearchContext {
        const std::vector<WhitelistEntry>* targets;
        const AppConfig* profileConfig;
        HWND result;
        HWND foregroundRoot;
        int checked;
        int matched;
        int bestScore;
        bool requireExactProcessNames;
        uint32_t bestPid;
        std::string bestProcessName;
        WhitelistEntry bestTarget;
        bool hasBestTarget;
    };

    HWND foregroundRoot = GetForegroundWindow();
    if (foregroundRoot) {
        HWND root = GetAncestor(foregroundRoot, GA_ROOT);
        if (root) {
            foregroundRoot = root;
        }
    }

    WgcSearchContext ctx = {&targets, profileConfig, NULL, foregroundRoot, 0, 0, std::numeric_limits<int>::min(),
                            requireExactProcessNames, 0, {}, {}, false};
    EnumWindows(
        [](HWND hwnd, LPARAM lParam) -> BOOL {
            WgcSearchContext* context = (WgcSearchContext*)lParam;
            if (!IsWindowVisible(hwnd) || IsIconic(hwnd)) {
                return TRUE;
            }
            if (GetWindow(hwnd, GW_OWNER) != 0) {
                return TRUE;
            }
            const LONG_PTR style = GetWindowLongPtr(hwnd, GWL_STYLE);
            const LONG_PTR exStyle = GetWindowLongPtr(hwnd, GWL_EXSTYLE);
            if ((style & WS_CHILD) || (exStyle & WS_EX_TOOLWINDOW)) {
                return TRUE;
            }

            context->checked++;

            char title[256];
            GetWindowTextA(hwnd, title, sizeof(title));
            const std::string titleStr = ToLowerAscii(title);

            char className[256];
            GetClassNameA(hwnd, className, sizeof(className));
            const std::string classStr = ToLowerAscii(className);

            DWORD pid = 0;
            GetWindowThreadProcessId(hwnd, &pid);
            std::string procName;
            std::string procPath;
            if (pid != 0) {
                const ce::process::ProcessIdentityResult identity = GetProcessIdentityFromPID(pid);
                procName = identity.imageName;
                procPath = identity.imagePath;
            }

            const WhitelistEntry* bestEntry = nullptr;
            bool bestMatchedByTitleOrClass = false;
            bool bestMatchedByProcess = false;
            int bestTargetSpecificity = -1;
            for (const auto& entry : *context->targets) {
                if (context->profileConfig &&
                    !ce::profile_window::IsScreenCaptureTarget(ce::profile_window::SelectedProfile(
                        context->profileConfig->applicationProfiles, entry, ProcessTargetIdentity{procName, procPath})))
                    continue;
                MatchMode mode = entry.mode;
                bool matched = false;
                bool matchedByTitleOrClass = false;
                bool matchedByProcess = false;

                if (entry.HasWindow()) {
                    const std::string winLower = ce::utf8::AsciiLowerCopy(entry.windowName);

                    if (mode == MatchMode::kExact) {
                        matched = !titleStr.empty() && titleStr == winLower;
                    } else {
                        matched = !titleStr.empty() && titleStr.find(winLower) != std::string::npos;
                        if (!matched && mode == MatchMode::kTitleType && !classStr.empty()) {
                            matched = classStr.find(winLower) != std::string::npos;
                        }
                    }
                    matchedByTitleOrClass = matched;
                }

                if (!matched && MatchesProcessTarget(entry, ProcessTargetIdentity{procName, procPath},
                                                     context->requireExactProcessNames)) {
                    matched = true;
                    matchedByProcess = true;
                }

                if (matched) {
                    const int specificity =
                        entry.HasProcess()
                            ? ce::config::ProcessSelectorSpecificity(std::wstring_view(ce::utf8::ToWide(entry.pattern)))
                            : 0;
                    if (!bestEntry || specificity >= bestTargetSpecificity) {
                        bestEntry = &entry;
                        bestTargetSpecificity = specificity;
                        bestMatchedByTitleOrClass = matchedByTitleOrClass;
                        bestMatchedByProcess = matchedByProcess;
                    }
                }
            }

            if (bestEntry) {
                RECT windowRect = {};
                RECT clientRect = {};
                const bool haveWindowRect = GetWindowRect(hwnd, &windowRect) != FALSE;
                const bool haveClientRect = GetWindowClientRectInScreen(hwnd, clientRect);
                const int64_t area =
                    std::max(haveWindowRect ? RectArea(windowRect) : 0, haveClientRect ? RectArea(clientRect) : 0);
                int score = 1000;
                if (context->foregroundRoot && hwnd == context->foregroundRoot) {
                    score += 100000;
                }
                if (IsWindowFullscreenLike(hwnd)) {
                    score += 50000;
                }
                if (bestMatchedByTitleOrClass) {
                    score += 5000;
                }
                if (bestMatchedByProcess) {
                    score += 2000;
                }
                score += static_cast<int>(std::min<int64_t>(area / 1000, 40000));

                ++context->matched;
                if (!context->result || score > context->bestScore) {
                    context->result = hwnd;
                    context->bestScore = score;
                    context->bestPid = pid;
                    context->bestProcessName = procName;
                    context->bestTarget = *bestEntry;
                    context->hasBestTarget = true;
                }
            }
            return TRUE;
        },
        (LPARAM)&ctx);

    if (ctx.result) {
        DWORD pid = 0;
        GetWindowThreadProcessId(ctx.result, &pid);
        LogDebug(
            "[Media] WGC window detection selected hwnd=0x%p pid=%lu fullscreenLike=%d score=%d "
            "(matched=%d checked=%d foreground=%d)",
            ctx.result, static_cast<unsigned long>(pid), IsWindowFullscreenLike(ctx.result) ? 1 : 0, ctx.bestScore,
            ctx.matched, ctx.checked, (ctx.foregroundRoot && ctx.result == ctx.foregroundRoot) ? 1 : 0);
    }

    if (selectedScore)
        *selectedScore = ctx.result ? ctx.bestScore : std::numeric_limits<int>::min();
    if (selectedPid)
        *selectedPid = ctx.result ? ctx.bestPid : 0;
    if (selectedProcessName)
        *selectedProcessName = ctx.result ? ctx.bestProcessName : std::string{};
    if (selectedTarget)
        *selectedTarget = ctx.hasBestTarget ? ctx.bestTarget : WhitelistEntry{};

    return ctx.result;
}
