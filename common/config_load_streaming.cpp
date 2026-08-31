#include "config_load_internal.h"

#include "live_stream_config.h"

void ApplyStreamingSettings(ConfigReader& reader, AppConfig& config) {
    if (!reader.GetBool("Streaming", "enabled", false))
        return;

    const ce::live_stream::Service service =
        ce::live_stream::ParseService(reader.GetStr("Streaming", "service", "twitch"));
    std::string destination = reader.GetStr("Streaming", "url", "");
    const bool destinationValid = destination.empty()
                                      ? ce::live_stream::BuildLiveStreamTarget(
                                            reader.GetStr("Streaming", "server", ""),
                                            reader.GetStr("Streaming", "stream_key", ""), &destination)
                                      : ce::live_stream::IsValidLiveStreamTarget(destination);

    ce::live_stream::ProfileSettings settings;
    settings.service = service;
    settings.destination = destination;
    settings.videoBitrateKbps = reader.GetBoundedInt("Streaming", "video_bitrate", 0, 0, 100000);
    settings.audioBitrateKbps = reader.GetBoundedInt("Streaming", "audio_bitrate", 0, 0, 320);
    settings.maximumFps = reader.GetBoundedInt("Streaming", "max_fps", 60, 1, 60);
    settings.bFrames = reader.GetBoundedInt("Streaming", "b_frames", 0, 0, 2);

    std::string error;
    if (!destinationValid || !ce::live_stream::ApplyProfile(settings, &config.video, &config.audioSources, &error)) {
        config.video.container = "flv";
        config.video.outputDir = std::string(ce::live_stream::kInvalidTarget);
        LogWarn("[LiveStream] Configuration rejected without exposing endpoint credentials: %s",
                error.empty() ? "invalid destination" : error.c_str());
        return;
    }

    int audioBitrate = 0;
    for (const AudioConfig& audio : config.audioSources) {
        if (audio.enabled) {
            audioBitrate = audio.bitrate;
            break;
        }
    }
    LogInfo(
        "[LiveStream] Enabled service=%s endpoint=<redacted> encoder=%s fps=%d videoBitrate=%s "
        "audioBitrate=%dKbps bFrames=%d",
        ce::live_stream::ServiceName(service), config.video.encoder.c_str(), config.video.fps,
        config.video.bitrate.c_str(), audioBitrate, config.video.bFrames);
}
