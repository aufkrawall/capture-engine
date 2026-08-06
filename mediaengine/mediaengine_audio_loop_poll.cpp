#include "mediaengine_internal.h"

bool MediaEngine::SourceParticipatesInSharedStartupRebase(size_t srcIdx) const {
    if (srcIdx >= audioSources.size()) {
        return false;
    }
    const auto& src = audioSources[srcIdx];
    const bool hasPhysicalOrRoutedCapture =
        src.capture || src.appCapture || src.captureFanoutOwnerIndex < audioSources.size();
    if (!src.sawSyncPendingPackets || !src.sharedEncoderPtr || !hasPhysicalOrRoutedCapture) {
        return false;
    }
    return src.sourceType != AudioConfig::Microphone;
}

bool MediaEngine::TrySelectSharedStartupRebase(AudioLoopState& s, bool finalStopDrain) {
    if (s.sharedStartupRebaseOffsetSamples >= 0) {
        return true;
    }

    size_t participants = 0;
    size_t readyParticipants = 0;
    int64_t earliestPacketStartSamples = std::numeric_limits<int64_t>::max();
    for (size_t i = 0; i < audioSources.size(); ++i) {
        if (!SourceParticipatesInSharedStartupRebase(i)) {
            continue;
        }
        ++participants;
        if (!s.deferredFirstTimelinePacketValid[i] && s.sourceTimestamps[i] == 0) {
            continue;
        }
        ++readyParticipants;
        if (s.deferredFirstTimelinePacketValid[i]) {
            earliestPacketStartSamples =
                std::min<int64_t>(earliestPacketStartSamples, s.deferredFirstTimelinePacketStartSamples[i]);
        }
    }

    if (participants == 0) {
        s.sharedStartupRebaseOffsetSamples = 0;
        return true;
    }
    if (readyParticipants < participants && !finalStopDrain) {
        return false;
    }
    if (readyParticipants < participants) {
        DLL_Log("[AudioLoop] Final stop bypassing absent shared-startup participants: ready=%zu total=%zu",
                readyParticipants, participants);
    }
    if (earliestPacketStartSamples == std::numeric_limits<int64_t>::max()) {
        s.sharedStartupRebaseOffsetSamples = 0;
        DLL_Log("[AudioLoop] Shared startup rebase disabled: participants=%zu had no deferred QPC baseline",
                participants);
        return true;
    }

    s.sharedStartupRebaseOffsetSamples = ce::audio::ComputeSharedStartupFirstPacketRebaseOffset(
        earliestPacketStartSamples, AudioLoopState::kStartupFirstPacketGapCapSamples,
        AudioLoopState::kStartupFirstPacketRebaseThresholdSamples);
    DLL_Log(
        "[AudioLoop] Shared startup rebase selected offset=%lld samples earliest=%lld cap=%lld "
        "participants=%zu",
        (long long)s.sharedStartupRebaseOffsetSamples, (long long)earliestPacketStartSamples,
        (long long)AudioLoopState::kStartupFirstPacketGapCapSamples, participants);
    return true;
}

#include "mediaengine_internal.h"

