#pragma once

#include "config.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace ce::live_stream {

inline constexpr std::string_view kInvalidTarget = "captureengine-live-invalid:";

enum class Service {
    kYouTube,
    kTwitch,
    kCustom,
    kInvalid,
};

struct ProfileSettings {
    Service service = Service::kInvalid;
    std::string destination;
    int videoBitrateKbps = 0;
    int audioBitrateKbps = 0;
    int maximumFps = 60;
    int bFrames = 0;
};

Service ParseService(std::string_view value);
const char* ServiceName(Service service);

bool IsLiveStreamTarget(std::string_view value);
bool IsValidLiveStreamTarget(std::string_view value);
bool BuildLiveStreamTarget(std::string_view server, std::string_view streamKey, std::string* target);

bool ApplyProfile(const ProfileSettings& settings, VideoConfig* video, std::vector<AudioConfig>* audioSources,
                  std::string* error);

OverlayNotificationType SelectOutputCompletionNotification(bool liveStream, bool canceled, bool outputPublished,
                                                            bool degraded);

bool IsSuccessfulSession(bool discardRequested, bool terminalFailure, int trailerResult, int closeResult,
                         int64_t finalDurationUs, uint64_t writtenVideoPackets);

size_t ComputeQueueBudgetBytes(int64_t totalBitsPerSecond);
bool ExceedsQueueBudget(size_t queuedBytes, size_t packetBytes, size_t limitBytes);

}  // namespace ce::live_stream
