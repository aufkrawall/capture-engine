// libFuzzer harness for IPC message deserialization
// Build: clang++ -fsanitize=fuzzer,address -o fuzz_ipc
//   tests/fuzz_ipc_deserialize.cpp common/process_ipc.cpp -I common -I ..
// Run: ./fuzz_ipc -max_total_time=300 tests/fuzz_corpus/ipc/

#include <cstdint>
#include <cstring>

#include "../common/process_ipc.h"

// Forward-declare the internal validation function from process_ipc.cpp
namespace ce::ipc {
bool ValidatePayload(const ProcessMessage& msg, size_t bytesAvailable);
bool ValidateOpcodePayload(const ProcessMessage& msg, size_t bytesAvailable);
}

// Simulates receiving a ProcessMessage over a pipe and running validation.
// The fuzzer feeds raw bytes; we interpret them as a ProcessMessage with
// available bytes = data size.
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size < sizeof(ProcessMessage))
        return 0;

    ProcessMessage msg;
    memcpy(&msg, data, sizeof(ProcessMessage));

    // Run both validation paths
    (void)ce::ipc::ValidatePayload(msg, size);
    (void)ce::ipc::ValidateOpcodePayload(msg, size);

    return 0;
}
