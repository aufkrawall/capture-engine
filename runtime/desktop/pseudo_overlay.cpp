#include "pseudo_overlay_internal.h"

// ---- Static instance pointer for wndproc routing ----
PseudoOverlay* PseudoOverlay::instance_ = nullptr;

PseudoOverlay::PseudoOverlay() = default;
