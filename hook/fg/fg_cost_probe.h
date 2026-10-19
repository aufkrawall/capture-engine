#pragma once

#include <cstdint>

#include "common/graphics/fg_cost_probe_mask.h"

// Frame-generation cost probe, hook side: the bit set and the parser (and the full
// description of what the probe is for) are in common/graphics/fg_cost_probe_mask.h.
namespace ce::fg_cost_probe {

// Resolved once per process from CE_FG_COST_PROBE.
uint32_t Mask();

inline bool Active(uint32_t bit) { return (Mask() & bit) != 0; }

}  // namespace ce::fg_cost_probe
