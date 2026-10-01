#pragma once

// Argument contract of the elevated role the Capture Engine installer runs to
// enable the elevation service and startup registration:
//
//   captureengine.exe --ce-installer-setup --owner-sid=<SID> --owner-admin=0|1
//                     --prefs=<0-7> --service=install|remove|keep
//                     --autostart=apply|keep
//
// --prefs uses the same encoding as the tray toggles (service=1, elevated=2,
// autostart=4). The role acts for --owner-sid because the installer may itself
// run as a different administrator account. Pure parsing lives here so it is
// unit tested; the process-level checks are in captureengine/startup_control.cpp.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "startup_policy.h"

namespace ce::startup {

enum class ServiceStep { Keep, Install, Remove };

struct InstallerSetupRequest {
    std::wstring ownerSid;
    bool ownerAdministrator = false;
    Preferences preferences;
    ServiceStep service = ServiceStep::Keep;
    bool applyAutostart = false;
};

inline constexpr wchar_t kInstallerSetupArgument[] = L"--ce-installer-setup";

// Structural only (S-1-<authority>-<subauthority>...); the operating system
// parses the SID for real before anything acts on it.
inline bool LooksLikeSidString(std::wstring_view text) {
    if (text.size() < 7 || text.size() > 184 || text.substr(0, 4) != L"S-1-")
        return false;
    bool digitSeen = false;
    for (size_t index = 4; index < text.size(); ++index) {
        const wchar_t character = text[index];
        if (character >= L'0' && character <= L'9') {
            digitSeen = true;
        } else if (character == L'-' && digitSeen && index + 1 < text.size()) {
            digitSeen = false;
        } else {
            return false;
        }
    }
    return digitSeen;
}

// `arguments` excludes the program name and the --ce-installer-setup marker.
// Every option must appear exactly once; unknown or repeated options are
// rejected so a malformed call can never be half applied.
inline bool ParseInstallerSetupArguments(const std::vector<std::wstring>& arguments, InstallerSetupRequest* request) {
    InstallerSetupRequest parsed;
    bool seenSid = false;
    bool seenAdmin = false;
    bool seenPrefs = false;
    bool seenService = false;
    bool seenAutostart = false;
    const auto value = [](std::wstring_view argument, std::wstring_view name, std::wstring_view* out) {
        if (argument.size() > name.size() && argument.substr(0, name.size()) == name) {
            *out = argument.substr(name.size());
            return true;
        }
        return false;
    };
    for (const std::wstring& argument : arguments) {
        std::wstring_view text;
        if (value(argument, L"--owner-sid=", &text)) {
            if (seenSid || !LooksLikeSidString(text))
                return false;
            seenSid = true;
            parsed.ownerSid.assign(text);
        } else if (value(argument, L"--owner-admin=", &text)) {
            if (seenAdmin || (text != L"0" && text != L"1"))
                return false;
            seenAdmin = true;
            parsed.ownerAdministrator = text == L"1";
        } else if (value(argument, L"--prefs=", &text)) {
            if (seenPrefs || text.size() != 1 || text[0] < L'0' || text[0] > L'7')
                return false;
            seenPrefs = true;
            const unsigned mask = static_cast<unsigned>(text[0] - L'0');
            parsed.preferences = {(mask & 1) != 0, (mask & 2) != 0, (mask & 4) != 0};
        } else if (value(argument, L"--service=", &text)) {
            if (seenService)
                return false;
            seenService = true;
            if (text == L"install")
                parsed.service = ServiceStep::Install;
            else if (text == L"remove")
                parsed.service = ServiceStep::Remove;
            else if (text == L"keep")
                parsed.service = ServiceStep::Keep;
            else
                return false;
        } else if (value(argument, L"--autostart=", &text)) {
            if (seenAutostart || (text != L"apply" && text != L"keep"))
                return false;
            seenAutostart = true;
            parsed.applyAutostart = text == L"apply";
        } else {
            return false;
        }
    }
    if (!(seenSid && seenAdmin && seenPrefs && seenService && seenAutostart))
        return false;
    // A service that is being installed is, by definition, a service preference.
    if (parsed.service == ServiceStep::Install && !parsed.preferences.service)
        return false;
    if (parsed.service == ServiceStep::Remove && parsed.preferences.service)
        return false;
    *request = std::move(parsed);
    return true;
}

}  // namespace ce::startup
