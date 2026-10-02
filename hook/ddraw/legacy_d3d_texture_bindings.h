/**
 * Ownership of a Direct3D 7 device's application texture bindings.
 *
 * A Direct3D 7 state block records the surface bound to each texture stage as a
 * raw `IDirectDrawSurface7` pointer and takes no reference on it. The runtime
 * itself never re-binds an old texture, so an application is free to release a
 * surface while it is still bound: the device keeps its own internal texture
 * object alive and nothing ever dereferences the surface interface again.
 *
 * Restoring such a state block breaks that. `ApplyStateBlock` replays the
 * recorded bindings through `IDirect3DDevice7::SetTexture`, whose first act is
 * to read `surface->lpLcl` - already freed and NULL on a destroyed interface.
 * Gothic II session `20260916_000027` is exactly that fault:
 * `DIRECT3DDEVICEI::SetTextureInternal+0x12`, `mov eax,[eax]` with EAX zero,
 * reached from the runtime's internal state-replay dispatch rather than from
 * the public `SetTexture` wrapper, moments after the intro videos released the
 * textures they had left bound.
 *
 * CE's overlay sidecar is the only state-block user in such a process, so CE
 * owns the missing guarantee: while the device holds a binding, CE holds a
 * reference to it. That is bounded by construction - at most one surface per
 * stage - and is released as soon as the application binds something else.
 *
 * `Traits` supplies AddRef/Release so the ownership rules stay testable without
 * a DirectDraw device.
 */

#pragma once

#include <array>
#include <cstddef>

namespace ce::legacy_d3d {

// Direct3D 7 exposes eight texture stages; `D3DDP_MAXTEXCOORD` and every
// `IDirect3DDevice7` stage argument are bounded by it.
inline constexpr size_t kTextureStageCount = 8;

template <typename Traits>
class TextureBindingSet {
public:
    TextureBindingSet() = default;
    TextureBindingSet(const TextureBindingSet&) = delete;
    TextureBindingSet& operator=(const TextureBindingSet&) = delete;
    ~TextureBindingSet() {
        ReleaseAll();
    }

    // Records what the application bound to `stage`. A stage outside the
    // tracked range is remembered rather than ignored: a restore CE cannot
    // prove complete must not be trusted, and returning false is how the caller
    // learns that.
    bool Bind(size_t stage, void* texture) {
        if (stage >= kTextureStageCount) {
            untrackedStageObserved_ = true;
            return false;
        }
        void* const previous = bindings_[stage];
        if (previous == texture) {
            return true;
        }
        // The new reference is taken before the old one is dropped, so a
        // surface that is merely moved between stages never passes through a
        // zero reference count.
        if (texture) {
            Traits::AddRef(texture);
        }
        bindings_[stage] = texture;
        if (previous) {
            Traits::Release(previous);
        }
        return true;
    }

    void* Get(size_t stage) const {
        return stage < kTextureStageCount ? bindings_[stage] : nullptr;
    }

    // True while every binding the device holds is one CE owns a reference to.
    // This is the precondition for letting a Direct3D 7 state block restore the
    // application's textures at all.
    bool RestoreIsReferenceSafe() const {
        return !untrackedStageObserved_;
    }

    // Drops every owned reference. `releaseReferences` is false only during
    // process teardown, where calling into a runtime that may already be
    // unloaded is worse than leaking a reference the process is about to drop
    // anyway.
    void ReleaseAll(bool releaseReferences = true) {
        for (auto& binding : bindings_) {
            void* const owned = binding;
            binding = nullptr;
            if (owned && releaseReferences) {
                Traits::Release(owned);
            }
        }
        untrackedStageObserved_ = false;
    }

private:
    std::array<void*, kTextureStageCount> bindings_{};
    bool untrackedStageObserved_ = false;
};

}  // namespace ce::legacy_d3d
