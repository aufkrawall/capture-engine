// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall
#include "include/cengine/cengine_draft.h"

#include "common/platform/build_identity.h"

// Its own unit: v1 (captureengine/app/libcaptureengine.cpp) exports the same symbol until M4 removes
// it, so linking both into one image needs this file left out or v1's definition removed.
extern "C" {

const char* ce_version_string(void) {
    return GetCaptureVersion();
}

}  // extern "C"
