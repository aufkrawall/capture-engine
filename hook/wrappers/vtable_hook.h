/**
 * VTable Hooking Utility
 *
 * This header now forwards to vtable_hook_minhook.h which provides
 * a MinHook-based implementation for more stable hooking.
 * 
 * The API remains the same for backwards compatibility.
 */

#pragma once

// Include the MinHook-based implementation
#include "vtable_hook_minhook.h"
