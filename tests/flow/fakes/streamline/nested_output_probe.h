#pragma once

// Test-only observation of the native buffer the nested presenter copied into and presented, read once its Present
// returned (after the Present chain and CE drew on it). A sample says nothing about whether DXGI displayed it.

#include "tests/flow/fakes/output_pixel_probe.h"

namespace ce::flow::fake {

using NestedOutputProbe = OutputPixelProbe;

}  // namespace ce::flow::fake
