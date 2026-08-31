#include "live_stream_config.h"

#include <algorithm>
#include <cctype>
#include <limits>

namespace ce::live_stream {
namespace {

std::string Lowercase(std::string_view value) {
    std::string result(value);
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return result;
}

bool StartsWithInsensitive(std::string_view value, std::string_view prefix) {
    if (value.size() < prefix.size())
        return false;
    for (size_t i = 0; i < prefix.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(value[i])) !=
            std::tolower(static_cast<unsigned char>(prefix[i]))) {
            return false;
        }
    }
    return true;
}

bool HasOnlySafeUrlCharacters(std::string_view value) {
    if (value.empty() || value.size() > 4095)
        return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char ch) { return ch > 0x20 && ch < 0x7f; });
}

bool SelectH264Encoder(const std::string& configured, std::string* selected) {
    if (!selected)
        return false;

    const std::string lower = Lowercase(configured);
    static constexpr std::string_view backends[] = {"_nvenc", "_amf", "_qsv", "_mf"};
    for (const std::string_view backend : backends) {
        if (lower.size() >= backend.size() &&
            lower.compare(lower.size() - backend.size(), backend.size(), backend.data(), backend.size()) == 0) {
            *selected = "h264" + std::string(backend);
            return true;
        }
    }
    return false;
}

int DefaultVideoBitrateKbps(Service service) {
    return service == Service::kYouTube ? 12000 : 6000;
}

int DefaultAudioBitrateKbps(Service service) {
    return service == Service::kYouTube ? 128 : 160;
}

}  // namespace

Service ParseService(std::string_view value) {
    const std::string lower = Lowercase(value);
    if (lower == "youtube")
        return Service::kYouTube;
    if (lower == "twitch")
        return Service::kTwitch;
    if (lower == "custom")
        return Service::kCustom;
    return Service::kInvalid;
}

const char* ServiceName(Service service) {
    switch (service) {
        case Service::kYouTube:
            return "youtube";
        case Service::kTwitch:
            return "twitch";
        case Service::kCustom:
            return "custom";
        case Service::kInvalid:
        default:
            return "invalid";
    }
}

bool IsLiveStreamTarget(std::string_view value) {
    return value == kInvalidTarget || StartsWithInsensitive(value, "rtmp://") ||
           StartsWithInsensitive(value, "rtmps://");
}

bool IsValidLiveStreamTarget(std::string_view value) {
    if (!HasOnlySafeUrlCharacters(value))
        return false;

    size_t schemeLength = 0;
    if (StartsWithInsensitive(value, "rtmp://")) {
        schemeLength = 7;
    } else if (StartsWithInsensitive(value, "rtmps://")) {
        schemeLength = 8;
    } else {
        return false;
    }

    const size_t pathSeparator = value.find('/', schemeLength);
    return pathSeparator != std::string_view::npos && pathSeparator > schemeLength &&
           pathSeparator + 1 < value.size();
}

bool BuildLiveStreamTarget(std::string_view server, std::string_view streamKey, std::string* target) {
    if (!target || !HasOnlySafeUrlCharacters(server) || !HasOnlySafeUrlCharacters(streamKey))
        return false;
    if (!StartsWithInsensitive(server, "rtmp://") && !StartsWithInsensitive(server, "rtmps://"))
        return false;

    std::string combined(server);
    if (combined.back() != '/')
        combined.push_back('/');
    combined.append(streamKey);
    if (!IsValidLiveStreamTarget(combined))
        return false;
    *target = std::move(combined);
    return true;
}

