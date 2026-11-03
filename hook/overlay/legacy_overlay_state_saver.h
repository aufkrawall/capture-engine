#pragma once

#include <cstddef>
#include <cstdint>

namespace CustomOverlay {

enum class LegacyOverlayStateKind : uint8_t { kRender, kStage, kSampler };

// The application's value of each device state a legacy (D3D9/D3D8/D3D7)
// overlay draw changes: read the first time the draw changes it, written back
// afterwards.
//
// A state block is not the tool for this. D3DSBT_ALL saves and restores the
// whole device on every frame. A block recorded with BeginStateBlock and
// EndStateBlock costs no more than the states it holds, but Windows' d3d9
// copies a fresh dispatch table over the device's own in both calls, erasing
// every hook CE or another tool wrote into it (tests/test_dx9_overlay_backend.cpp).
// Forced anisotropic filtering went dead in Borderlands 2 the moment the
// overlay recorded its block. The D3D8 and D3D7 runtimes are not known to
// differ, and no 64-bit build of them exists to check.
//
// Every state write of the draw goes through this object, so the restore
// cannot miss one; a write that would not change the value is skipped on both
// sides. Bindings (textures, buffers, shaders, viewport) are the backend's to
// save, since each holds a reference.
//
// `Access` adapts one API:
//   using Device = ...;
//   static bool Read(Device*, LegacyOverlayStateKind, uint32_t stage, uint32_t state, uint32_t* value);
//   static bool Write(Device*, LegacyOverlayStateKind, uint32_t stage, uint32_t state, uint32_t value);
// each returning whether the device accepted the call.
template <typename Access>
class LegacyOverlayStates {
public:
    using Device = typename Access::Device;
    static constexpr size_t kCapacity = 48;

    // `readable` is false on a device that answers no Get* call (a D3D9/D3D8
    // pure device). The caller saves its state with a D3DSBT_ALL block then;
    // writes go straight through and Restore has nothing to do.
    LegacyOverlayStates(Device* device, bool readable) : device_(device), readable_(readable) {}

    void Render(uint32_t state, uint32_t value) {
        Set(LegacyOverlayStateKind::kRender, 0, state, value);
    }
    void Stage(uint32_t stage, uint32_t state, uint32_t value) {
        Set(LegacyOverlayStateKind::kStage, stage, state, value);
    }
    void Sampler(uint32_t sampler, uint32_t state, uint32_t value) {
        Set(LegacyOverlayStateKind::kSampler, sampler, state, value);
    }

    // Writes back every state whose value the draw changed.
    void Restore() {
        for (size_t i = 0; i < count_; ++i) {
            const Entry& entry = entries_[i];
            if (entry.current != entry.saved)
                Access::Write(device_, entry.kind, entry.stage, entry.state, entry.saved);
        }
        count_ = 0;
    }

    // States left at the application's value because it could not be read
    // (or the table was full): the overlay may draw wrong, the game never does.
    uint32_t Unread() const {
        return unread_;
    }
    size_t Saved() const {
        return count_;
    }

private:
    struct Entry {
        LegacyOverlayStateKind kind = LegacyOverlayStateKind::kRender;
        uint32_t stage = 0;
        uint32_t state = 0;
        uint32_t saved = 0;
        uint32_t current = 0;
    };

    void Set(LegacyOverlayStateKind kind, uint32_t stage, uint32_t state, uint32_t value) {
        if (!readable_) {
            Access::Write(device_, kind, stage, state, value);
            return;
        }
        Entry* entry = nullptr;
        for (size_t i = 0; i < count_ && !entry; ++i) {
            if (entries_[i].kind == kind && entries_[i].stage == stage && entries_[i].state == state)
                entry = &entries_[i];
        }
        if (!entry) {
            uint32_t saved = 0;
            if (count_ == kCapacity || !Access::Read(device_, kind, stage, state, &saved)) {
                ++unread_;
                return;
            }
            entry = &entries_[count_++];
            *entry = {kind, stage, state, saved, saved};
        }
        if (entry->current != value && Access::Write(device_, kind, stage, state, value))
            entry->current = value;
    }

    Device* device_ = nullptr;
    bool readable_ = true;
    Entry entries_[kCapacity] = {};
    size_t count_ = 0;
    uint32_t unread_ = 0;
};

}  // namespace CustomOverlay
