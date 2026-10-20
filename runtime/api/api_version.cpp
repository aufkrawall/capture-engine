// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall
#include "include/cengine/cengine.h"

#include "common/platform/build_identity.h"

// Its own unit, so the product-version string has one definition in the image.
extern "C" {

const char* ce_version_string(void) {
    return GetCaptureVersion();
}

}  // extern "C"
