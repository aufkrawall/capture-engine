// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall
#include "include/cengine/cengine.h"

// Entry points that work without a runtime: no handle, no global state, no allocation.
extern "C" {

uint32_t ce_api_version(void) {
    return CE_API_VERSION;
}

const char* ce_status_string(ce_status_t status) {
    switch (status) {
        case CE_OK:
            return "ok";
        case CE_E_INVALID_ARGUMENT:
            return "invalid argument (NULL, bad value, bad struct_size or stale handle)";
        case CE_E_INVALID_STATE:
            return "invalid state for this operation";
        case CE_E_BUSY:
            return "busy (another runtime owns this process or Windows session)";
        case CE_E_VERSION:
            return "API version incompatible";
        case CE_E_PACKAGE:
            return "runtime package incomplete or mismatched";
        case CE_E_UNSUPPORTED:
            return "capability unavailable on this system";
        case CE_E_TIMEOUT:
            return "timed out";
        case CE_E_SETTINGS:
            return "settings validation failed";
        case CE_E_CONFLICT:
            return "settings changed since the edit began";
        case CE_E_IPC:
            return "helper did not accept or acknowledge the request";
        case CE_E_PROCESS:
            return "helper process failed to start or was lost";
        case CE_E_IO:
            return "file system failure";
        case CE_E_NO_MEMORY:
            return "allocation failed";
        case CE_E_BUFFER_TOO_SMALL:
            return "buffer too small";
        case CE_E_NOT_FOUND:
            return "not found";
        case CE_E_CANCELED:
            return "canceled";
        case CE_E_INTERNAL:
            return "internal failure";
        default:
            return "unknown status";
    }
}

}  // extern "C"
