#pragma once

#include "audio_capture.h"

#include <algorithm>

#include <exception>

#include <iostream>

#include <utility>

#include "audio_time_utils.h"

#include "mediaengine.h"  // For DLL_Log

#define REFTIMES_PER_SEC 10000000

#define REFTIMES_PER_MILLISEC 10000

    // NOLINTNEXTLINE(bugprone-throwing-static-initialization) - __uuidof resolves to a compile-time constant GUID
inline const CLSID audio_capture_CLSID_MMDeviceEnumerator = __uuidof(MMDeviceEnumerator);

    // NOLINTNEXTLINE(bugprone-throwing-static-initialization) - __uuidof resolves to a compile-time constant GUID
inline const IID audio_capture_IID_IMMDeviceEnumerator = __uuidof(IMMDeviceEnumerator);

// IEEE Float subformat GUID: {00000003-0000-0010-8000-00aa00389b71}
inline bool IsIEEEFloat(const GUID& g) {
    return g.Data1 == 0x00000003 && g.Data2 == 0x0000 && g.Data3 == 0x0010 && g.Data4[0] == 0x80 &&
           g.Data4[1] == 0x00 && g.Data4[2] == 0x00 && g.Data4[3] == 0xaa && g.Data4[4] == 0x00 && g.Data4[5] == 0x38 &&
           g.Data4[6] == 0x9b && g.Data4[7] == 0x71;
}

inline uint32_t ExtractChannelMask(const WAVEFORMATEX* format) {
    if (!format) {
        return 0;
    }
    if (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
        format->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
        const auto* wfex = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(format);
        return static_cast<uint32_t>(wfex->dwChannelMask);
    }
    if (format->nChannels == 1) {
        return SPEAKER_FRONT_CENTER;
    }
    if (format->nChannels == 2) {
        return SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
    }
    return 0;
}

inline void FillPacketFormatFromWaveFormat(const WAVEFORMATEX* format, AudioPacket* packet) {
    if (!format || !packet) {
        return;
    }

    packet->channels = format->nChannels;
    // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
    packet->sampleRate = format->nSamplesPerSec;
    packet->bitsPerSample = format->wBitsPerSample;
    packet->blockAlign = format->nBlockAlign;
    packet->validBitsPerSample = 0;
    packet->channelMask = ExtractChannelMask(format);
    packet->isFloat = format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
    packet->devicePosition = 0;
    packet->qpcPosition = 0;
    packet->rawQpcPosition = 0;
    packet->streamLatency = 0;

    if (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
        format->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
        const auto* wfex = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(format);
        if (IsIEEEFloat(wfex->SubFormat)) {
            packet->isFloat = true;
        }
        packet->validBitsPerSample = wfex->Samples.wValidBitsPerSample;
    }
}
