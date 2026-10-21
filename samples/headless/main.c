/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 aufkrawall */
/*
 * A headless client of the CaptureEngine runtime (cengine.dll), written against the SDK only
 * (sdk/include, sdk/lib). Deploy it into the CaptureEngine folder, next to cengine.dll:
 *
 *   headless.exe [--record-ms N] [--video] [--rounds N] [--isolated-helpers]
 *
 * Each round creates the runtime, waits until it is ready, optionally records for N ms (audio only unless
 * --video), waits until the recording's file is finalized, then shuts the runtime down and destroys it.
 * --isolated-helpers sets CE_FEATURE_ISOLATED_HELPERS: the helper roles run as separate captureengine.exe
 * processes, so a crash in capture or encoding ends only that process. Without it the runtime chooses the
 * transport (sdk/README.md, Concepts).
 * Exit status: 0 success, 1 failure, 2 another CaptureEngine runtime already runs in this Windows session.
 */
#include <cengine/cengine.h>

#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { kReadyTimeoutMs = 60000, kCommandTimeoutMs = 30000, kFinalizeTimeoutMs = 300000, kShutdownTimeoutMs = 300000 };

typedef struct Expectation {
    int32_t type;         /* CE_EVENT_* to wait for */
    ce_request_t request; /* COMMAND_COMPLETED: the request; 0 otherwise */
    int32_t state;        /* RUNTIME_STATE / RECORDING_STATE: the state; 0 = any */
    uint64_t recording;   /* RECORDING_FINALIZED: the recording id; 0 = any */
    ce_status_t status;   /* out: the matching event's status */
    uint64_t recordingId; /* out: the recording id the matching event names */
    int hasOutput;        /* out: RECORDING_FINALIZED named an output file */
} Expectation;

static void PrintEvent(const ce_event_t* event) {
    printf("event #%llu type=%d request=%llu status=%d", (unsigned long long)event->sequence, (int)event->type,
           (unsigned long long)event->request, (int)event->status);
    switch (event->type) {
        case CE_EVENT_RUNTIME_STATE:
            printf(" runtime_state=%d", (int)event->data.runtime.state);
            break;
        case CE_EVENT_RECORDING_STATE:
            printf(" recording=%llu state=%d failure=%u", (unsigned long long)event->data.recording.recording_id,
                   (int)event->data.recording.state, (unsigned)event->data.recording.failure);
            break;
        case CE_EVENT_RECORDING_FINALIZED:
            printf(" recording=%llu path=%s", (unsigned long long)event->data.finalized.recording_id,
                   event->data.finalized.output_path ? event->data.finalized.output_path : "(none)");
            break;
        default:
            break;
    }
    if (event->message)
        printf(" message=\"%s\"", event->message);
    printf("\n");
}

static int Matches(const ce_event_t* event, Expectation* expected) {
    if (event->type != expected->type)
        return 0;
    switch (event->type) {
        case CE_EVENT_COMMAND_COMPLETED:
            return event->request == expected->request;
        case CE_EVENT_RUNTIME_STATE:
            return expected->state == 0 || event->data.runtime.state == expected->state ||
                   event->data.runtime.state == CE_RUNTIME_FAILED;
        case CE_EVENT_RECORDING_STATE:
            if (expected->state != 0 && event->data.recording.state != expected->state)
                return 0;
            expected->recordingId = event->data.recording.recording_id;
            return 1;
        case CE_EVENT_RECORDING_FINALIZED:
            if (expected->recording != 0 && event->data.finalized.recording_id != expected->recording)
                return 0;
            expected->recordingId = event->data.finalized.recording_id;
            expected->hasOutput = event->data.finalized.output_path != NULL;
            return 1;
        default:
            return 1;
    }
}

/* Prints every event until one matches `expected` or timeout_ms passed. A NULL `expected` only drains events
 * for timeout_ms (the recording time). Returns 1 when matched. */
static int WaitFor(ce_runtime_t* rt, uint32_t timeoutMs, Expectation* expected) {
    const ULONGLONG deadline = GetTickCount64() + timeoutMs;
    for (;;) {
        const ULONGLONG now = GetTickCount64();
        if (now >= deadline)
            return 0;
        const ce_event_t* event = NULL;
        const ce_status_t status = ce_runtime_next_event(rt, (uint32_t)(deadline - now), &event);
        if (status == CE_E_TIMEOUT)
            return 0;
        if (status != CE_OK) {
            fprintf(stderr, "ce_runtime_next_event: %s\n", ce_status_string(status));
            return 0;
        }
        PrintEvent(event);
        int matched = 0;
        if (expected && Matches(event, expected)) {
            expected->status = event->status;
            if (event->type == CE_EVENT_RUNTIME_STATE && event->data.runtime.state == CE_RUNTIME_FAILED &&
                expected->state != CE_RUNTIME_FAILED)
                expected->status = CE_E_INVALID_STATE;
            matched = 1;
        }
        ce_runtime_release_event(rt, event);
        if (matched)
            return 1;
    }
}

