#pragma once

#include "common/graphics/post_process_route_policy.h"

// A route outside the DX12 frame transaction (PostSL render path, FSR present callback, FSR overlay output) ran, or
// failed to run, the post-process pass for one frame. NotRequested results are ignored.
void NotePostProcessRouteResult(ce::post_process_route::RuntimeRoute route,
                                ce::post_process_route::PassResult result);
