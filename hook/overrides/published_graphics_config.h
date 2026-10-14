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

#include <windows.h>

#include <cstddef>

#include "common/ipc/shared_defs.h"

namespace ce::published_config {

// Copies the injector's published graphics config. Returns false when no CE
// host answered - which is NOT the same as a host publishing defaults, and is
// why the caller must not cache a false result.
bool ReadPublishedGraphicsConfig(SharedGraphicsConfig& out);

// The configured runtime-override paths (dlss_*_dll_path, streamline_dll_path)
// as UTF-8, the encoding of every path the loader redirect compares and loads.
// A published snapshot is immutable: readers on the loader-lock path hold the
// reference without a lock while the hook thread publishes its own config, and
// a later inherited-renderer sync publishes a new snapshot instead of mutating.
struct RuntimeOverridePaths {
    // A MAX_PATH code-page value needs at most three UTF-8 bytes per character.
    static constexpr size_t kCapacity = MAX_PATH * 3;
    char dlssSr[kCapacity];
    char dlssRr[kCapacity];
    char dlssFg[kCapacity];
    char streamline[kCapacity];
};

// Resolves the injector-published paths once, for use before the hook
// thread's own config exists. Idempotent; safe to call from DllMain (no
// allocation, no loader calls). Never replaces a local publication.
void ResolveEarlyRuntimeOverridePaths();

// Publishes the hook thread's authoritative paths from code-page config text.
// Allocates, so it must not run in DllMain. A snapshot equal to the current
// one is not republished.
void PublishLocalRuntimeOverridePaths(const char* dlssSr, const char* dlssRr, const char* dlssFg,
                                      const char* streamline, const char* source);

// The current snapshot: the local publication once it exists, else the early
// one, else all-empty. Never null; every field is NUL-terminated.
const RuntimeOverridePaths& CurrentRuntimeOverridePaths();

// True once a host has answered, so callers can tell "no override configured"
// from "nothing known yet".
bool EarlyRuntimeOverridePathsResolved();

// Code-page text to UTF-8 into `destination`, without allocating. False (and an
// empty destination) when the text cannot be converted or does not fit.
bool CodePagePathToUtf8(const char* source, unsigned codePage, char* destination, size_t capacity);

}  // namespace ce::published_config