static int Record(ce_runtime_t* rt, int32_t mode, uint32_t recordMs) {
    ce_request_t request = 0;
    ce_status_t status = ce_recording_start(rt, mode, "headless sample", &request);
    if (status != CE_OK) {
        fprintf(stderr, "ce_recording_start: %s\n", ce_status_string(status));
        return 0;
    }
    Expectation started = {CE_EVENT_COMMAND_COMPLETED, request, 0, 0, CE_OK, 0, 0};
    if (!WaitFor(rt, kCommandTimeoutMs, &started) || started.status != CE_OK) {
        fprintf(stderr, "the recording did not start: %s\n", ce_status_string(started.status));
        return 0;
    }
    Expectation live = {CE_EVENT_RECORDING_STATE, 0, CE_RECORDING_LIVE, 0, CE_OK, 0, 0};
    if (!WaitFor(rt, kCommandTimeoutMs, &live)) {
        fprintf(stderr, "the recording did not go live\n");
        return 0;
    }
    WaitFor(rt, recordMs, NULL);
    status = ce_recording_stop(rt, "headless sample", &request);
    if (status != CE_OK) {
        fprintf(stderr, "ce_recording_stop: %s\n", ce_status_string(status));
        return 0;
    }
    Expectation stopped = {CE_EVENT_COMMAND_COMPLETED, request, 0, 0, CE_OK, 0, 0};
    if (!WaitFor(rt, kCommandTimeoutMs, &stopped) || stopped.status != CE_OK) {
        fprintf(stderr, "the recording did not stop: %s\n", ce_status_string(stopped.status));
        return 0;
    }
    Expectation finalized = {CE_EVENT_RECORDING_FINALIZED, 0, 0, live.recordingId, CE_OK, 0, 0};
    if (!WaitFor(rt, kFinalizeTimeoutMs, &finalized) || finalized.status != CE_OK) {
        fprintf(stderr, "recording %llu was not finalized: %s\n", (unsigned long long)live.recordingId,
                ce_status_string(finalized.status));
        return 0;
    }
    /* CE_OK also covers a canceled recording and a live stream; a recording to a file must name its file. */
    if (!finalized.hasOutput) {
        fprintf(stderr, "recording %llu finalized without an output file\n", (unsigned long long)live.recordingId);
        return 0;
    }
    return 1;
}

static int RunRound(int32_t mode, uint32_t recordMs, int isolated) {
    ce_runtime_desc_t desc;
    ce_status_t status = ce_runtime_desc_init(&desc, (uint32_t)sizeof(desc), CE_API_VERSION);
    if (status != CE_OK) {
        fprintf(stderr, "ce_runtime_desc_init: %s\n", ce_status_string(status));
        return 1;
    }
    desc.client_name = "headless-sample";
    desc.features = isolated ? CE_FEATURE_ISOLATED_HELPERS : 0;
    ce_runtime_t* rt = NULL;
    status = ce_runtime_create(&desc, &rt);
    if (status == CE_E_BUSY) {
        fprintf(stderr, "another CaptureEngine runtime already runs in this Windows session\n");
        return 2;
    }
    if (status != CE_OK) {
        fprintf(stderr, "ce_runtime_create: %s\n", ce_status_string(status));
        return 1;
    }
    int ok = 1;
    Expectation ready = {CE_EVENT_RUNTIME_STATE, 0, CE_RUNTIME_READY, 0, CE_OK, 0, 0};
    if (!WaitFor(rt, kReadyTimeoutMs, &ready) || ready.status != CE_OK) {
        fprintf(stderr, "the runtime did not become ready\n");
        ok = 0;
    }
    if (ok && recordMs > 0)
        ok = Record(rt, mode, recordMs);
    status = ce_runtime_shutdown(rt, kShutdownTimeoutMs);
    if (status != CE_OK) {
        /* The runtime keeps its helpers until they exit; a client may wait again or leave the handle. */
        fprintf(stderr, "ce_runtime_shutdown: %s\n", ce_status_string(status));
        return 1;
    }
    status = ce_runtime_destroy(rt); /* frees the events still queued */
    if (status != CE_OK) {
        fprintf(stderr, "ce_runtime_destroy: %s\n", ce_status_string(status));
        return 1;
    }
    return ok ? 0 : 1;
}

int main(int argc, char** argv) {
    uint32_t recordMs = 0;
    int32_t mode = CE_MODE_AUDIO_ONLY;
    int rounds = 1;
    int isolated = 0;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--record-ms") == 0 && i + 1 < argc)
            recordMs = (uint32_t)strtoul(argv[++i], NULL, 10);
        else if (strcmp(argv[i], "--video") == 0)
            mode = CE_MODE_VIDEO;
        else if (strcmp(argv[i], "--rounds") == 0 && i + 1 < argc)
            rounds = atoi(argv[++i]);
        else if (strcmp(argv[i], "--isolated-helpers") == 0)
            isolated = 1;
        else {
            fprintf(stderr, "usage: %s [--record-ms N] [--video] [--rounds N] [--isolated-helpers]\n", argv[0]);
            return 1;
        }
    }
    if ((ce_api_version() >> 16) != CE_API_VERSION_MAJOR) {
        fprintf(stderr, "cengine.dll implements API %u.%u, this client needs %u.x\n", ce_api_version() >> 16,
                ce_api_version() & 0xFFFFu, CE_API_VERSION_MAJOR);
        return 1;
    }
    printf("cengine %s, API %u.%u\n", ce_version_string(), ce_api_version() >> 16, ce_api_version() & 0xFFFFu);
    printf("helper roles: %s\n", isolated ? "isolated (CE_FEATURE_ISOLATED_HELPERS)" : "default");
    for (int round = 0; round < rounds; ++round) {
        const int result = RunRound(mode, recordMs, isolated);
        if (result != 0)
            return result;
    }
    return 0;
}
