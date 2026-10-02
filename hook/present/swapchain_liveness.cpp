#include "swapchain_liveness.h"

#include <deque>
#include <mutex>
#include <unordered_map>

namespace ce::swapchain_liveness {
namespace {

constexpr size_t kMaxNotes = 64;

std::mutex& LedgerLock() {
    static std::mutex lock;
    return lock;
}

std::unordered_map<const void*, LivenessNote>& Notes() {
    static std::unordered_map<const void*, LivenessNote> notes;
    return notes;
}

// Insertion order, so the ledger evicts the oldest observation once it is full.
std::deque<const void*>& InsertionOrder() {
    static std::deque<const void*> order;
    return order;
}

void ForgetLocked(const void* chain) {
    if (Notes().erase(chain) == 0) {
        return;
    }
    auto& order = InsertionOrder();
    for (auto it = order.begin(); it != order.end(); ++it) {
        if (*it == chain) {
            order.erase(it);
            return;
        }
    }
}

}  // namespace

void NoteCeReleasedLastOwnedReference(const void* chain, unsigned long residualRefs) {
    if (!chain) {
        return;
    }

    std::lock_guard<std::mutex> guard(LedgerLock());
    ForgetLocked(chain);

    LivenessNote note;
    note.known = true;
    note.ceReleasedLastOwnedReference = true;
    note.residualRefsAtCeRelease = residualRefs;
    Notes()[chain] = note;
    InsertionOrder().push_back(chain);

    while (InsertionOrder().size() > kMaxNotes) {
        const void* oldest = InsertionOrder().front();
        InsertionOrder().pop_front();
        Notes().erase(oldest);
    }
}

void ForgetNote(const void* chain) {
    if (!chain) {
        return;
    }

    std::lock_guard<std::mutex> guard(LedgerLock());
    ForgetLocked(chain);
}

LivenessNote Query(const void* chain) {
    if (!chain) {
        return LivenessNote{};
    }

    std::lock_guard<std::mutex> guard(LedgerLock());
    const auto it = Notes().find(chain);
    if (it == Notes().end()) {
        return LivenessNote{};
    }
    return it->second;
}

size_t NoteCount() {
    std::lock_guard<std::mutex> guard(LedgerLock());
    return Notes().size();
}

size_t MaxNotes() {
    return kMaxNotes;
}

void ResetForTesting() {
    std::lock_guard<std::mutex> guard(LedgerLock());
    Notes().clear();
    InsertionOrder().clear();
}

}  // namespace ce::swapchain_liveness
