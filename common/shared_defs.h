#pragma once

// Shared-memory ABI between CaptureEngine, the hook DLL, and the Vulkan layer.
//
// Layout-critical: every declaration sits inside the same #pragma pack(push, 8)
// region it always did, and the topic headers below preserve the original
// declaration order exactly. Do not reorder the includes.

#include "shared_defs_detail/abi_constants_and_config.h"
#include "shared_defs_detail/capture_state.h"
#include "shared_defs_detail/frame_ring.h"
#include "shared_defs_detail/shared_memory_layout.h"
#include "shared_defs_detail/abi_signature_and_helpers.h"
