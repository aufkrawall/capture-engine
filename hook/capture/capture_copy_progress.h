#pragma once

#include <cstdint>

// Copy-fence progress as the Present-stall attribution reads it around the
// game's Present (present_stall_attribution.h).
struct CaptureCopyProgress {
    bool valid = false;           // a capture generation with a live fence
    uint64_t submitted = 0;       // latest fence value a copy was submitted for
    uint64_t completed = 0;       // the fence's completed value
    uint32_t framesCaptured = 0;  // copies submitted by this generation
};
