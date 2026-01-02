#include "fg_detection.h"
#include "hook_common.h"
#include <vector>
#include <string>

FGCompatibility g_FGCompat;

FGCompatibility::FGType FGCompatibility::DetectLoadedFGRuntime() {
    // FG Handling Removed per user request
    return FGType::None;
}

bool FGCompatibility::IsFGLikelyActive() const {
    // Always return false to allow overlay immediately
    return false;
}

void FGCompatibility::SuspendFor(int milliseconds) {
    // No-op
}

void FGCompatibility::OnDeviceChange() {
    // No-op
}

void FGCompatibility::OnSwapchainRecreation() {
    // No-op
}

void FGCompatibility::CheckBehavioralPatterns() {
    // No-op
}

FGCompatibility::FGType FGCompatibility::GetDetectedType() const {
    return FGType::None;
}

int FGCompatibility::GetRecommendedInitDelayFrames() const {
    return 0;
}

void FGCompatibility::RecordPresentCall() {
    // No-op
}

void FGCompatibility::RecordRealFrame() {
    // No-op
}

float FGCompatibility::GetOutputFPS() const {
    return 0.0f;
}

float FGCompatibility::GetBaseFPS() const {
    return 0.0f;
}
