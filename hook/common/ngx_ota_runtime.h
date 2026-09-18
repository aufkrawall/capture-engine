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
 * The mode is published once the hook thread has a config. That is NOT early
 * enough on its own, and the original version of this comment claiming it was
 * ("the NGX core does not reach its updater until well after that point") was
 * wrong: session 20260918_223542 created nine nvngx_update.exe processes at
 * 22:35:48, all parented to the game, while CE published the policy at
 * 22:35:49.072. So `CurrentMode` falls back to the value the injector already
 * published in shared memory, which in that session was available from
 * 22:35:42.842 - before the game existed.
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

// The OTA mode in force. Before the hook thread publishes one, this resolves
// the injector's already-published value from shared memory rather than
// answering "default" - see the note above for why that distinction decides
// whether the first burst of updater launches is answered at all.
uint8_t CurrentMode();

// True when CE should refuse this CreateProcess call outright. `imagePath` is
// the application name when the caller supplied one, otherwise its command
// line; both spellings resolve to the same answer.
bool ShouldRefuseProcessLaunch(const char* imagePath);

// Records that a refusal happened, for the one-line summary the hook logs. Kept
// separate from the decision so the decision stays a pure query.
void NoteUpdaterLaunchRefused(const char* imagePath);

}  // namespace ce::ngx_ota
