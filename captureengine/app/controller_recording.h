#pragma once

#include "recording_session.h"

ce::controller::RecordingSnapshot ControllerRecordingSnapshot();
ce::controller::CommandOutcome StartControllerRecording(RecordingStartIntent intent, const char* reason);
ce::controller::CommandOutcome ToggleControllerRecording(RecordingStartIntent intent, const char* reason);
bool StopControllerRecording(const char* reason);
void ReconcileControllerRecording(bool includeChildHealth);
void PrepareRecordingDiagnosticIdentity();
void ShutdownControllerRecording();

// The session exists for ControllerMain, including frontend initialization and shutdown.
class ControllerRecordingSessionScope {
public:
    ControllerRecordingSessionScope();
    ~ControllerRecordingSessionScope();
    ControllerRecordingSessionScope(const ControllerRecordingSessionScope&) = delete;
    ControllerRecordingSessionScope& operator=(const ControllerRecordingSessionScope&) = delete;

private:
    ce::controller::RecordingSession session_;
};
