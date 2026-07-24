// libFuzzer harness for IPC message validation.
//
// Target: ValidateProcessMessage(), the single trust boundary every pipe reader
// funnels through (common/process_ipc.cpp). It covers the bytesRead bounds check,
// magic/version/headerSize/totalSize consistency, sender identity, nonce, sequence
// policy, and both payload validators. Do not target the file-local ValidatePayload
// helpers directly: they live in an anonymous namespace and are deliberately not
// part of the public surface.
//
// Built and run by build.py --run-fuzz; see llm-wiki/fuzzing.md.

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "../../common/process_ipc.h"

namespace {

// Pull a value off the tail of the buffer so the leading bytes stay byte-aligned
// with the ProcessMessage image the fuzzer is mutating.
template <typename T>
T TakeTrailing(const uint8_t* data, size_t size, size_t indexFromEnd, T fallback) {
    const size_t needed = sizeof(T) * (indexFromEnd + 1);
    if (size < sizeof(ProcessMessage) + needed) {
        return fallback;
    }
    T value{};
    memcpy(&value, data + size - needed, sizeof(T));
    return value;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size < sizeof(ProcessMessage)) {
        return 0;
    }

    ProcessMessage message;
    memcpy(&message, data, sizeof(ProcessMessage));

    // Derive the peer expectations from trailing bytes so the fuzzer explores both
    // the matching-identity path (deep validation) and the mismatching-identity path
    // (early rejection), instead of only ever failing on the first comparison.
    const auto expectedKind = static_cast<ProcessMessageKind>(TakeTrailing<uint16_t>(data, size, 0, 0));
    const auto expectedSenderMode = static_cast<ProcessMode>(TakeTrailing<uint32_t>(data, size, 1, 0));
    const uint32_t expectedSenderPid = TakeTrailing<uint32_t>(data, size, 2, 0);
    const uint64_t expectedSequence = TakeTrailing<uint64_t>(data, size, 3, 0);
    const bool requireExactSequence = (TakeTrailing<uint8_t>(data, size, 4, 0) & 1u) != 0u;

    // Alternate between the peer's own nonce (accepting path) and a foreign one.
    ProcessChannelNonce expectedNonce = message.nonce;
    if ((TakeTrailing<uint8_t>(data, size, 5, 0) & 1u) != 0u) {
        expectedNonce[0] = static_cast<uint8_t>(expectedNonce[0] + 1u);
    }

    // bytesRead is attacker-influenced in the real reader, so fuzz it too rather
    // than always passing a self-consistent sizeof(ProcessMessage).
    const size_t bytesRead =
        static_cast<size_t>(TakeTrailing<uint16_t>(data, size, 6, 0)) % (sizeof(ProcessMessage) + 1);

    (void)ValidateProcessMessage(message, bytesRead, expectedKind, expectedSenderMode, expectedSenderPid, expectedNonce,
                                 expectedSequence, requireExactSequence);

    // Also exercise the self-consistent length, which is the common live case.
    (void)ValidateProcessMessage(message, message.totalSize, expectedKind, expectedSenderMode, expectedSenderPid,
                                 expectedNonce, expectedSequence, requireExactSequence);

    return 0;
}
