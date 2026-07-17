#include "build_identity.h"

#include "build_version.h"

uint32_t GetCurrentBuildNumber() noexcept {
    return BUILD_NUMBER;
}

const char* GetCaptureVersion() noexcept {
    return CAPTURE_VERSION;
}

const char* GetBuildTimestamp() noexcept {
    return BUILD_TIMESTAMP;
}
