#pragma once

namespace ce::sampler_override {

// A translator can synchronously create native descriptors while forwarding a
// D3D11 sampler. The frontend owns that sampler's safety/bias policy; applying
// the native policy again can promote a protected sampler or add a bias twice.
inline thread_local unsigned g_d3d11SamplerCreationDepth = 0;

class D3D11SamplerCreationScope {
public:
    D3D11SamplerCreationScope() { ++g_d3d11SamplerCreationDepth; }
    ~D3D11SamplerCreationScope() { --g_d3d11SamplerCreationDepth; }
    D3D11SamplerCreationScope(const D3D11SamplerCreationScope&) = delete;
    D3D11SamplerCreationScope& operator=(const D3D11SamplerCreationScope&) = delete;
};

inline bool D3D11OwnsTranslatedSampler() { return g_d3d11SamplerCreationDepth != 0; }

}  // namespace ce::sampler_override
