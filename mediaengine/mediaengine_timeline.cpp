#include "mediaengine_internal.h"
#include "audio_fault_accounting.h"


int64_t MediaEngine::GetVideoElapsedMs() const {


        return videoElapsedMs.load();

}


bool MediaEngine::SessionUsesVfr() const {


        return timingModeFrozenForSession ? sessionUseVfr : config.video.useVFR;

}


bool MediaEngine::SessionUsesScreenGrab() const {


        return activeScreenGrab;

}


int64_t MediaEngine::GetCommittedVideoElapsedUs(int64_t fallbackElapsedUs) const {


        if (SessionUsesVfr() || !videoEnc) {
            return fallbackElapsedUs;
        }

        const int64_t encodedDurationUs = videoEnc->GetEncodedDurationUs();
        return encodedDurationUs > 0 ? encodedDurationUs : fallbackElapsedUs;

}


void MediaEngine::CommitVideoElapsedUs(SourceTimelineState& timelineState,  int64_t elapsedUs) {


        if (elapsedUs < 0) {
            return;
        }

        timelineState.lastElapsedUs = std::max(timelineState.lastElapsedUs, elapsedUs);
        this->videoElapsedMs.store(elapsedUs / 1000);
        lastVideoFrameMs = elapsedUs / 1000;

}


int64_t MediaEngine::GetLastVideoEncodeTimeUs() const {


        if (videoEnc)
            return videoEnc->GetLastFrameEncodeTimeUs();
        return 0;

}


int64_t MediaEngine::GetLastFrameFenceWaitUs() const {


        if (videoEnc)
            return videoEnc->GetLastFrameFenceWaitUs();
        return 0;

}


bool MediaEngine::WasLastFrameDeferred() const {


        if (videoEnc)
            return videoEnc->WasLastFrameDeferred();
        return false;

}


int32_t MediaEngine::QueryInjectFrameCopyCompletion(HANDLE fenceHandle, uint64_t fenceValue, uint32_t sourcePid,
                                                    uint32_t transportGeneration) const {


        if (videoEnc)
            return videoEnc->QueryInjectFrameCopyCompletion(fenceHandle, fenceValue, sourcePid, transportGeneration);
        return -1;

}


void MediaEngine::SetInjectTransportGeneration(uint32_t transportGeneration) {


        if (videoEnc)
            videoEnc->SetInjectTransportGeneration(transportGeneration);

}


bool MediaEngine::CanRepeatLastFrame() {


        std::lock_guard<std::recursive_mutex> lock(muxMutex);
        return videoEnc && recording && firstVideoFrameCommitted && videoEnc->CanRepeatLastFrame();

}


void MediaEngine::ResetRepeatFrameCache() {


        std::lock_guard<std::recursive_mutex> lock(muxMutex);
        if (videoEnc) {
            videoEnc->ResetRepeatFrameCache();
        }

}


int64_t MediaEngine::GetLastVideoFenceWaitUs() const {


        if (videoEnc)
            return videoEnc->GetLastFrameFenceWaitUs();
        return 0;

}


void MediaEngine::ResetAudioPullStateForRecording() {


        // The audio worker keeps running across this reset (SyncAudioToFirstVideoFrame
        // only waits for its acknowledgement) and reads both cursor containers under
        // the leaf lock; rebuilding them without it raced a std::map clear against
        // the worker's lookups.
        std::unique_lock<std::mutex> cursorLock(ce::audio::g_audioCursorSyncMutex);
        encodedSamplesPerSource.assign(audioSources.size(), 0);
        trackTimelineSamples.clear();
        trackRealMixedSamples.clear();
        trackFullSilenceSamples.clear();
        trackPartialSilenceSamples.clear();
        trackWasSilent.clear();
        trackFadeInSamplesRemaining.clear();
        trackFadeInLengthSamples.clear();
        trackSilentSamples.clear();
        trackSilentChunks.clear();
        trackSilenceTransitions.clear();
        trackLastSilenceLogTick.clear();
        trackBootstrapComplete.clear();
        trackFirstPullAfterBootstrap.clear();
        trackBootstrapWaitLogCounters.clear();
        cachedTrackToSources.clear();
        for (size_t i = 0; i < audioSources.size(); ++i) {
            auto& src = audioSources[i];
            if (!src.ringBuffer || !src.sharedEncoderPtr) {
                continue;
            }
            cachedTrackToSources[src.track].push_back(i);
            trackTimelineSamples[src.track] = 0;
            trackRealMixedSamples[src.track] = 0;
            trackFullSilenceSamples[src.track] = 0;
            trackPartialSilenceSamples[src.track] = 0;
        }
        cursorLock.unlock();

        audioIngestWorstHeadroomSamples.store(kNoAudioIngestHeadroom, std::memory_order_relaxed);
        audioIngestReservoir = ce::audio::AudioIngestReservoirState{};
        audioIngestReservoirExtraMs = 0;
        audioIngestReservoirEvalTick = 0;
        audioIngestReservoirLogTick = 0;
        audioIngestReservoirLoggedMs = 0;
        audioIngestReservoirPeakMs = 0;

        warpCount = 0;
        dropLogCounter = 0;
        driftLogCounter = 0;
        trackSyncCheckCounters.clear();
        injectFrameLogCount = 0;
        screengrabFrameLogCount = 0;
        silenceLogCounter = 0;
        mixLogCounter = 0;

}
