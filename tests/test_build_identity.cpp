#include "../common/build_identity.h"

// Unit tests validate build-identity behavior with a deterministic identity.
// Product binaries continue to link common/build_identity.cpp and therefore
// retain their exact generated build number and timestamp.
uint32_t GetCurrentBuildNumber() noexcept {
    return 1;
}

const char* GetCaptureVersion() noexcept {
    return "0.1.test";
}

const char* GetBuildTimestamp() noexcept {
    return "unit-test";
}
