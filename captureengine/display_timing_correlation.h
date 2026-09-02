#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>
#include <iterator>

// Exact identity for an MPO layer transition.  Source and layer alone are
// insufficient while multiple PresentIds are in flight.
struct DisplayLayerPresentKey {
    uint32_t displaySource = 0;
    uint32_t layer = 0;
    uint64_t presentId = 0;

    bool operator==(const DisplayLayerPresentKey& other) const {
        return displaySource == other.displaySource && layer == other.layer && presentId == other.presentId;
    }
};

struct DisplayLayerPresentKeyHash {
    std::size_t operator()(const DisplayLayerPresentKey& key) const {
        const uint64_t foldedPresent = key.presentId ^ (key.presentId >> 32u);
        return static_cast<std::size_t>((static_cast<uint64_t>(key.displaySource) << 32u) ^ key.layer ^
                                        foldedPresent);
    }
};

enum class DisplayCompletionKind : uint8_t {
    Unconditional,
    Sync,
    Immediate,
};

struct DisplayFrameTypeState {
    int64_t timestamp = 0;
    bool explicitFrameSeen = false;
    bool generatedFrameSeen = false;
    bool nonGeneratedFrameSeen = false;
    bool payloadQueued = false;
    bool fallbackCommitted = false;
};

inline bool IsGeneratedDisplayFrameType(uint8_t frameType);
inline bool ShouldPublishDisplayCompletion(DisplayCompletionKind completionKind,
                                           const DisplayFrameTypeState* state);

struct DisplayLayerAssociation {
    uint32_t processId = 0;
    uint64_t associationId = 0;
    int64_t timestamp = 0;
    int64_t presentStartTimestamp = 0;
};

struct DisplayPendingFrameTypeFlip {
    int64_t screenTime = 0;
    int64_t eventTimestamp = 0;
    uint8_t frameType = 0;
};

struct PendingRuntimePresent {
    uint32_t threadId = 0;
    int64_t timestamp = 0;
};

struct SubmitAssociation {
    uint32_t processId = 0;
    int64_t timestamp = 0;
    uint64_t associationId = 0;
    int64_t presentStartTimestamp = 0;
};

struct PendingTimestamp {
    uint32_t processId = 0;
    uint64_t associationId = 0;
    int64_t timestamp = 0;
    DisplayCompletionKind completionKind = DisplayCompletionKind::Unconditional;
    uint64_t arrivalOrder = 0;
    int64_t presentStartTimestamp = 0;
};

// Stateful reducer for the two independently delivered ETW streams.  The
// tuple key is PresentMon-compatible for this service lifetime; all maps are
// cleared when the service stops/is destroyed.  It deliberately implements a
// bounded policy, not a causal claim about arbitrarily late provider events.
class DisplayTimingCorrelation {
public:
    enum class PayloadResult : uint8_t { Pending, Correlated, Duplicate, Late };

    void Associate(const DisplayLayerPresentKey& key, const DisplayLayerAssociation& association) {
        layerAssociations_[key] = association;
        auto pending = pendingPayloads_.find(key);
        if (pending == pendingPayloads_.end())
            return;
        CorrelatePayload(association, pending->second);
        pendingPayloads_.erase(pending);
    }

    std::optional<DisplayLayerAssociation> FindAssociation(const DisplayLayerPresentKey& key) const {
        const auto it = layerAssociations_.find(key);
        return it == layerAssociations_.end() ? std::nullopt : std::optional<DisplayLayerAssociation>(it->second);
    }

    PayloadResult ObservePayload(const DisplayLayerPresentKey& key, const DisplayPendingFrameTypeFlip& payload) {
        const auto association = FindAssociation(key);
        if (association)
            return CorrelatePayload(*association, payload);
        const auto [it, inserted] = pendingPayloads_.emplace(key, payload);
        return inserted ? PayloadResult::Pending : PayloadResult::Duplicate;
    }

    const std::unordered_map<DisplayLayerPresentKey, DisplayPendingFrameTypeFlip, DisplayLayerPresentKeyHash>&
    pendingPayloads() const noexcept { return pendingPayloads_; }
    const std::unordered_map<uint64_t, DisplayFrameTypeState>& states() const noexcept { return states_; }
    std::vector<PendingTimestamp> TakePayloads() {
        std::vector<PendingTimestamp> result;
        result.swap(payloadQueue_);
        return result;
    }
    DisplayFrameTypeState* State(uint64_t associationId) { return &states_[associationId]; }

