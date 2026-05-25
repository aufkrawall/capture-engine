#pragma once

namespace ce::injection_policy {

inline bool ShouldInjectAfterGraphicsProbe(bool d3d12Loaded) {
    // The hook is designed to install before the first real swapchain whenever
    // possible. A fixed post-D3D12 delay can miss early Presents and leaves the
    // overlay visibly late in games that initialize Streamline/FFX before the
    // main loop starts.
    (void)d3d12Loaded;
    return true;
}

}  // namespace ce::injection_policy
