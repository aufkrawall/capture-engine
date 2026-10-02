#include "common/ipc/elevation_protocol.h"
#include <cstring>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size < sizeof(ce::elevation::Header))
        return 0;
    ce::elevation::Header header;
    std::memcpy(&header, data, sizeof(header));
    ce::elevation::ValidateHeader(header, false);
    ce::elevation::ValidateHeader(header, true);
    ce::elevation::ValidateRequest(header, data + sizeof(header), size - sizeof(header));
    return 0;
}
