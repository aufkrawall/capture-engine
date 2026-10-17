#pragma once

// The game's D3D11 sampler creations under CE's creation-time sampler policy
// (CreateSamplerWithCreationPolicy, d3d11_af_engine.h), with a per-thread memo.
//
// BioShock Infinite creates a sampler on almost every draw for each texture still streaming in
// (~157k per second, session 20261017_193612). For 99.9% of them the runtime hands back an
// existing object for an identical descriptor, and that object already carries CE's creation
// tag. Each creation still applied the whole sampler policy to the descriptor (string compares,
// bias parsing), read the tag back through GetPrivateData to confirm it, and counted itself in
// two process-wide atomics.
//
// Each creating thread now owns a block: single-writer counters and a memo keyed by the policy
// hash and the application's descriptor. An entry keeps the policy's result (a pure function of
// both) and the object last confirmed to carry exactly that tag. The memo holds no references -
// a held sampler counts toward the device's 4096 unique sampler objects - so an object's address
// can be reused once it dies. Every confirmed object therefore carries the replacement registry's
// sentinel, whose final release (the runtime destroying the object) clears the object from every
// block before its memory can be reused. A tag rewrite by another creation clears it as well.

#include <d3d11.h>

namespace ce::d3d11_af {

// `sampler` is being destroyed (its sentinel's final release) or its creation tag was just
// rewritten: no memo may vouch for its tag any longer. Any thread; lock-free.
void ForgetCreatedSampler(const ID3D11SamplerState* sampler);

}  // namespace ce::d3d11_af
