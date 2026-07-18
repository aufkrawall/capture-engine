#pragma once

#include "shared_defs.h"

namespace ce::recording_indicator {

enum class State : uint8_t {
    Idle = 0,
    StartingVideo,
    StartingAudio,
    RecordingVideo,
    RecordingAudio,
};

inline State SelectState(bool recordingActive, bool recordingAudioOnly, RecordingStartIntent startIntent) {
    if (recordingActive) {
        return recordingAudioOnly ? State::RecordingAudio : State::RecordingVideo;
    }
    if (startIntent == RecordingStartIntent::AudioOnly) {
        return State::StartingAudio;
    }
    if (startIntent == RecordingStartIntent::Video) {
        return State::StartingVideo;
    }
    return State::Idle;
}

inline bool IsStarting(State state) {
    return state == State::StartingVideo || state == State::StartingAudio;
}

inline bool IsRecording(State state) {
    return state == State::RecordingVideo || state == State::RecordingAudio;
}

inline bool IsVisible(State state) {
    return state != State::Idle;
}

inline const char* GetStartingText(State state) {
    return state == State::StartingAudio ? "STARTING AUDIO..." : "STARTING RECORDING...";
}

}  // namespace ce::recording_indicator
