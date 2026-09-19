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

// Applies as much of the policy as `DllMain` can know, from the mode the
// injector published in shared memory. Two things happen here and both matter:
//
//  - `__NGX_DISABLE_UPDATER` is written. This is the only mechanism that stops
//    the NGX core from *attempting* a launch, and it has to be in place before
//    the core reads its environment. Publishing it from the hook thread's
//    config load was ~550 ms late in session 20260918_224737, which is why nine
//    updaters had to be refused one by one rather than never being started.
//  - The mode is resolved into process state, so the very first CreateProcess
//    call is answered without a lazy read on a hot path.
//
// Reads shared memory and writes an environment variable; loads nothing, so it
// is safe beside the kernel32 loader hooks under the loader lock. Idempotent,
// and a no-op once `PublishPolicy` has run - that value is the authoritative
// one. Also runs in launcher processes, where the variable is then inherited by
// the game CE launches, before CE's DLL is even in it.
void ApplyEarlyPolicyFromPublishedConfig();

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
//
// The wide overload exists so `HookedCreateProcessW` decides on the caller's
// own string. It used to convert into a MAX_PATH narrow buffer first, and
// WideCharToMultiByte writes nothing when the buffer is too small, so a command
// line over 260 characters resolved to "" and was never recognized.
bool ShouldRefuseProcessLaunch(const char* imagePath);
bool ShouldRefuseProcessLaunch(const wchar_t* imagePath);

// Records that a refusal happened, for the one-line summary the hook logs. Kept
// separate from the decision so the decision stays a pure query.
void NoteUpdaterLaunchRefused(const char* imagePath);
void NoteUpdaterLaunchRefused(const wchar_t* imagePath);

}  // namespace ce::ngx_ota
