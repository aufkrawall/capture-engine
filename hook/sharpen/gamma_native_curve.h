#pragma once

namespace ce::gamma {

// Published only after UE's actual game/render/write-through storage verifies.
// Negative means no confirmed native override, so the explicit source wins.
void PublishVerifiedNativeCurve(float exponent);
float VerifiedNativeCurve(bool nativeOverrideRequested);

}  // namespace ce::gamma