    PayloadResult QueuePayload(uint32_t processId, uint64_t associationId, int64_t screenTime,
                               int64_t eventTimestamp, uint8_t frameType,
                               std::vector<PendingTimestamp>& queue, uint64_t& order,
                               int64_t presentStartTimestamp = 0) {
        auto& state = states_[associationId];
        // Classify before touching timestamps: duplicates and late payloads do
        // not extend retention or overwrite the accepted transition time.
        if (state.fallbackCommitted)
            return PayloadResult::Late;
        if (state.payloadQueued)
            return PayloadResult::Duplicate;
        state.timestamp = eventTimestamp;
        state.explicitFrameSeen = true;
        state.generatedFrameSeen |= IsGeneratedDisplayFrameType(frameType);
        state.nonGeneratedFrameSeen |= !IsGeneratedDisplayFrameType(frameType);
        state.payloadQueued = true;
        queue.push_back({processId, associationId, screenTime, DisplayCompletionKind::Unconditional, order++,
                         presentStartTimestamp});
        return PayloadResult::Correlated;
    }

    void CommitFallback(PendingTimestamp& pending) {
        auto& state = states_[pending.associationId];
        state.fallbackCommitted = true;
        if (state.timestamp == 0)
            state.timestamp = pending.timestamp;
    }

    void QueueFallback(uint32_t processId, uint64_t associationId, int64_t timestamp,
                       DisplayCompletionKind kind, std::vector<PendingTimestamp>& queue, uint64_t& order,
                       int64_t presentStartTimestamp = 0) {
        if (timestamp <= 0)
            return;
        auto& state = states_[associationId];
        if (state.timestamp == 0)
            state.timestamp = timestamp;
        queue.push_back({processId, associationId, timestamp, kind, order++, presentStartTimestamp});
    }

    bool ShouldPublish(const PendingTimestamp& pending) const {
        const auto it = states_.find(pending.associationId);
        return ShouldPublishDisplayCompletion(pending.completionKind,
            it == states_.end() ? nullptr : &it->second);
    }

    void Prune(int64_t cutoff) {
        for (auto it = layerAssociations_.begin(); it != layerAssociations_.end();)
            it = it->second.timestamp < cutoff ? layerAssociations_.erase(it) : std::next(it);
        for (auto it = pendingPayloads_.begin(); it != pendingPayloads_.end();)
            it = it->second.eventTimestamp < cutoff ? pendingPayloads_.erase(it) : std::next(it);
        for (auto it = states_.begin(); it != states_.end();)
            it = it->second.timestamp < cutoff ? states_.erase(it) : std::next(it);
    }

    void Clear() { layerAssociations_.clear(); pendingPayloads_.clear(); states_.clear(); }

private:
    PayloadResult CorrelatePayload(const DisplayLayerAssociation& association,
                                   const DisplayPendingFrameTypeFlip& payload) {
        return QueuePayload(association.processId, association.associationId, payload.screenTime,
                            payload.eventTimestamp, payload.frameType, payloadQueue_, nextOrder_,
                            association.presentStartTimestamp);
    }

    std::unordered_map<DisplayLayerPresentKey, DisplayLayerAssociation, DisplayLayerPresentKeyHash> layerAssociations_;
    std::unordered_map<DisplayLayerPresentKey, DisplayPendingFrameTypeFlip, DisplayLayerPresentKeyHash> pendingPayloads_;
    std::unordered_map<uint64_t, DisplayFrameTypeState> states_;
    std::vector<PendingTimestamp> payloadQueue_;
    uint64_t nextOrder_ = 1;
};

inline bool IsGeneratedDisplayFrameType(uint8_t frameType) {
    return frameType == 50 || frameType == 100;
}

inline void ObserveDisplayFrameType(DisplayFrameTypeState& state, int64_t timestamp, uint8_t frameType) {
    state.timestamp = timestamp;
    state.explicitFrameSeen = true;
    if (IsGeneratedDisplayFrameType(frameType))
        state.generatedFrameSeen = true;
    else
        state.nonGeneratedFrameSeen = true;
}

inline bool ShouldPublishDisplayCompletion(DisplayCompletionKind completionKind,
                                           const DisplayFrameTypeState* state) {
    if (completionKind == DisplayCompletionKind::Unconditional || !state || !state->explicitFrameSeen)
        return true;
    // A generated FrameType is a distinct display transition.  A later sync
    // or immediate event remains the application's completion.  An explicit
    // non-generated FrameType identifies that completion as a duplicate.
    return !state->nonGeneratedFrameSeen;
}