bool MediaEngine::AudioLoopPollSource(AudioLoopState& s, size_t srcIdx) {
    auto& sourceTimestamps = s.sourceTimestamps;
    auto& sourceLoggedPreStartDrop = s.sourceLoggedPreStartDrop;
    auto& sourceLastPackets = s.sourceLastPackets;
    auto& deferredFirstTimelinePackets = s.deferredFirstTimelinePackets;
    auto& deferredFirstTimelinePacketValid = s.deferredFirstTimelinePacketValid;
    auto& deferredFirstTimelinePacketStartSamples = s.deferredFirstTimelinePacketStartSamples;
    auto& sourceCaptureEpochs = s.sourceCaptureEpochs;
    auto& captureFanoutPacketCounts = s.captureFanoutPacketCounts;
    auto& batchedPreStartDiscardCounts = s.batchedPreStartDiscardCounts;
    auto& pendingEpochPackets = s.pendingEpochPackets;
    auto& captureFanoutQueues = s.captureFanoutQueues;
    auto& gotAnyPacket = s.gotAnyPacket;
    auto& audioEqualizationDelaySamples = s.audioEqualizationDelaySamples;
    auto& sharedStartupRebaseOffsetSamples = s.sharedStartupRebaseOffsetSamples;

    auto& src = audioSources[srcIdx];
    auto& packet = s.packet;
        bool gotPacket = false;
        bool gotDeferredFirstTimelinePacket = false;

        const uint64_t requestedEpochReset = src.epochResetRequested->load(std::memory_order_acquire);
        const uint64_t acknowledgedEpochReset = src.epochResetAcknowledged->load(std::memory_order_acquire);
        if (requestedEpochReset != 0 && requestedEpochReset != acknowledgedEpochReset) {
            // The pull owner is still encoding the previous epoch's ring/sync/post tail.
            // Do not poll new capture data into route state until it acknowledges the reset.
            return false;
        }

        if (!pendingEpochPackets[srcIdx].empty()) {
            packet = std::move(pendingEpochPackets[srcIdx].front());
            pendingEpochPackets[srcIdx].pop_front();
            gotPacket = true;
            DLL_Log(
                "[AudioEpoch] Consuming deferred epoch record after pull acknowledgement src=%zu track=%d "
                "epoch=%llu remaining=%zu",
                srcIdx, src.track, static_cast<unsigned long long>(packet.captureEpoch),
                pendingEpochPackets[srcIdx].size());
        } else if (deferredFirstTimelinePacketValid[srcIdx]) {
            const bool finalStopDrain = audioStopDrainRequested.load(std::memory_order_acquire);
            if (!TrySelectSharedStartupRebase(s, finalStopDrain)) {
                gotAnyPacket = true;
                return false;
            }
            packet = std::move(deferredFirstTimelinePackets[srcIdx]);
            deferredFirstTimelinePacketValid[srcIdx] = false;
            gotPacket = true;
            gotDeferredFirstTimelinePacket = true;
        } else {
            bool packetCameFromPhysicalCapture = false;
            if (!captureFanoutQueues[srcIdx].empty()) {
                packet = std::move(captureFanoutQueues[srcIdx].front());
                captureFanoutQueues[srcIdx].pop_front();
                gotPacket = true;
            } else {
                // Poll the one physical capture that owns this route group.
                if (src.appCapture) {
                    gotPacket = src.appCapture->GetNextPacket(packet);
                    if (!gotPacket && src.appCapture->HasIntegrityFailure() &&
                        !processLoopbackIntegrityFailureSignaled) {
                        processLoopbackIntegrityFailureSignaled = true;
                        const uint32_t status = src.appCapture->GetTransportStatus();
                        DLL_Log(
                            "[AudioLoop] FATAL: app-audio transport integrity failure src=%zu track=%d "
                            "status=%u; requesting a failed recording stop",
                            srcIdx, src.track, status);
                        if (sharedMemLayout) {
                            sharedMemLayout->runtimeState.recordingFailureCode.store(
                                static_cast<uint32_t>(RecordingFailureCode::ProcessLoopbackTransportIntegrity),
                                std::memory_order_release);
                            sharedMemLayout->runtimeState.cmdStopRecording.store(true,
                                                                                 std::memory_order_release);
                        }
                    }
                } else if (src.capture) {

                    gotPacket = src.capture->GetNextPacket(packet);
                }
                packetCameFromPhysicalCapture = gotPacket;
            }

            // During WGC startup, process-loopback queues can contain over a
            // second of packets preceding the shared video anchor. Removing
            // only one stale packet per outer pass lets live audio remain
            // hundreds of milliseconds behind. Drain a bounded batch before
            // doing any resampling, while preserving evidence needed by the
            // startup-rebase policy.
            constexpr size_t kMaxPreStartDiscardBatchPackets = 256;
            size_t batchedDiscards = 0;
            int64_t startQPC = recordingStartSystemQPCMs.load(std::memory_order_acquire);
            while (packetCameFromPhysicalCapture && gotPacket && !packet.data.empty() &&
                   sourceTimestamps[srcIdx] == 0 && startQPC != 0 && packet.timestamp < (startQPC - 5) &&
                   batchedDiscards < kMaxPreStartDiscardBatchPackets) {
                const bool rememberPreStartPacket = ce::audio::ShouldRememberPreStartPacketForAppBootstrap(
                    src.sourceType == AudioConfig::AppAudio, true, packet.timestamp, startQPC);
                for (size_t routeIdx = 0; routeIdx < audioSources.size(); ++routeIdx) {
                    if (audioSources[routeIdx].captureFanoutOwnerIndex == srcIdx && rememberPreStartPacket) {
                        audioSources[routeIdx].sawSyncPendingPackets = true;
                    }
                }
                ++batchedDiscards;
                gotAnyPacket = true;
                packet = {};
                gotPacket = src.appCapture ? src.appCapture->GetNextPacket(packet)
                                           : (src.capture ? src.capture->GetNextPacket(packet) : false);
            }
            if (batchedDiscards > 0) {
                batchedPreStartDiscardCounts[srcIdx] += batchedDiscards;
                if (!sourceLoggedPreStartDrop[srcIdx] || batchedDiscards == kMaxPreStartDiscardBatchPackets) {
                    DLL_Log(
                        "[AudioLoop] Batched pre-start discard owner=%zu packets=%zu total=%llu "
                        "start=%lld nextPacket=%lld fanoutRoutes=%zu%s",
                        srcIdx, batchedDiscards,
                        static_cast<unsigned long long>(batchedPreStartDiscardCounts[srcIdx]), startQPC,
                        gotPacket ? packet.timestamp : 0,
                        static_cast<size_t>(std::count_if(audioSources.begin(), audioSources.end(),
                                                          [srcIdx](const AudioSource& route) {
                                                              return route.captureFanoutOwnerIndex == srcIdx;
                                                          })),
                        batchedDiscards == kMaxPreStartDiscardBatchPackets ? " batch_limit" : "");
                    sourceLoggedPreStartDrop[srcIdx] = true;
                }
            }

            const bool packetStillPreStart = gotPacket && !packet.data.empty() &&
                                             sourceTimestamps[srcIdx] == 0 && startQPC != 0 &&
                                             packet.timestamp < (startQPC - 5);
            if (packetCameFromPhysicalCapture && gotPacket && !packetStillPreStart &&
                src.captureFanoutOwnerIndex == srcIdx) {
                size_t followerCount = 0;
                for (size_t routeIdx = srcIdx + 1; routeIdx < audioSources.size(); ++routeIdx) {
                    if (audioSources[routeIdx].captureFanoutOwnerIndex != srcIdx) {
                        continue;  // not a follower of this owner; keep scanning
                    }
                    captureFanoutQueues[routeIdx].push_back(packet);
                    ++followerCount;
                }
                if (followerCount > 0) {
                    ++captureFanoutPacketCounts[srcIdx];
                    if (captureFanoutPacketCounts[srcIdx] == 1 ||
                        captureFanoutPacketCounts[srcIdx] % 1000 == 0) {
                        DLL_Log(
                            "[AudioRoute] Fanned packet owner=%zu routes=%zu packet=%llu "
                            "pendingFollowerMax=%zu qpc=%llu",
                            srcIdx, followerCount + 1,
                            static_cast<unsigned long long>(captureFanoutPacketCounts[srcIdx]),
                            [&]() {
                                size_t maximum = 0;
                                for (size_t routeIdx = 0; routeIdx < captureFanoutQueues.size(); ++routeIdx) {
                                    if (audioSources[routeIdx].captureFanoutOwnerIndex == srcIdx) {
                                        maximum = std::max(maximum, captureFanoutQueues[routeIdx].size());
                                    }
                                }
                                return maximum;
                            }(),
                            static_cast<unsigned long long>(packet.qpcPosition));
                    }
                }
            }
        }

        if (gotPacket && packet.recordType == AudioPacketRecordType::EpochStart) {
            src.sawCaptureEpoch = true;
            const uint64_t previousCaptureEpoch = sourceCaptureEpochs[srcIdx];
            if (packet.captureEpoch != 0 && previousCaptureEpoch == 0) {
                sourceCaptureEpochs[srcIdx] = packet.captureEpoch;
            }
            if (ce::audio::IsAudioCaptureEpochTransition(previousCaptureEpoch, packet.captureEpoch)) {
                if (!FlushCaptureResamplerForEpoch(src, srcIdx, previousCaptureEpoch, packet.captureEpoch)) {
                    pendingEpochPackets[srcIdx].push_front(std::move(packet));
                    return false;
                }
                src.epochResetRequested->store(packet.captureEpoch, std::memory_order_release);
                audioDrainCv.notify_all();
                gotAnyPacket = true;
                DLL_Log(
                    "[AudioEpoch] Ordered epoch start requested pull reset src=%zu track=%d type=%d "
                    "epoch=%llu->%llu ringBuffered=%zu",
                    srcIdx, src.track, static_cast<int>(src.sourceType),
                    static_cast<unsigned long long>(previousCaptureEpoch),
                    static_cast<unsigned long long>(packet.captureEpoch),
                    src.ringBuffer ? src.ringBuffer->GetAvailable() /
                                         static_cast<size_t>(std::clamp(src.mixChannels, 1, 8))
                                   : 0);
                return false;
            }
            gotAnyPacket = true;
            DLL_Log(
                "[AudioRoute] Ordered epoch start reached src=%zu track=%d type=%d epoch=%llu previous=%llu "
                "ringBuffered=%zu",
                srcIdx, src.track, static_cast<int>(src.sourceType),
                static_cast<unsigned long long>(packet.captureEpoch),
                static_cast<unsigned long long>(previousCaptureEpoch),
                src.ringBuffer
                    ? src.ringBuffer->GetAvailable() / static_cast<size_t>(std::clamp(src.mixChannels, 1, 8))
                    : 0);
            return false;
        }

        if (gotPacket && (packet.recordType == AudioPacketRecordType::EndOfStream || packet.endOfStream)) {
            if (src.appCaptureRouteEnded) {
                src.appCaptureRouteEnded->store(true, std::memory_order_release);
            }
            src.timelineValid = true;
            gotAnyPacket = true;
            DLL_Log(
                "[AudioRoute] Ordered app capture end reached src=%zu track=%d process=%s epoch=%llu "
                "buffered=%zu; future source gaps are timeline silence until a new epoch live-joins",
                srcIdx, src.track, src.config.processName.empty() ? "<none>" : src.config.processName.c_str(),
                static_cast<unsigned long long>(packet.captureEpoch),
                src.ringBuffer
                    ? src.ringBuffer->GetAvailable() / static_cast<size_t>(std::clamp(src.mixChannels, 1, 8))
                    : 0);
            return false;
        }

        if (gotPacket && !packet.data.empty()) {
            const bool resumedAfterEnd =
                src.appCaptureRouteEnded && src.appCaptureRouteEnded->load(std::memory_order_acquire);
            const uint64_t previousCaptureEpoch = sourceCaptureEpochs[srcIdx];
            const bool captureEpochTransition =
                ce::audio::IsAudioCaptureEpochTransition(previousCaptureEpoch, packet.captureEpoch);
            if (captureEpochTransition) {
                const uint64_t requested = src.epochResetRequested->load(std::memory_order_acquire);
                const uint64_t acknowledged = src.epochResetAcknowledged->load(std::memory_order_acquire);
                if (requested != packet.captureEpoch) {
                    if (!FlushCaptureResamplerForEpoch(src, srcIdx, previousCaptureEpoch,
                                                       packet.captureEpoch)) {
                        pendingEpochPackets[srcIdx].push_front(std::move(packet));
                        return false;
                    }
                    src.epochResetRequested->store(packet.captureEpoch, std::memory_order_release);
                    audioDrainCv.notify_all();
                    const uint64_t deferredCaptureEpoch = packet.captureEpoch;
                    pendingEpochPackets[srcIdx].push_back(std::move(packet));
                    gotAnyPacket = true;
                    DLL_Log(
                        "[AudioEpoch] Data arrived without a preceding transition marker; requested "
                        "pull reset and deferred data src=%zu track=%d type=%d epoch=%llu->%llu",
                        srcIdx, src.track, static_cast<int>(src.sourceType),
                        static_cast<unsigned long long>(previousCaptureEpoch),
                        static_cast<unsigned long long>(deferredCaptureEpoch));
                    return false;
                }
                if (acknowledged != packet.captureEpoch) {
                    pendingEpochPackets[srcIdx].push_back(std::move(packet));
                    gotAnyPacket = true;
                    return false;
                }
                // A name-monitored app may exit and return under a new PID many minutes later;
                // fatal/silent-stall recovery also creates a fresh process-loopback stream. The
                // new stream's first QPC is valid in the recording domain, but it is not contiguous
                // with this route's old source-local write cursor. Treat it as a fresh late source
                // so the existing live-join policy overlaps already-encoded silence instead of
                // materializing the absence as a multi-minute ring-buffer gap.
                src.resampler.reset();
                sourceTimestamps[srcIdx] = 0;
                sourceLastPackets[srcIdx] = {};
                deferredFirstTimelinePacketValid[srcIdx] = false;
                const auto epochReadiness =
                    ce::audio::ComputeAudioCaptureEpochReadinessReset(src.bootstrapComplete);
                src.timelineValid = epochReadiness.timelineValid;
                src.isPrimed = epochReadiness.isPrimed;
                src.bootstrapComplete = epochReadiness.bootstrapComplete;
                src.sawSyncPendingPackets = false;
                src.startupRealAudioSeen = false;
                src.startupSyntheticRingSamples = 0;
                src.startupGapProtectionSamples = 0;
                src.packetBoundaryFadeInSamplesRemaining = 0;
                DLL_Log(
                    "[AudioEpoch] Capture owner accepted acknowledged transition src=%zu track=%d type=%d "
                    "process=%s epoch=%llu->%llu requested=%llu acknowledged=%llu trackCursor=%lld "
                    "sourceCursor=%llu bootstrapPreserved=%d; epoch-local capture state reset for live rejoin",
                    srcIdx, src.track, static_cast<int>(src.sourceType),
                    src.config.processName.empty() ? "<none>" : src.config.processName.c_str(),
                    static_cast<unsigned long long>(previousCaptureEpoch),
                    static_cast<unsigned long long>(packet.captureEpoch),
                    static_cast<unsigned long long>(requested), static_cast<unsigned long long>(acknowledged),
                    static_cast<long long>(trackTimelineSamples[src.track]),
                    static_cast<unsigned long long>(src.qpcAlignedWrittenSamples),
                    src.bootstrapComplete ? 1 : 0);
            }
            if (packet.captureEpoch != 0) {
                sourceCaptureEpochs[srcIdx] = packet.captureEpoch;
            }
            if (resumedAfterEnd) {
                src.appCaptureRouteEnded->store(false, std::memory_order_release);
                DLL_Log(
                    "[AudioRoute] App capture resumed after ordered end src=%zu track=%d process=%s "
                    "epoch=%llu; old tail preserved before live rejoin",
                    srcIdx, src.track,
                    src.config.processName.empty() ? "<none>" : src.config.processName.c_str(),
                    static_cast<unsigned long long>(packet.captureEpoch));
            }
            // First captured audio for this source: grow its ring buffer to
            // full CFR capacity (deferred for app-audio sources whose target
            // process may never run). The buffer is still empty at this point,
            // so the in-place grow is lossless, and AudioLoop is the only
            // thread touching this buffer, so it is race-free.
            if (src.ringBuffer && src.ringBuffer->GetCapacity() < src.fullRingBufferCapacityFloats) {
                if (src.ringBuffer->EnsureCapacity(src.fullRingBufferCapacityFloats)) {
                    DLL_Log(
                        "[AudioLoop] Grew ring buffer for src %d to full capacity %zu floats on first capture",
                        (int)srcIdx, src.fullRingBufferCapacityFloats);
                }
            }
            int64_t startQPC = recordingStartSystemQPCMs.load(std::memory_order_acquire);
            if (startQPC != 0 && sourceTimestamps[srcIdx] == 0 && packet.timestamp < (startQPC - 5)) {
                const bool rememberPreStartPacket = ce::audio::ShouldRememberPreStartPacketForAppBootstrap(
                    src.sourceType == AudioConfig::AppAudio, sourceTimestamps[srcIdx] == 0, packet.timestamp,
                    startQPC);
                if (rememberPreStartPacket) {
                    src.sawSyncPendingPackets = true;
                }
                if (!sourceLoggedPreStartDrop[srcIdx]) {
                    DLL_Log(
                        "[AudioLoop] Discarding pre-start packet src=%d packet=%lld start=%lld "
                        "appStartupEvidence=%d",
                        (int)srcIdx, packet.timestamp, startQPC, rememberPreStartPacket ? 1 : 0);
                    sourceLoggedPreStartDrop[srcIdx] = true;
                }
                return false;
            }

            const int64_t startQpc100nsForFirstPacket =
                recordingStartSystemQpc100ns.load(std::memory_order_acquire);
            if (!gotDeferredFirstTimelinePacket && sourceTimestamps[srcIdx] == 0 &&
                SourceParticipatesInSharedStartupRebase(srcIdx) && sharedStartupRebaseOffsetSamples < 0 &&
                startQpc100nsForFirstPacket > 0 &&
                packet.qpcPosition >= static_cast<uint64_t>(startQpc100nsForFirstPacket)) {
                const uint64_t packetStartDelta100ns =
                    packet.qpcPosition - static_cast<uint64_t>(startQpc100nsForFirstPacket);
                deferredFirstTimelinePacketStartSamples[srcIdx] =
                    static_cast<int64_t>(ce::audio::HundredNanosecondsToSamples(packetStartDelta100ns, 48000)) +
                    audioEqualizationDelaySamples[srcIdx];
                deferredFirstTimelinePackets[srcIdx] = std::move(packet);
                deferredFirstTimelinePacketValid[srcIdx] = true;
                gotAnyPacket = true;
                TrySelectSharedStartupRebase(s, audioStopDrainRequested.load(std::memory_order_acquire));
                return false;
            }
        }
    return gotPacket && !packet.data.empty();
}
