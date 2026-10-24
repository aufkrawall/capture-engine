#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace ce::elevation {

inline constexpr wchar_t kServiceName[] = L"CaptureEngineElevation";
inline constexpr wchar_t kPipeName[] = L"\\\\.\\pipe\\CaptureEngineElevation.v1";
// Rights granted to the installing user on the service-owned ETW session. ProcessTrace needs
// the session to be queryable as well as the real-time access right; no control rights.
inline constexpr unsigned long kConsumerTraceRights = 0x0001 /*WMIGUID_QUERY*/ | 0x0004 /*WMIGUID_NOTIFICATION*/ |
                                                      0x0400 /*TRACELOG_ACCESS_REALTIME*/;
inline constexpr wchar_t kTraceName[] = L"CE_DisplayTiming_Elevation";
inline constexpr uint32_t kProtocolMagic = 0x31454543;
inline constexpr uint32_t kProtocolVersion = 1;
inline constexpr size_t kMaximumPayload = 4096;
inline constexpr size_t kMetricCount = 9;

// AcquireTrace leases the shared session with the display-timing providers on;
// AcquireProcessTrace leases it for process discovery alone. Appended values
// only: an older service rejects them (and drops the connection), so a client
// sends them only to a service advertising kProcessOnlyTraceCapability.
enum class Operation : uint32_t {
    Hello = 1,
    SubscribeSensors,
    SampleSensors,
    AcquireTrace,
    ReleaseTrace,
    Status,
    AcquireProcessTrace,
    ReleaseProcessTrace,
    // Recreates the installation's missing config.ini from the service's own template. No payload: the folder is the
    // one the service runs from, never a path the client names.
    RestoreConfig
};

struct Header {
    uint32_t magic = kProtocolMagic;
    uint32_t version = kProtocolVersion;
    Operation operation = Operation::Hello;
    uint32_t size = 0;
    uint32_t error = 0;
    uint32_t sequence = 0;
};

inline constexpr uint32_t kSensorCapability = 1;
inline constexpr uint32_t kDisplayTraceCapability = 2;
// Reported by Status only while the leased session's process provider is enabled.
// Older services retain the same wire layout and simply omit this bit.
inline constexpr uint32_t kProcessStartTraceCapability = 4;
// The service understands AcquireProcessTrace/ReleaseProcessTrace.
inline constexpr uint32_t kProcessOnlyTraceCapability = 8;
// The service understands RestoreConfig. An older service drops a connection that sends an operation it does not know.
inline constexpr uint32_t kConfigRestoreCapability = 16;
struct Capabilities {
    uint32_t protocolVersion = kProtocolVersion;
    uint32_t features =
        kSensorCapability | kDisplayTraceCapability | kProcessOnlyTraceCapability | kConfigRestoreCapability;
    uint32_t serverPid = 0;
    uint32_t controllerPid = 0;
};

struct Hello {
    uint32_t controllerPid = 0;
    uint32_t sessionId = 0;
};

struct SensorRequest {
    uint32_t pollIntervalMs = 1000;
    std::array<std::array<char, 256>, kMetricCount> selectors{};
};

struct SensorSample {
    uint64_t sampledTickMs = 0;
    std::array<char, 2048> line{};
};

inline bool IsOperation(Operation operation) {
    return operation >= Operation::Hello && operation <= Operation::RestoreConfig;
}

inline size_t RequestSize(Operation operation) {
    switch (operation) {
        case Operation::Hello:
            return sizeof(Hello);
        case Operation::SubscribeSensors:
            return sizeof(SensorRequest);
        default:
            return 0;
    }
}

inline bool ValidateHeader(const Header& header, bool response) {
    return header.magic == kProtocolMagic && header.version == kProtocolVersion && IsOperation(header.operation) &&
           header.sequence != 0 && header.size <= kMaximumPayload &&
           (response || (header.error == 0 && header.size == RequestSize(header.operation)));
}

inline bool ValidateSelector(const std::array<char, 256>& selector) {
    const auto* end = static_cast<const char*>(std::memchr(selector.data(), 0, selector.size()));
    if (!end)
        return false;
    const std::string_view value(selector.data(), static_cast<size_t>(end - selector.data()));
    if (value == "auto" || value == "off")
        return true;
    if (value.size() < 2 || value.front() != '/')
        return false;
    for (unsigned char character : value) {
        if (!((character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
              (character >= '0' && character <= '9') || character == '/' || character == '_' || character == '-' ||
              character == '.'))
            return false;
    }
    return true;
}

inline bool ValidateRequest(const Header& header, const void* payload, size_t size) {
    if (!ValidateHeader(header, false) || size != header.size || (size != 0 && !payload))
        return false;
    if (header.operation == Operation::Hello) {
        Hello hello;
        std::memcpy(&hello, payload, sizeof(hello));
        return hello.controllerPid != 0;
    }
    if (header.operation == Operation::SubscribeSensors) {
        SensorRequest request;
        std::memcpy(&request, payload, sizeof(request));
        if (request.pollIntervalMs < 250 || request.pollIntervalMs > 10000)
            return false;
        for (const auto& selector : request.selectors) {
            if (!ValidateSelector(selector))
                return false;
        }
    }
    return true;
}

static_assert(sizeof(Header) == 24);
static_assert(sizeof(SensorRequest) <= kMaximumPayload);

}  // namespace ce::elevation
