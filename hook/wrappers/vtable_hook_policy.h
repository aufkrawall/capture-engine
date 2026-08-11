#pragma once

namespace ce::vtable_hook_policy {

inline bool ShouldReclaimRestoredSlot(const void* current, const void* detour, const void* predecessor) {
    return predecessor && current != detour && current == predecessor;
}

inline bool ShouldPreserveForeignFollower(const void* current, const void* detour, const void* predecessor) {
    return predecessor && current != detour && current != predecessor;
}

}  // namespace ce::vtable_hook_policy
