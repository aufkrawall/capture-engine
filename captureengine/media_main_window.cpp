#include "media_main_internal.h"

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
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
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

bool MatchesProcessEntry(const WhitelistEntry& entry, const std::string& lowerProcessName) {
    return MatchesProcessName(entry, lowerProcessName);
}

bool MatchesProcessEntries(const std::vector<WhitelistEntry>& entries, const std::string& processName) {
    if (processName.empty()) {
        return false;
    }

    std::string lowerName = processName;
    std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });

    for (const auto& entry : entries) {
        if (MatchesProcessEntry(entry, lowerName)) {
            return true;
        }
    }

    return false;
}

const ApplicationProfile* FindApplicationProfileForProcess(const AppConfig& config,
                                                                  const std::string& processName) {
    if (processName.empty())
        return nullptr;

    std::string lowerName = processName;
    std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    for (const ApplicationProfile& profile : config.applicationProfiles) {
        if (!profile.target.HasProcess())
            continue;
        std::string lowerTarget = profile.target.pattern;
        std::transform(lowerTarget.begin(), lowerTarget.end(), lowerTarget.begin(),
                       [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        if ((!profile.legacy && lowerName == lowerTarget) ||
            (profile.legacy && MatchesProcessEntry(profile.target, lowerName)))
            return &profile;
    }
    return nullptr;
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
                                  WhitelistEntry* selectedTarget ) {
    struct WgcSearchContext {
        const std::vector<WhitelistEntry>* targets;
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

    WgcSearchContext ctx = {&targets, NULL, foregroundRoot, 0, 0, std::numeric_limits<int>::min(),
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
            std::string titleStr = title;
            std::transform(titleStr.begin(), titleStr.end(), titleStr.begin(),
                           [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });

            char className[256];
            GetClassNameA(hwnd, className, sizeof(className));
            std::string classStr = className;
            std::transform(classStr.begin(), classStr.end(), classStr.begin(),
                           [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });

            DWORD pid = 0;
            GetWindowThreadProcessId(hwnd, &pid);
            std::string procName;
            if (pid != 0) {
                procName = GetProcessNameFromPID(pid);
                std::transform(procName.begin(), procName.end(), procName.begin(),
                               [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
            }

            for (const auto& entry : *context->targets) {
                MatchMode mode = entry.mode;
                bool matched = false;
                bool matchedByTitleOrClass = false;
                bool matchedByProcess = false;

                if (entry.HasWindow()) {
                    std::string winLower = entry.windowName;
                    std::transform(winLower.begin(), winLower.end(), winLower.begin(),
                                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });

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

                if (!matched && MatchesProcessName(entry, procName, context->requireExactProcessNames)) {
                    matched = true;
                    matchedByProcess = true;
                }

                if (matched) {
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
                    if (matchedByTitleOrClass) {
                        score += 5000;
                    }
                    if (matchedByProcess) {
                        score += 2000;
                    }
                    score += static_cast<int>(std::min<int64_t>(area / 1000, 40000));

                    ++context->matched;
                    if (!context->result || score > context->bestScore) {
                        context->result = hwnd;
                        context->bestScore = score;
                        context->bestPid = pid;
                        context->bestProcessName = procName;
                        context->bestTarget = entry;
                        context->hasBestTarget = true;
                    }
                    break;
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
