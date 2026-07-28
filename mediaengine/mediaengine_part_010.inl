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
                                continue;
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
                            continue;
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
                        continue;
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
                    continue;
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
                    continue;
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
                                continue;
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
                            continue;
                        }
                        if (acknowledged != packet.captureEpoch) {
                            pendingEpochPackets[srcIdx].push_back(std::move(packet));
                            gotAnyPacket = true;
                            continue;
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
                        src.timelineValid = false;
                        src.isPrimed = false;
                        src.bootstrapComplete = false;
                        src.sawSyncPendingPackets = false;
                        src.startupRealAudioSeen = false;
                        src.startupSyntheticRingSamples = 0;
                        src.startupGapProtectionSamples = 0;
                        src.packetBoundaryFadeInSamplesRemaining = 0;
                        DLL_Log(
                            "[AudioEpoch] Capture owner accepted acknowledged transition src=%zu track=%d type=%d "
                            "process=%s epoch=%llu->%llu requested=%llu acknowledged=%llu trackCursor=%lld "
                            "sourceCursor=%llu; capture state reset for live rejoin",
                            srcIdx, src.track, static_cast<int>(src.sourceType),
                            src.config.processName.empty() ? "<none>" : src.config.processName.c_str(),
                            static_cast<unsigned long long>(previousCaptureEpoch),
                            static_cast<unsigned long long>(packet.captureEpoch),
                            static_cast<unsigned long long>(requested), static_cast<unsigned long long>(acknowledged),
                            static_cast<long long>(trackTimelineSamples[src.track]),
                            static_cast<unsigned long long>(src.qpcAlignedWrittenSamples));
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
                        continue;
                    }

                    const int64_t startQpc100nsForFirstPacket =
                        recordingStartSystemQpc100ns.load(std::memory_order_acquire);
                    if (!gotDeferredFirstTimelinePacket && sourceTimestamps[srcIdx] == 0 &&
                        sourceParticipatesInSharedStartupRebase(srcIdx) && sharedStartupRebaseOffsetSamples < 0 &&
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
                        trySelectSharedStartupRebase(audioStopDrainRequested.load(std::memory_order_acquire));
                        continue;
                    }

                    gotAnyPacket = true;
                    packetCount++;
                    sourceLastPackets[srcIdx] = packet;
                    lastPacketTime[srcIdx] = now;

                    if (packetCount <= 3 || packetCount % 1000 == 0) {
                        DLL_Log("AudioLoop: Packet #%d src=%d track=%d, size=%d", packetCount, (int)srcIdx, src.track,
                                (int)packet.data.size());
                    }

                    // Standardize each source to the resolved track layout at 48kHz
                    // float. This preserves multichannel tracks while still giving
                    // the mixer a single format per track.
                    if (!src.resampler) {
                        src.resampler = std::make_unique<AudioResampler>();
                    }

                    // Define target format for mixing (48kHz, Stereo, Float)
                    AudioResampler::OutputFormat targetFmt;
                    targetFmt.sampleRate = 48000;
                    targetFmt.channels = std::clamp(src.mixChannels, 1, 8);
                    targetFmt.sampleFmt = AV_SAMPLE_FMT_FLT;  // Packed float (interleaved)
                    targetFmt.channelMask = src.mixChannelMask;

                    // Define input format from packet
                    AudioResampler::InputFormat inputFmt;
                    inputFmt.sampleRate = packet.sampleRate;
                    inputFmt.channels = packet.channels;
                    inputFmt.bitsPerSample = packet.bitsPerSample;
                    inputFmt.validBitsPerSample =
                        packet.validBitsPerSample > 0 ? packet.validBitsPerSample : packet.bitsPerSample;
                    inputFmt.isFloat = packet.isFloat;
                    inputFmt.blockAlign = (packet.channels * packet.bitsPerSample) / 8;
                    inputFmt.channelMask = packet.channelMask;

                    // Initialize/Reinitialize resampler if needed (check all input format fields)
                    bool needReinit = !src.resampler->IsReady();
                    if (!needReinit) {
                        const auto& cur = src.resampler->GetInputFormat();
                        needReinit = (cur.sampleRate != inputFmt.sampleRate || cur.channels != inputFmt.channels ||
                                      cur.bitsPerSample != inputFmt.bitsPerSample || cur.isFloat != inputFmt.isFloat ||
                                      cur.channelMask != inputFmt.channelMask);
                    }
                    if (needReinit) {
                        src.resampler->Init(inputFmt, targetFmt);
                    }

                    uint8_t** resampledData = nullptr;
                    int outSamples = 0;

                    if (src.resampler->Process(packet.data.data(), (int)packet.data.size(), &resampledData,
                                               &outSamples)) {
                        if (outSamples > 0 && resampledData && resampledData[0]) {
                            // OBSOLETE: sourceBuffers accumulation removed to prevent memory
                            // leak size_t oldSize = sourceBuffers[srcIdx].size();
                            // sourceBuffers[srcIdx].resize(oldSize + numFloats);

                            // PULL MODEL (Phase 2): Write to Ring Buffer only
                            // SYNC FIX: Skip write if video thread is clearing buffers
                            if (src.ringBuffer && !audioSyncPending.load()) {
                                // Check for startup delay (Audio arriving late relative to
                                // Video Start)
                                int64_t startQPC = recordingStartSystemQPCMs.load();
                                if (startQPC != 0 && sourceTimestamps[srcIdx] == 0) {
                                    // First packet alignment using System QPC
                                    int64_t pTime = packet.timestamp;  // Packet comes with Absolute QPC MS
                                    int64_t diff = pTime - startQPC;
                                    src.alignedStartMs = diff;
                                    src.observedLateStartMs = std::max<int64_t>(diff, 0);
                                    src.hasAlignedStart = true;
                                    src.timelineValid = true;
                                    if (diff < -5) {
                                        DLL_Log("[AudioLoop] First packet leads recording start by %lld ms for src=%d",
                                                diff, (int)srcIdx);
                                    }
                                }

                                const bool firstTimelinePacket = (sourceTimestamps[srcIdx] == 0);
                                const bool firstPacketSawSyncPending = src.sawSyncPendingPackets;
                                sourceTimestamps[srcIdx] = packet.timestamp;
                                float* writeFloats = (float*)resampledData[0];
                                size_t writeSamples = static_cast<size_t>(outSamples);
                                const int64_t startQpc100ns =
                                    recordingStartSystemQpc100ns.load(std::memory_order_acquire);
                                if (startQpc100ns > 0 && packet.qpcPosition >= static_cast<uint64_t>(startQpc100ns)) {
                                    const uint64_t packetStartDelta100ns =
                                        packet.qpcPosition - static_cast<uint64_t>(startQpc100ns);
                                    int64_t packetStartSamples =
                                        static_cast<int64_t>(ce::audio::HundredNanosecondsToSamples(
                                            packetStartDelta100ns, targetFmt.sampleRate));
                                    // A/V equalization: delay this source to the common max latency
                                    // (leading silence inserted by the gap path below). 0 for the slowest.
                                    packetStartSamples += audioEqualizationDelaySamples[srcIdx];
                                    packetStartSamples = ce::audio::ApplyStartupPacketTimelineRebaseOffset(
                                        packetStartSamples, static_cast<int64_t>(src.startupRebasedGapSamples));
                                    packetStartSamples += src.timelineResyncOffsetSamples;
                                    const uint64_t ingestTick = GetTickCount64();
                                    src.lastRealPacketIngestTick = ingestTick;
                                    if (srcIdx < encodedSamplesPerSource.size()) {
                                        const int64_t encodedCursorSamples =
                                            ce::audio::ResolveSourceTimelineWriteCursor(
                                                src.qpcAlignedWrittenSamples, encodedSamplesPerSource[srcIdx]);
                                        // Ingest headroom: how far this packet's content still sits AHEAD of the
                                        // already-exported cursor. Negative means the consumer overran the capture
                                        // edge and the packet is about to be destroyed as timeline overlap. The
                                        // pull side turns the worst observation into extra scheduling lookahead.
                                        PublishAudioIngestHeadroom(packetStartSamples - encodedSamplesPerSource[srcIdx],
                                                                   targetFmt.sampleRate);
                                        if (encodedCursorSamples > static_cast<int64_t>(src.qpcAlignedWrittenSamples)) {
                                            const int64_t cursorAdvance =
                                                encodedCursorSamples -
                                                static_cast<int64_t>(src.qpcAlignedWrittenSamples);
                                            src.qpcAlignedWrittenSamples = static_cast<uint64_t>(encodedCursorSamples);
                                            if (cursorAdvance >= targetFmt.sampleRate / 200) {
                                                const uint64_t nowTick = GetTickCount64();
                                                if (nowTick - src.lastPacketTimelineAdjustWarnTick >= 1000) {
                                                    DLL_Log(
                                                        "[AudioLoop] Late source cursor advance src=%d advanced=%lld "
                                                        "samples encodedCursor=%lld before packet stitching",
                                                        (int)srcIdx, (long long)cursorAdvance,
                                                        (long long)encodedCursorSamples);
                                                    src.lastPacketTimelineAdjustWarnTick = nowTick;
                                                }
                                            }
                                        }
                                    }
                                    if (firstTimelinePacket) {
                                        // Keep a small preserved startup gap so the first live chunk does not
                                        // begin mid-waveform. A smaller 5ms cap caused large real-audio
                                        // startup backlogs and aggressive steady-state trim/correction.
                                        const bool usesSharedStartupRebase =
                                            sourceParticipatesInSharedStartupRebase(srcIdx) &&
                                            sharedStartupRebaseOffsetSamples >= 0;
                                        const int64_t requestedRebaseOffset =
                                            usesSharedStartupRebase ? sharedStartupRebaseOffsetSamples
                                                                    : ce::audio::ComputeStartupFirstPacketRebaseOffset(
                                                                          packetStartSamples, firstPacketSawSyncPending,
                                                                          kStartupFirstPacketGapCapSamples +
                                                                              audioEqualizationDelaySamples[srcIdx],
                                                                          kStartupFirstPacketRebaseThresholdSamples);
                                        const int64_t rebaseOffset =
                                            std::clamp<int64_t>(requestedRebaseOffset, 0, packetStartSamples);
                                        if (rebaseOffset > 0) {
                                            packetStartSamples -= rebaseOffset;
                                            src.startupRebasedGapSamples += static_cast<uint64_t>(rebaseOffset);
                                            DLL_Log(
                                                "[AudioLoop] Startup rebase src=%d suppressed %lld samples of "
                                                "first-packet gap (packetStart=%lld cap=%lld shared=%d)",
                                                (int)srcIdx, (long long)rebaseOffset,
                                                (long long)(packetStartSamples + rebaseOffset),
                                                (long long)kStartupFirstPacketGapCapSamples,
                                                usesSharedStartupRebase ? 1 : 0);
                                        }
                                        src.sawSyncPendingPackets = false;
                                    }
                                    const auto lateJoin = ce::audio::ComputeLateAppSourceJoin(
                                        src.sourceType == AudioConfig::AppAudio, firstTimelinePacket,
                                        firstPacketSawSyncPending, packetStartSamples, trackTimelineSamples[src.track],
                                        targetFmt.sampleRate / 2, targetFmt.sampleRate / 100);
                                    if (lateJoin.joinLive) {
                                        if (lateJoin.joinCursorSamples >
                                            static_cast<int64_t>(src.qpcAlignedWrittenSamples)) {
                                            src.qpcAlignedWrittenSamples =
                                                static_cast<uint64_t>(lateJoin.joinCursorSamples);
                                        }
                                        src.lateAppJoinSuppressedGapSamples +=
                                            static_cast<uint64_t>(lateJoin.suppressedGapSamples);
                                        src.lateAppJoinPreservedGapSamples +=
                                            static_cast<uint64_t>(lateJoin.preservedGapSamples);
                                        src.pendingStartupJoinFade = true;
                                        src.packetBoundaryFadeInSamplesRemaining =
                                            static_cast<int>(std::max<int64_t>(1, targetFmt.sampleRate / 750));
                                        DLL_Log(
                                            "[AudioLoop] Late app source live join src=%d track=%d process=%s "
                                            "packetStart=%lld trackCursor=%lld joinCursor=%lld "
                                            "suppressedGap=%lld preservedGap=%lld qpcStart=%llu",
                                            (int)srcIdx, src.track,
                                            src.config.processName.empty() ? "<none>" : src.config.processName.c_str(),
                                            (long long)packetStartSamples, (long long)trackTimelineSamples[src.track],
                                            (long long)lateJoin.joinCursorSamples,
                                            (long long)lateJoin.suppressedGapSamples,
                                            (long long)lateJoin.preservedGapSamples,
                                            (unsigned long long)packet.qpcPosition);
                                    }
                                    const auto timelineAdjustment =
                                        ce::audio::ComputeStartupAwarePacketTimelineAdjustment(
                                            packetStartSamples, static_cast<int64_t>(src.qpcAlignedWrittenSamples),
                                            targetFmt.sampleRate / 1000, (targetFmt.sampleRate * 150) / 1000,
                                            targetFmt.sampleRate / 250, targetFmt.sampleRate / 200);
                                    const size_t packetTimelineFadeSamples =
                                        static_cast<size_t>(std::max<int64_t>(1, targetFmt.sampleRate / 750));
                                    if (timelineAdjustment.gapSamples > 0) {
                                        // Defense-in-depth: bound the leading-silence gap to what the ring
                                        // buffer can actually retain. WriteRetainNew drops the oldest samples
                                        // to make room, so any excess is discarded anyway; clamping here makes
                                        // it impossible for a corrupt/out-of-domain packet timestamp to size a
                                        // pathological allocation (previously a bogus 192kHz-loopback QPC
                                        // produced a multi-TB std::vector<float> -> bad_alloc -> terminate).
                                        const int64_t ringCapacitySamples =
                                            static_cast<int64_t>(src.ringBuffer->GetCapacity()) /
                                            std::max<int64_t>(1, targetFmt.channels);
                                        const int64_t boundedGapSamples = ce::audio::ClampTimelineGapSamplesToCapacity(
                                            timelineAdjustment.gapSamples, ringCapacitySamples);
                                        if (boundedGapSamples < timelineAdjustment.gapSamples) {
                                            DLL_Log(
                                                "[AudioLoop] WARNING: clamped oversized timeline gap src=%d "
                                                "gap=%lld -> %lld samples (ringCapacity=%lld); qpcStart=%llu likely "
                                                "out-of-domain timestamp",
                                                (int)srcIdx, (long long)timelineAdjustment.gapSamples,
                                                (long long)boundedGapSamples, (long long)ringCapacitySamples,
                                                (unsigned long long)packet.qpcPosition);
                                        }
                                        const size_t gapSamples = static_cast<size_t>(boundedGapSamples);
                                        std::vector<float> silence(gapSamples * targetFmt.channels, 0.0f);
                                        const size_t writtenGapFloats =
                                            src.ringBuffer->WriteRetainNew(silence.data(), silence.size());
                                        const size_t writtenGapSamples = writtenGapFloats / targetFmt.channels;
                                        if (!src.bootstrapComplete) {
                                            src.startupSyntheticRingSamples += writtenGapSamples;
                                            if (firstTimelinePacket) {
                                                src.startupGapProtectionSamples += writtenGapSamples;
                                            }
                                        }
                                        src.qpcAlignedWrittenSamples += writtenGapSamples;
                                        src.packetTimelineGapSamples += writtenGapSamples;
                                        if (writtenGapSamples > 0 && writeSamples > 0) {
                                            src.packetBoundaryFadeInSamplesRemaining =
                                                static_cast<int>(packetTimelineFadeSamples);
                                        }
                                    } else if (timelineAdjustment.overlapSamples > 0) {
                                        const size_t overlapSamples = static_cast<size_t>(std::min<int64_t>(
                                            timelineAdjustment.overlapSamples, static_cast<int64_t>(writeSamples)));
                                        writeFloats += overlapSamples * targetFmt.channels;
                                        writeSamples -= overlapSamples;
                                        src.packetTimelineOverlapSamples += overlapSamples;
                                        if (overlapSamples > 0 && writeSamples > 0) {
                                            src.packetBoundaryFadeInSamplesRemaining =
                                                static_cast<int>(packetTimelineFadeSamples);
                                        }
                                    }

                                    // Consumer-overrun attribution. Losing the WHOLE packet to timeline overlap
                                    // is not a benign de-duplication: it is real captured audio destroyed because
                                    // the exported cursor ran past the capture edge. Both advance at wall rate, so
                                    // without intervention the deficit is permanent and the source stays silent
                                    // for the rest of the recording.
                                    ServiceSourceIngestStarvation(src, srcIdx, packetStartSamples,
                                                                  timelineAdjustment.overlapSamples, writeSamples,
                                                                  outSamples, targetFmt.sampleRate, ingestTick);

                                    if ((timelineAdjustment.gapSamples >= (targetFmt.sampleRate / 200) ||
                                         timelineAdjustment.overlapSamples >= (targetFmt.sampleRate / 200))) {
                                        const uint64_t nowTick = GetTickCount64();
                                        if (nowTick - src.lastPacketTimelineAdjustWarnTick >= 1000) {
                                            DLL_Log(
                                                "[AudioLoop] Packet timeline adjust src=%d gap=%lld overlap=%lld "
                                                "written=%llu qpcStart=%llu",
                                                (int)srcIdx, (long long)timelineAdjustment.gapSamples,
                                                (long long)timelineAdjustment.overlapSamples,
                                                (unsigned long long)src.qpcAlignedWrittenSamples,
                                                (unsigned long long)packet.qpcPosition);
                                            src.lastPacketTimelineAdjustWarnTick = nowTick;
                                        }
                                    }

                                    // Placement-divergence diagnostics (throttled 1/s, app sources). The smoking
                                    // gun for "app track goes silent while capture stays live" is the qpc-placed
                                    // write position racing ahead of the read/encoded cursor: that fills the ring
                                    // (forcing retain-trim of the very audio the reader needs) while the reader
                                    // underruns at its own cursor. writeMinusEncoded growing unbounded + ringAvail
                                    // pinned near capacity is the failure; a stable small writeMinusEncoded is healthy.
                                    if (src.sourceType == AudioConfig::AppAudio) {
                                        const uint64_t nowDiagTick = GetTickCount64();
                                        if (nowDiagTick - src.lastAppPlaceDiagTick >= 1000) {
                                            const int64_t encodedCursor = srcIdx < encodedSamplesPerSource.size()
                                                                              ? encodedSamplesPerSource[srcIdx]
                                                                              : 0;
                                            const int64_t writeMinusEncoded =
                                                static_cast<int64_t>(src.qpcAlignedWrittenSamples) - encodedCursor;
                                            const size_t ringAvailSamples =
                                                src.ringBuffer
                                                    ? src.ringBuffer->GetAvailable() / std::max(1, targetFmt.channels)
                                                    : 0;
                                            const size_t ringCapSamples =
                                                src.ringBuffer
                                                    ? src.ringBuffer->GetCapacity() / std::max(1, targetFmt.channels)
                                                    : 0;
                                            DLL_Log(
                                                "[AppDiag] place src=%d track=%d placed=%lld writeCursor=%llu "
                                                "encodedCursor=%lld writeMinusEncoded=%lld pendingWriteSamples=%zu "
                                                "ringAvail=%zu/%zu gapTotal=%llu overlapTotal=%llu lastGap=%lld "
                                                "lastOverlap=%lld",
                                                (int)srcIdx, src.track, (long long)packetStartSamples,
                                                (unsigned long long)src.qpcAlignedWrittenSamples,
                                                (long long)encodedCursor, (long long)writeMinusEncoded, writeSamples,
                                                ringAvailSamples, ringCapSamples,
                                                (unsigned long long)src.packetTimelineGapSamples,
                                                (unsigned long long)src.packetTimelineOverlapSamples,
                                                (long long)timelineAdjustment.gapSamples,
                                                (long long)timelineAdjustment.overlapSamples);
                                            src.lastAppPlaceDiagTick = nowDiagTick;
                                        }
                                    }
                                }

                                if (writeSamples > 0) {
                                    if (src.packetBoundaryFadeInSamplesRemaining > 0) {
                                        const size_t fadeSamples =
                                            static_cast<size_t>(src.packetBoundaryFadeInSamplesRemaining);
                                        ApplyPacketBoundaryFadeIn(writeFloats, writeSamples,
                                                                  static_cast<size_t>(targetFmt.channels), fadeSamples);
                                        src.packetBoundaryFadeInSamplesRemaining =
                                            fadeSamples > writeSamples ? static_cast<int>(fadeSamples - writeSamples)
                                                                       : 0;
                                    }
                                    // WriteRetainNew: atomically drops oldest audio to make room,
                                    // then writes new audio. No race between GetFree/Skip/Write.
                                    const size_t writtenFloats =
                                        src.ringBuffer->WriteRetainNew(writeFloats, writeSamples * targetFmt.channels);
                                    src.qpcAlignedWrittenSamples += writtenFloats / targetFmt.channels;
                                }
                            } else if (src.ringBuffer && audioSyncPending.load()) {
                                // The sync gate is still closed, so this packet is
                                // intentionally discarded and must not establish the
                                // source timeline yet.
                                src.sawSyncPendingPackets = true;
                            } else if (!src.ringBuffer) {
                                DLL_Log("[AudioLoop] ERROR: No RingBuffer for source %d", (int)srcIdx);
                            }
                        }

                        // CRITICAL FIX: Always free the buffer allocated by resampler, even
                        // if outSamples == 0 AudioResampler::Process allocates memory via
                        // av_samples_alloc... which must be freed.
                        AudioResampler::FreeOutputBuffer(resampledData);
                    }
                }
            }

            // ===================================================================
            // PULL MODEL (Phase 2): Legacy audio mixing logic removed
            // ===================================================================

            // Audio-only recording has no video thread to drive the pull model.
            // Use the same per-track mixer as video recording so every source
            // sharing a track contributes exactly once and every exported track
            // advances from one coherent sample cursor.
            if (audioOnly && recording && !audioStopDrainRequested.load(std::memory_order_acquire)) {
                const int64_t elapsedUs = std::chrono::duration_cast<std::chrono::microseconds>(
                                              std::chrono::steady_clock::now() - recordingStartTime)
                                              .count();
                if (elapsedUs > 0) {
                    PullAndEncodeAudio(elapsedUs);
                    ++mixCount;
                }
            }

            if (gotAnyPacket) {
                audioDrainCv.notify_all();
            }

            if (!gotAnyPacket) {
                if (audioStopDrainRequested.load(std::memory_order_acquire)) {
                    if (audioOnly && recording && !audioOnlyStopTailFinalized) {
                        // All capture queues are now empty. Flush both resampler
                        // layers on their owning audio worker before selecting the
                        // final shared-track endpoint, then force every track to
                        // that same sample count.
                        FlushAudioOnlyResamplerTails();

                        constexpr int64_t kMixerSampleRate = 48000;
                        const int64_t wallElapsedUs =
                            std::max<int64_t>(0, std::chrono::duration_cast<std::chrono::microseconds>(
                                                     std::chrono::steady_clock::now() - recordingStartTime)
                                                     .count());
                        int64_t finalTargetSamples =
                            ce::audio::ComputeDurationUsToSamples(wallElapsedUs, kMixerSampleRate);
                        for (const auto& src : audioSources) {
                            finalTargetSamples = std::max<int64_t>(finalTargetSamples,
                                                                   static_cast<int64_t>(src.qpcAlignedWrittenSamples));
                            finalTargetSamples = std::max(finalTargetSamples, src.syncSamplesOutput);
                        }
                        for (const auto& track : trackTimelineSamples) {
                            finalTargetSamples = std::max(finalTargetSamples, track.second);
                        }
                        const int64_t finalTargetUs =
                            (finalTargetSamples * 1000000ll + kMixerSampleRate - 1) / kMixerSampleRate;
