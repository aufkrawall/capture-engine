#pragma once

// Package-private ABI types shared by mediaengine.dll and its loader (DR-12, DR-42). The build identity handshake
// (common/platform/build_identity_check.h) refuses a module of another build before any of these is used.

#include <stdint.h>

// Process default log sink (MediaEngine_SetLogCallback).
typedef void (*LogCallback)(const char* msg);

// One media pipeline. Zero is never a valid handle. A handle is valid from MediaEngine_Create until
// MediaEngine_Destroy; any later use is refused.
typedef uintptr_t MediaEngineHandle;

// Per-pipeline log sink (DR-40): receives the DLL's log lines of the calls made on that pipeline and of the threads it starts.
typedef void (*MediaEngineLogFn)(void* context, const char* msg);

struct MediaEngineCreateDesc {
    uint32_t structSize;           // must equal sizeof(MediaEngineCreateDesc)
    MediaEngineLogFn logCallback;  // optional; null sends the pipeline's lines to the process sink
    void* logContext;              // passed to logCallback
    // Optional UTF-16 path of the package's role host (captureengine.exe), which runs per-app audio workers (DR-43).
    // Null or empty: the executable of the process that runs the pipeline.
    const wchar_t* roleHostPath;
};
