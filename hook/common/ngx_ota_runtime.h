#pragma once

/*
 * Process-local application of the `ngx_ota` / `ngx_log` policy.
 *
 * The policy itself lives in ngx_ota_policy.h and is pure. This unit owns the
 * one piece of process state it needs - the mode the profile resolved to - and
 * the two side effects that act on it: the environment variables `_nvngx.dll`
 * reads, and the answer CE's CreateProcess hook gives when the NGX core tries
 * to launch `nvngx_update.exe`.
 *
 * The mode is published once the hook thread has a config. Until then the
 * accessors answer "default", which is exactly right: with no profile resolved
 * CE has no mandate to change what the driver would do, and the NGX core does
 * not reach its updater until well after that point.
 */

#include <cstdint>

namespace ce::ngx_ota {

// Publishes the resolved profile policy. Applies the environment variables as a
// side effect, because their only useful moment is as early as possible and the
// caller is the first code that knows the mode. Safe to call repeatedly; the
// environment is rewritten only when the resolved policy actually changes.
//
// `sessionLogDirectory` may be null or empty, in which case NGX keeps its own
// log location and only the level is stated.
void PublishPolicy(uint8_t otaMode, uint8_t logLevel, const char* sessionLogDirectory);

// The currently published OTA mode, or kNgxOtaModeDefault before a profile has
// been resolved.
uint8_t CurrentMode();

// True when CE should refuse this CreateProcess call outright. `imagePath` is
// the application name when the caller supplied one, otherwise its command
// line; both spellings resolve to the same answer.
bool ShouldRefuseProcessLaunch(const char* imagePath);

// Records that a refusal happened, for the one-line summary the hook logs. Kept
// separate from the decision so the decision stays a pure query.
void NoteUpdaterLaunchRefused(const char* imagePath);

}  // namespace ce::ngx_ota