bool ApplyProfile(const ProfileSettings& settings, VideoConfig* video, std::vector<AudioConfig>* audioSources,
                  std::string* error) {
    if (!video || !audioSources) {
        if (error)
            *error = "missing configuration output";
        return false;
    }
    if (settings.service == Service::kInvalid || !IsValidLiveStreamTarget(settings.destination)) {
        if (error)
            *error = "invalid service or destination";
        return false;
    }

    std::string h264Encoder;
    if (!SelectH264Encoder(video->encoder, &h264Encoder)) {
        if (error)
            *error = "the selected encoder backend has no supported H.264 live-streaming variant";
        return false;
    }

    const int videoBitrateKbps =
        settings.videoBitrateKbps > 0 ? settings.videoBitrateKbps : DefaultVideoBitrateKbps(settings.service);
    int audioBitrateKbps =
        settings.audioBitrateKbps > 0 ? settings.audioBitrateKbps : DefaultAudioBitrateKbps(settings.service);
    if (settings.service == Service::kTwitch)
        audioBitrateKbps = std::min(audioBitrateKbps, 160);

    std::string destination = settings.destination;
    if (StartsWithInsensitive(destination, "rtmps://")) {
        destination.replace(0, 8, "rtmps://");
    } else {
        destination.replace(0, 7, "rtmp://");
    }

    video->encoder = std::move(h264Encoder);
    video->fps = std::clamp(video->fps, 1, std::clamp(settings.maximumFps, 1, 60));
    video->container = "flv";
    video->outputDir = std::move(destination);
    video->rateControl = "CBR";
    video->bitrate = std::to_string(videoBitrateKbps) + "Kbps";
    video->maxBitrate = video->bitrate;
    video->bufferSize = video->bitrate;
    video->keyframeInterval = 2;
    video->preset = "p1";
    video->tuning = "ull";
    video->multipass = "disabled";
    video->splitEncode = "0";
    video->profile = "high";
    video->lookahead = "off";
    video->spatialAq = false;
    video->temporalAq = false;
    video->bFrames = std::clamp(settings.bFrames, 0, 2);
    video->bRefMode = "disabled";
    video->customOptions.clear();
    video->bitDepth = "8";
    video->colorSpace = "bt709";
    video->colorRange = "limited";
    video->chromaSubsampling = "420";
    video->useVFR = false;
    video->useVFR_AudioSync = false;

    video->amfUsage = "ultralowlatency";
    video->amfPreset = "speed";
    video->amfAsyncDepth = 2;
    video->amfPreencode = false;
    video->amfPreanalysis = false;
    video->amfLookahead = "off";
    video->amfSpatialAq = false;
    video->amfTemporalAq = false;
    video->amfHighMotionQualityBoost = false;
    video->amfBRefMode = "disabled";
    video->amfEnforceHrd = true;
    video->amfFillerData = true;

    video->qsvPreset = "veryfast";
    video->qsvAsyncDepth = 2;
    video->qsvLookahead = "off";
    video->qsvMbbRc = "disabled";
    video->qsvExtBrc = "disabled";
    video->qsvAdaptiveI = "disabled";
    video->qsvAdaptiveB = "disabled";
    video->qsvLowDelayBrc = "enabled";
    video->qsvScenario = "livestreaming";

    video->mfRateControl = "cbr";
    video->mfScenario = "live_streaming";
    video->mfLowLatency = true;

    for (AudioConfig& audio : *audioSources) {
        if (!audio.enabled)
            continue;
        audio.tracks = {1};
        audio.codec = "aac";
        audio.bitrate = audioBitrateKbps;
        audio.sampleRate = "48000";
        audio.bitDepth = "default";
        audio.downmix = true;
        audio.outputChannels = 2;
    }
    return true;
}

OverlayNotificationType SelectOutputCompletionNotification(bool liveStream, bool canceled, bool outputPublished,
                                                            bool degraded) {
    if (canceled)
        return OverlayNotificationType::RecordingCanceled;
    if (liveStream) {
        if (!outputPublished)
            return OverlayNotificationType::StreamingFailed;
        return degraded ? OverlayNotificationType::StreamingEndedDegraded : OverlayNotificationType::StreamingEnded;
    }
    if (!outputPublished)
        return OverlayNotificationType::RecordingFailed;
    return degraded ? OverlayNotificationType::RecordingSavedDegraded : OverlayNotificationType::RecordingSaved;
}

bool IsSuccessfulSession(bool discardRequested, bool terminalFailure, int trailerResult, int closeResult,
                         int64_t finalDurationUs, uint64_t writtenVideoPackets) {
    return !discardRequested && !terminalFailure && trailerResult >= 0 && closeResult >= 0 && finalDurationUs > 0 &&
           writtenVideoPackets > 0;
}

size_t ComputeQueueBudgetBytes(int64_t totalBitsPerSecond) {
    constexpr size_t kMinimumBytes = 1024 * 1024;
    constexpr size_t kMaximumBytes = 64 * 1024 * 1024;
    if (totalBitsPerSecond <= 0)
        return kMinimumBytes;

    const uint64_t unsignedRate = static_cast<uint64_t>(totalBitsPerSecond);
    const uint64_t twoSecondsBytes = unsignedRate > std::numeric_limits<uint64_t>::max() / 2
                                         ? std::numeric_limits<uint64_t>::max()
                                         : (unsignedRate * 2) / 8;
    return static_cast<size_t>(std::clamp<uint64_t>(twoSecondsBytes, kMinimumBytes, kMaximumBytes));
}

bool ExceedsQueueBudget(size_t queuedBytes, size_t packetBytes, size_t limitBytes) {
    return queuedBytes > limitBytes || packetBytes > limitBytes - std::min(queuedBytes, limitBytes);
}

}  // namespace ce::live_stream
