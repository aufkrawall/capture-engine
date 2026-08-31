// libFuzzer harness for the bounded LibreHardwareMonitor bridge protocol.
//
// Target: ParseBridgeMessage(), the controller-side boundary for every line
// emitted by the separately loaded managed sensor process.
//
// Built and run by build.py --run-fuzz; see llm-wiki/fuzzing.md.

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "../../captureengine/sensor_plugin.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    const auto* text = size == 0 ? "" : reinterpret_cast<const char*>(data);
    std::string_view line(text, size);
    ce::hardware_sensors::BridgeMessage message;
    (void)ce::hardware_sensors::ParseBridgeMessage(line, message);

    // Pipe framing removes CRLF before parsing. Exercise that live path as well
    // while preserving the raw input above for embedded/trailing-byte coverage.
    if (!line.empty() && line.back() == '\n') {
        line.remove_suffix(1);
        if (!line.empty() && line.back() == '\r')
            line.remove_suffix(1);
        (void)ce::hardware_sensors::ParseBridgeMessage(line, message);
    }
    return 0;
}
