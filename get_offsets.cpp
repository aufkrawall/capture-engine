#include <iostream>
#include <cstddef>
#include "common/shared_defs.h"

int main() {
    std::cout << "OFFSET_RUNTIME_STATE=" << offsetof(SharedMemoryLayout, runtimeState) << std::endl;
    std::cout << "OFFSET_CMD_START=" << (offsetof(SharedMemoryLayout, runtimeState) + offsetof(CaptureState, cmdStartRecording)) << std::endl;
    std::cout << "OFFSET_CMD_STOP=" << (offsetof(SharedMemoryLayout, runtimeState) + offsetof(CaptureState, cmdStopRecording)) << std::endl;
    std::cout << "OFFSET_IS_RECORDING=" << (offsetof(SharedMemoryLayout, runtimeState) + offsetof(CaptureState, isRecording)) << std::endl;
    std::cout << "SIZEOF_SHARED_MEMORY=" << sizeof(SharedMemoryLayout) << std::endl;
    return 0;
}
