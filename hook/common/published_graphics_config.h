#pragma once

/*
 * The graphics configuration the injector published in shared memory, read
 * from inside the game process.
 *
 * This exists because CE's hooks go in during DllMain but the config they act
 * on does not arrive until the hook thread has read config.ini - roughly 400 ms
 * later in session 20260919_194818 (DllMain 19:48:28.860, config 19:48:29.25).
 * A hook that is installed but has no answer passes the call through, and for
 * a one-shot load or launch that is indistinguishable from not being hooked at
 * all. The `ngx_ota` mode hit this first; the loader redirect has the same
 * shape and the same fix, because the injector already publishes both, before
 * the game process even exists.
 *
 * Only OpenFileMapping/MapViewOfFile are used, neither of which takes the
 * loader lock, so this is safe on the DllMain and LdrLoadDll paths.
 */

#include "../../common/shared_defs.h"

namespace ce::published_config {

// Copies the injector's published graphics config. Returns false when no CE
// host answered - which is NOT the same as a host publishing defaults, and is
// why the caller must not cache a false result.
bool ReadPublishedGraphicsConfig(SharedGraphicsConfig& out);

// Resolves the configured runtime-override DLL paths once, for use before the
// hook thread's own config exists. Idempotent; safe to call from DllMain.
void ResolveEarlyRuntimeOverridePaths();

// The published override paths, or "" when none was published or no host
// answered. Never null, so a caller can use the result directly.
//
// These are only meaningful until the hook thread's config load, which is
// authoritative; `main_redirect.cpp` prefers that and falls back to these.
const char* EarlyDlssSrDllPath();
const char* EarlyDlssRrDllPath();
const char* EarlyDlssFgDllPath();
const char* EarlyStreamlineDllPath();

// True once a host has answered, so callers can tell "no override configured"
// from "nothing known yet".
bool EarlyRuntimeOverridePathsResolved();

}  // namespace ce::published_config
