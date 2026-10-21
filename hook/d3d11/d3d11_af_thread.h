#pragma once

// Per-thread state of CE's D3D11 forced AF: a block for each thread that creates samplers
// (d3d11_af_creation.cpp) or switches pixel shaders (d3d11_af_engine.cpp), with single-writer
// counters and two memos. Blocks are claimed on a thread's first use, never freed (cached thread
// pointers stay valid, and a sentinel can run after static destruction at process exit), and
// reclaimed from threads that have exited. Threads beyond kMaxThreadBlocks run without one.
//
// Neither memo holds a reference: a held sampler counts toward the device's 4096 unique sampler
// objects, and held shaders outlive the game's own use of them. So an entry may name an object
// only while that object carries a private-data interface of CE's whose final release - the
// runtime destroying the object - clears the object from every block before its address can be
// reused: the replacement registry's sentinel for a sampler (ForgetCreatedSampler), the shader's
// metadata handle for a shader (ForgetShader). The owner thread reads an entry's key while the
// game holds the object it asks about, so a matching key always names that live object.

#include <d3d11.h>

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "d3d11_af_engine.h"
#include "d3d11_af_shader_memo.h"

namespace ce::d3d11_af {

// A creation the memo answered completely - policy result and tag - counts once (kFullHits);
// SnapshotSamplerCreationStats derives the totals from the parts.
enum CreationStat : size_t {
    kFullHits,         // policy result from the memo, tag vouched for: no private-data read
    kOtherCreates,     // every other creation
    kDescHitsPartial,  // policy result from the memo, tag read back or written
    kTagsWritten,
    kTagsKeptRead,  // tag read back and found unchanged
    kRaced,         // another creation rewrote a tag while this one confirmed its own: not memoized
    kTimedCreates,
    kTimedCeTicks,
    kTimedRuntimeTicks,
    kCreationStatCount,
};

struct CreationMemoEntry {
    // The object last confirmed to carry this entry's tag. Set by the owner thread; cleared by
    // any thread (ForgetCreatedSampler).
    std::atomic<const ID3D11SamplerState*> tagged{nullptr};
    // Owner thread only.
    bool used = false;
    bool modified = false;
    uint32_t descHash = 0;
    uint64_t policyHash = 0;
    uint64_t stamp = 0;
    // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization) - zero-initialized placeholder; set when claimed
    D3D11_SAMPLER_DESC application = {};
    // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization) - zero-initialized placeholder; set when claimed
    D3D11_SAMPLER_DESC applied = {};
};

struct ThreadBlock {
    static constexpr size_t kCreationEntries = 128;
    static constexpr size_t kCreationProbe = 4;

    HANDLE owner = nullptr;  // claim lock: the owning thread, null when unknown (never reclaimed)
    Count creationStats[kCreationStatCount];
    uint64_t creationClock = 0;  // owner thread
    uint32_t shaderClock = 0;    // owner thread
    CreationMemoEntry creation[kCreationEntries];
    ShaderMemoEntry shaders[kShaderEntries];  // placement: d3d11_af_shader_memo.h
};

inline constexpr size_t kMaxThreadBlocks = 32;

// The calling thread's block, claimed on first use; null when every block belongs to a live
// thread (a claim is retried every 4096 calls).
ThreadBlock* CurrentThreadBlock();
// Block `index` (< kMaxThreadBlocks) if ever claimed: any thread, blocks are never freed.
ThreadBlock* ThreadBlockAt(size_t index);
size_t ClaimedThreadBlocks();

// ---- Shader memo (owner thread) ----

// The memoized metadata handle of `shader`, borrowed from the live shader; null when not cached.
IUnknown* FindShaderPlan(ThreadBlock& block, const ID3D11PixelShader* shader);
// Caches `shader` (which must carry `planOwner` as its metadata). True when an entry was evicted.
bool RememberShaderPlan(ThreadBlock& block, const ID3D11PixelShader* shader, IUnknown* planOwner);
// `shader` is being destroyed (its metadata handle's final release): no block may name it.
// Any thread; lock-free.
void ForgetShader(const ID3D11PixelShader* shader);

}  // namespace ce::d3d11_af
