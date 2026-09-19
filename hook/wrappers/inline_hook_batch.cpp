#include "inline_hook.h"
#include "inline_hook_internal.h"
#include "hook_patch_transaction.h"

#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>
#include <vector>

#include "../common/hook_common.h"

namespace InlineHook {
namespace {

enum class BatchEntryState : uint8_t {
    kNotPrepared,
    kPrepared,
    kGroupEligible,
    kInstalled,
    kFailed,
};

constexpr size_t kNoHookIndex = std::numeric_limits<size_t>::max();

size_t InstallPublishedIndividually(PublishedHookSpec* hooks, size_t count) {
    size_t installedCount = 0;
    for (size_t i = 0; i < count; ++i) {
        hooks[i].installed = false;
        if (hooks[i].outTrampoline) {
            *hooks[i].outTrampoline = nullptr;
        }
        if (InstallPublished(hooks[i].target, hooks[i].detour, hooks[i].outTrampoline,
                             hooks[i].publisher, hooks[i].publisherContext)) {
            hooks[i].installed = true;
            ++installedCount;
        }
    }
    return installedCount;
}

}  // namespace

bool CanCommitPreparedEntryPatchLocked(size_t hookIndex) {
    if (hookIndex >= g_hooks.size()) {
        return false;
    }
    const HookEntry& hook = g_hooks[hookIndex];
    return !hook.installed && hook.target && hook.detour && hook.patchDestination && hook.patchSize > 0 &&
           memcmp(hook.target, hook.origBytes, static_cast<size_t>(hook.patchSize)) == 0;
}

bool CommitPreparedEntryPatchQuiescedLocked(size_t hookIndex) {
    if (!CanCommitPreparedEntryPatchLocked(hookIndex)) {
        return false;
    }
    HookEntry& hook = g_hooks[hookIndex];
    if (!WriteOwnedEntryPatchQuiesced(hook.target, hook.patchDestination, hook.patchSize, hook.origBytes,
                                      hook.installedBytes)) {
        return false;
    }
    hook.installed = true;
    return true;
}

bool CommitPreparedEntryPatchLocked(size_t hookIndex) {
    if (hookIndex >= g_hooks.size()) {
        return false;
    }
    HookEntry& hook = g_hooks[hookIndex];
    if (hook.installed || !hook.target || !hook.detour || !hook.patchDestination || hook.patchSize <= 0) {
        return false;
    }
    if (!WriteOwnedEntryPatch(hook.target, hook.patchDestination, hook.patchSize, hook.origBytes,
                              hook.installedBytes)) {
        return false;
    }
    hook.installed = true;
    return true;
}

size_t InstallPublishedBatch(PublishedHookSpec* hooks, size_t count) {
    if (!hooks || count == 0) {
        return 0;
    }

    std::vector<size_t> hookIndices;
    std::vector<BatchEntryState> states;
    try {
        hookIndices.assign(count, kNoHookIndex);
        states.assign(count, BatchEntryState::kNotPrepared);
    } catch (...) {
        HookLogImportant("InlineHook: Batch bookkeeping allocation failed; using independent transactions");
        return InstallPublishedIndividually(hooks, count);
    }

    std::lock_guard<std::mutex> lock(g_hookMutex);
    size_t installedCount = 0;
    size_t preparedCount = 0;
    for (size_t i = 0; i < count; ++i) {
        hooks[i].installed = false;
        if (hooks[i].outTrampoline) {
            *hooks[i].outTrampoline = nullptr;
        }
        size_t hookIndex = kNoHookIndex;
        if (!PreparePublishedHookLocked(&hooks[i], &hookIndex)) {
            continue;
        }
        if (hooks[i].installed) {
            states[i] = BatchEntryState::kInstalled;
            ++installedCount;
            continue;
        }
        if (hookIndex == kNoHookIndex) {
            continue;
        }
        hookIndices[i] = hookIndex;
        states[i] = BatchEntryState::kPrepared;
        ++preparedCount;
    }

    size_t groupedCommitCount = 0;
    size_t independentCommitCount = 0;
    bool groupReady = false;
    if (preparedCount > 0) {
        bool groupQuiesceUnstable = false;
        {
            // Trampoline allocation, instruction decoding, publication and all
            // logging are complete before any peer is suspended. While this
            // object is alive, do only bounded range checks and byte commits.
            ce::hook_patch::ThreadQuiescence groupQuiescence;
            groupReady = groupQuiescence.IsReady();
            if (groupReady) {
                for (size_t i = 0; i < count; ++i) {
                    if (states[i] != BatchEntryState::kPrepared) {
                        continue;
                    }
                    const HookEntry& hook = g_hooks[hookIndices[i]];
                    if (!groupQuiescence.IsRangeSafe(hook.target, static_cast<size_t>(hook.patchSize))) {
                        continue;
                    }
                    if (!CanCommitPreparedEntryPatchLocked(hookIndices[i])) {
                        states[i] = BatchEntryState::kFailed;
                        continue;
                    }
                    states[i] = BatchEntryState::kGroupEligible;
                }
                for (size_t i = 0; i < count; ++i) {
                    if (states[i] != BatchEntryState::kGroupEligible) {
                        continue;
                    }
                    if (CommitPreparedEntryPatchQuiescedLocked(hookIndices[i])) {
                        states[i] = BatchEntryState::kInstalled;
                        ++groupedCommitCount;
                    } else {
                        states[i] = BatchEntryState::kFailed;
                    }
                }
            } else {
                groupQuiesceUnstable =
                    (groupQuiescence.FailureReason() == ce::hook_patch::QuiesceFailure::kUnstableSnapshot);
            }
        }

        if (!groupReady && groupQuiesceUnstable) {
            ce::hook_patch::ThreadQuiescence fallbackQuiescence(
                ce::hook_patch::UnstableSnapshotPolicy::kAcceptSuspendedSet);
            if (fallbackQuiescence.IsReady()) {
                groupReady = true;
                for (size_t i = 0; i < count; ++i) {
                    if (states[i] != BatchEntryState::kPrepared) {
                        continue;
                    }
                    const HookEntry& hook = g_hooks[hookIndices[i]];
                    if (!fallbackQuiescence.IsRangeSafe(hook.target, static_cast<size_t>(hook.patchSize))) {
                        continue;
                    }
                    if (!CanCommitPreparedEntryPatchLocked(hookIndices[i])) {
                        states[i] = BatchEntryState::kFailed;
                        continue;
                    }
                    states[i] = BatchEntryState::kGroupEligible;
                }
                for (size_t i = 0; i < count; ++i) {
                    if (states[i] != BatchEntryState::kGroupEligible) {
                        continue;
                    }
                    if (CommitPreparedEntryPatchQuiescedLocked(hookIndices[i])) {
                        states[i] = BatchEntryState::kInstalled;
                        ++groupedCommitCount;
                    } else {
                        states[i] = BatchEntryState::kFailed;
                    }
                }
            }
        }

        // A failed group snapshot or a thread executing one particular target
        // does not reduce hook coverage. Retry only those entries through the
        // established independent transaction after every peer has resumed.
        for (size_t i = 0; i < count; ++i) {
            if (states[i] != BatchEntryState::kPrepared) {
                continue;
            }
            if (CommitPreparedEntryPatchLocked(hookIndices[i])) {
                states[i] = BatchEntryState::kInstalled;
                ++independentCommitCount;
            } else {
                states[i] = BatchEntryState::kFailed;
            }
        }
    }

    size_t failedPreparedCount = 0;
    for (size_t i = 0; i < count; ++i) {
        if (states[i] == BatchEntryState::kInstalled) {
            hooks[i].installed = true;
            if (hookIndices[i] != kNoHookIndex) {
                ++installedCount;
            }
            continue;
        }
        if (states[i] != BatchEntryState::kFailed || hookIndices[i] == kNoHookIndex) {
            continue;
        }
        hooks[i].publisher(nullptr, hooks[i].publisherContext);
        *hooks[i].outTrampoline = nullptr;
        ++failedPreparedCount;
    }

    // Indices were appended in request order, so reverse erasure preserves all
    // earlier indices. Published RX trampolines intentionally remain resident:
    // another route may already have acquired the briefly published pointer.
    for (size_t i = count; i-- > 0;) {
        if (states[i] == BatchEntryState::kFailed && hookIndices[i] != kNoHookIndex) {
            g_hooks.erase(g_hooks.begin() + static_cast<std::ptrdiff_t>(hookIndices[i]));
        }
    }

    HookLogImportant(
        "InlineHook: Batch completed (requested=%zu prepared=%zu installed=%zu grouped=%zu independent=%zu "
        "failedPrepared=%zu groupReady=%d)",
        count, preparedCount, installedCount, groupedCommitCount, independentCommitCount, failedPreparedCount,
        groupReady ? 1 : 0);
    return installedCount;
}

}  // namespace InlineHook
