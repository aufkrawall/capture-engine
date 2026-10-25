#include "precompiled_shaders.h"

#include <cstring>

// Generated before any media engine compile (tools/build/build_shaders.py).
#include "ce_precompiled_shaders.h"

namespace ce::precompiled_shaders {

namespace {

constexpr uint64_t kFnvOffset = 0xcbf29ce484222325ull;
constexpr uint64_t kFnvPrime = 0x100000001b3ull;

uint64_t Mix(uint64_t hash, const void* data, size_t size) {
    const auto* bytes = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= kFnvPrime;
    }
    return hash;
}

uint64_t MixString(uint64_t hash, const char* text) {
    hash = Mix(hash, text, text ? std::strlen(text) : 0);
    const uint8_t terminator = 0;
    return Mix(hash, &terminator, 1);
}

}  // namespace

// Mirrors request_key() in tools/build/build_shaders.py.
uint64_t RequestKey(std::string_view source, const char* entry, const char* target, uint32_t flags) {
    uint64_t hash = Mix(kFnvOffset, source.data(), source.size());
    const uint8_t terminator = 0;
    hash = Mix(hash, &terminator, 1);
    hash = MixString(hash, entry);
    hash = MixString(hash, target);
    const uint8_t flagBytes[4] = {static_cast<uint8_t>(flags), static_cast<uint8_t>(flags >> 8),
                                  static_cast<uint8_t>(flags >> 16), static_cast<uint8_t>(flags >> 24)};
    return Mix(hash, flagBytes, sizeof(flagBytes));
}

Bytecode Find(std::string_view source, const char* entry, const char* target, uint32_t flags) {
    if (!entry || !target)
        return {};
    const uint64_t key = RequestKey(source, entry, target, flags);
    for (size_t i = 0; i < generated::kEntryCount; ++i) {
        const generated::Entry& candidate = generated::kEntries[i];
        // The key decides; length, names and flags guard against a collision.
        if (candidate.key == key && candidate.sourceLength == source.size() && candidate.flags == flags &&
            std::strcmp(candidate.entry, entry) == 0 && std::strcmp(candidate.target, target) == 0) {
            return {candidate.data, candidate.size};
        }
    }
    return {};
}

size_t EntryCount() {
    return generated::kEntryCount;
}

}  // namespace ce::precompiled_shaders
