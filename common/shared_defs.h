#pragma once

// Shared-memory ABI between CaptureEngine, the hook DLL, and the Vulkan layer.
//
// Layout-critical: every declaration sits inside the same #pragma pack(push, 8)
// region it always did, and the topic headers below preserve the original
// declaration order exactly. Do not reorder the includes.

// Constants and inline functions only, no layout, so it sits outside the
// packed region. abi_constants_and_config.h includes it as well; it is listed
// here because the graphics config's frame generation fields are declared in
// terms of the encodings it carries.
#include "shared_defs_detail/dlss_frame_generation_policy.h"
#include "shared_defs_detail/abi_constants_and_config.h"
#include "shared_defs_detail/ngx_policy_and_override_status.h"
#include "shared_defs_detail/capture_state.h"
#include "shared_defs_detail/frame_ring.h"
#include "shared_defs_detail/shared_memory_layout.h"
#include "shared_defs_detail/abi_signature_and_helpers.h"
