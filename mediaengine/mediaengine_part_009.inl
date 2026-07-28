
        if (!vRes) {
            DLL_Log("MediaEngine: Failed to re-init VideoEncoder!");
            return;
        }
        DLL_Log("MediaEngine: VideoEncoder re-initialized successfully.");

        // Re-create audio sources with new config (including new codec)
        // Maps track number to encoder for that track
        std::map<int, AudioEncoder*> trackToEncoder;
        // Guards against summing two identical app-audio captures into one track.
        std::set<std::string> seenAppAudioTrackKeys;

        for (size_t i = 0; i < config.audioSources.size(); i++) {
            const AudioConfig& audioConfig = config.audioSources[i];
            if (!audioConfig.enabled) {
                DLL_Log("MediaEngine::ReloadConfig audio source %zu disabled", i);
                continue;
            }

            DLL_Log("MediaEngine::ReloadConfig setting up audio source %zu (codec=%s)", i, audioConfig.codec.c_str());

            // Get the list of tracks this source should output to
            std::vector<int> targetTracks = audioConfig.tracks;
            if (targetTracks.empty()) {
                targetTracks.push_back((int)(i + 1));
            }

            DLL_Log("MediaEngine::ReloadConfig Audio source %zu targets %zu tracks", i, targetTracks.size());

            // For each target track, create or reuse an encoder
            for (int track : targetTracks) {
                // Defense in depth: never create a second app-audio capture for the
                // same process on the same track. Summing identical captures combs.
                if (audioConfig.sourceType == AudioConfig::AppAudio) {
                    const std::string appKey = AppAudioTrackKey(audioConfig, track);
                    if (!seenAppAudioTrackKeys.insert(appKey).second) {
                        DLL_Log(
                            "MediaEngine::ReloadConfig WARNING: duplicate app-audio source (process='%s' "
                            "processId=%lu) already targets track %d - skipping duplicate capture to avoid "
                            "comb-filter artifacts",
                            audioConfig.processName.empty() ? "<pid>" : audioConfig.processName.c_str(),
                            (unsigned long)audioConfig.processId, track);
                        continue;
                    }
                }
                TrackAudioFormat trackFormat = GetTrackAudioFormat(track);
                AudioConfig resolvedAudioConfig = audioConfig;
                resolvedAudioConfig.outputChannels = audioConfig.downmix ? 2 : trackFormat.channels;
                resolvedAudioConfig.outputChannelMask =
                    audioConfig.downmix ? DefaultChannelMaskForChannels(2) : trackFormat.channelMask;
                AudioEncoder* encoderForTrack = nullptr;
                auto it = trackToEncoder.find(track);
                if (it != trackToEncoder.end()) {
                    encoderForTrack = it->second;
                    DLL_Log(
                        "MediaEngine::ReloadConfig Audio source %zu reusing encoder "
                        "for track %d",
                        i, track);
                } else {
                    // Create new encoder for this track
                    auto newEncoder = std::make_unique<AudioEncoder>();
                    bool aRes =
                        newEncoder->Init(resolvedAudioConfig, [this](AVPacket* pkt) { this->WritePacket(pkt); });

                    if (!aRes) {
                        DLL_Log("MediaEngine::ReloadConfig Audio encoder for track %d failed", track);
                        continue;
                    }

                    videoEnc->AddAudioContext(resolvedAudioConfig, newEncoder->GetCodecContext(), track);

                    encoderForTrack = newEncoder.get();
                    trackToEncoder[track] = encoderForTrack;

                    AudioSource source;
                    source.config = resolvedAudioConfig;
                    source.track = track;
                    source.configuredSourceIndex = i;
                    source.sourceType = audioConfig.sourceType;
                    source.mixChannels =
                        resolvedAudioConfig.outputChannels > 0 ? resolvedAudioConfig.outputChannels : 2;
                    source.mixChannelMask = resolvedAudioConfig.outputChannelMask != 0
                                                ? resolvedAudioConfig.outputChannelMask
                                                : DefaultChannelMaskForChannels(source.mixChannels);
                    source.encoder = std::move(newEncoder);
                    source.sharedEncoderPtr = source.encoder.get();

                    // Create appropriate capture type
                    if (audioConfig.sourceType == AudioConfig::AppAudio) {
                        source.appCapture = std::make_unique<ProcessLoopbackCapture>();
                        source.appCapture->SetRequestedFormat(48000, source.mixChannels, source.mixChannelMask);
                    } else {
                        source.capture = std::make_unique<AudioCapture>();
                    }

                    // INIT RING BUFFER AND SYNC RESAMPLER (Per-source drift compensation)
                    InitAudioSourceBuffers(source, audioConfig, i);

                    DLL_Log(
                        "MediaEngine::ReloadConfig Created new encoder for track %d "
                        "(source %zu, type=%d)",
                        track, i, (int)audioConfig.sourceType);
                    audioSources.push_back(std::move(source));
                }

                if (it != trackToEncoder.end()) {
                    AudioSource source;
                    source.config = resolvedAudioConfig;
                    source.track = track;
                    source.configuredSourceIndex = i;
                    source.sourceType = audioConfig.sourceType;
                    source.mixChannels =
                        resolvedAudioConfig.outputChannels > 0 ? resolvedAudioConfig.outputChannels : 2;
                    source.mixChannelMask = resolvedAudioConfig.outputChannelMask != 0
                                                ? resolvedAudioConfig.outputChannelMask
                                                : DefaultChannelMaskForChannels(source.mixChannels);
                    source.encoder = nullptr;
                    source.sharedEncoderPtr = encoderForTrack;

                    // Create appropriate capture type
                    if (audioConfig.sourceType == AudioConfig::AppAudio) {
                        source.appCapture = std::make_unique<ProcessLoopbackCapture>();
                        source.appCapture->SetRequestedFormat(48000, source.mixChannels, source.mixChannelMask);
                    } else {
                        source.capture = std::make_unique<AudioCapture>();
                    }

                    // INIT RING BUFFER AND SYNC RESAMPLER (Per-source drift compensation)
                    InitAudioSourceBuffers(source, audioConfig, i);

                    DLL_Log(
                        "MediaEngine::ReloadConfig Audio source %zu shares encoder "
                        "for track %d (type=%d)",
                        i, track, (int)audioConfig.sourceType);
                    audioSources.push_back(std::move(source));
                }
            }
        }

        CoalesceCaptureRoutes();

        DLL_Log(
            "MediaEngine: ReloadConfig complete. Audio sources: %zu, unique "
            "tracks: %zu",
            audioSources.size(), trackToEncoder.size());
    }

private:
    // Identity of an app-audio capture targeting a specific track. Two app-audio
    // sources that resolve to the same process AND feed the same track would
    // capture the same audio twice; summing those near-identical (independently
    // buffered, phase-offset) streams into one track produces comb-filter
    // "metallic" artifacts. We use this key to detect and skip such duplicates.
    static std::string AppAudioTrackKey(const AudioConfig& cfg, int track) {
        return ce::audio::AppAudioTrackIdentity(cfg.processName, static_cast<unsigned long>(cfg.processId), track);
    }

    // Collapse the route objects created for a multi-track source onto one physical
    // capture object. Route-local resamplers/rings remain independent because each
    // track owns a timeline and mix format, but their input packets are fanned out
    // by AudioLoop from this single owner. Process loopback requests the widest
    // routed layout once, then each route's resampler converts that common packet
    // stream to its track layout. Endpoint capture remains native.
    void CoalesceCaptureRoutes() {
        using CaptureRouteKey = std::tuple<int, std::string>;
        std::map<CaptureRouteKey, size_t> owners;
        std::map<CaptureRouteKey, std::pair<int, uint32_t>> appCaptureFormats;
        size_t physicalCaptureCount = 0;
        size_t sharedRouteCount = 0;

        auto captureKey = [](const AudioSource& src) -> CaptureRouteKey {
            std::string physicalIdentity;
            if (src.sourceType == AudioConfig::AppAudio) {
                physicalIdentity = ce::audio::AppAudioTrackIdentity(
                    src.config.processName, static_cast<unsigned long>(src.config.processId), 0);
            } else {
                physicalIdentity = src.config.device.empty() ? "<default>" : src.config.device;
                std::transform(physicalIdentity.begin(), physicalIdentity.end(), physicalIdentity.begin(),
                               [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
            }
            return {static_cast<int>(src.sourceType), std::move(physicalIdentity)};
        };

        for (const auto& src : audioSources) {
            if (src.sourceType != AudioConfig::AppAudio) {
                continue;
            }
            const CaptureRouteKey key = captureKey(src);
            auto& format = appCaptureFormats[key];
            if (src.mixChannels > format.first) {
                format = {src.mixChannels, src.mixChannelMask};
            }
        }

        for (size_t idx = 0; idx < audioSources.size(); ++idx) {
            auto& src = audioSources[idx];
            const CaptureRouteKey key = captureKey(src);
            const auto [it, inserted] = owners.emplace(key, idx);
            if (inserted) {
                src.captureFanoutOwnerIndex = idx;
                if (src.appCapture) {
                    const auto format = appCaptureFormats[key];
                    src.appCapture->SetRequestedFormat(48000, format.first, format.second);
                    DLL_Log("[AudioRoute] App capture owner=%zu configuredSource=%zu requests shared format=%dch/0x%x",
                            idx, src.configuredSourceIndex, format.first, format.second);
                }
                ++physicalCaptureCount;
                continue;
            }

            const size_t ownerIdx = it->second;
            src.captureFanoutOwnerIndex = ownerIdx;
            src.capture.reset();
            src.appCapture.reset();
            ++sharedRouteCount;
            DLL_Log(
                "[AudioRoute] Source route src=%zu track=%d shares physical capture owner=%zu track=%d "
                "configuredSource=%zu type=%d format=%dch/0x%x",
                idx, src.track, ownerIdx, audioSources[ownerIdx].track, src.configuredSourceIndex,
                static_cast<int>(src.sourceType), src.mixChannels, src.mixChannelMask);
        }

        DLL_Log("[AudioRoute] Capture topology: physical=%zu routedFollowers=%zu totalRoutes=%zu", physicalCaptureCount,
                sharedRouteCount, audioSources.size());
    }

    ProcessLoopbackCapture* GetAppCaptureForRoute(size_t srcIdx) {
        if (srcIdx >= audioSources.size()) {
            return nullptr;
        }
        const size_t ownerIdx = audioSources[srcIdx].captureFanoutOwnerIndex;
        if (ownerIdx >= audioSources.size()) {
            return nullptr;
        }
        return audioSources[ownerIdx].appCapture.get();
    }

    std::pair<int64_t, int64_t> GetCaptureGroupBufferedSampleRange(size_t srcIdx) const {
        if (srcIdx >= audioSources.size()) {
            return {0, 0};
        }
        const size_t ownerIdx = audioSources[srcIdx].captureFanoutOwnerIndex;
        int64_t minimum = std::numeric_limits<int64_t>::max();
        int64_t maximum = 0;
        for (const auto& route : audioSources) {
            if (route.captureFanoutOwnerIndex != ownerIdx || !route.ringBuffer) {
                continue;
            }
            const size_t channels = static_cast<size_t>(std::max(1, route.mixChannels));
            const int64_t bufferedSamples = static_cast<int64_t>(route.ringBuffer->GetAvailable() / channels);
            minimum = std::min(minimum, bufferedSamples);
            maximum = std::max(maximum, bufferedSamples);
        }
        return {minimum == std::numeric_limits<int64_t>::max() ? 0 : minimum, maximum};
    }

    // Shared initialization for ring buffer and sync resampler on an AudioSource.
    // Parses sample rate from config (defaults to 48000) and sets up both.
    void InitAudioSourceBuffers(AudioSource& source, const AudioConfig& audioConfig, size_t sourceIdx) {
        constexpr int kMixerSampleRate = 48000;
        constexpr size_t kDefaultAudioRingBufferSeconds = 8;
        // Heavy CFR overload runs can fall tens of seconds behind real time even
        // while audio/video file durations still stay mathematically equal. Give CFR
        // enough retention headroom to avoid destructive oldest-audio trims in those
        // runs so we preserve pitch and avoid crackle while diagnostics report the
        // underlying encoder shortfall honestly.
        constexpr size_t kCfrAudioRingBufferSeconds = 30;
        const bool isCfrPath = ce::audio::ShouldUseCfrAudioContinuityPolicy(config.video.useVFR);
        const size_t ringBufferSeconds = isCfrPath ? kCfrAudioRingBufferSeconds : kDefaultAudioRingBufferSeconds;
        const int channels = std::clamp(source.mixChannels, 1, 8);
        const size_t capacity = static_cast<size_t>(kMixerSampleRate) * ringBufferSeconds * channels;
        source.fullRingBufferCapacityFloats = capacity;

        // App-audio sources commonly target candidate processes that may not be
        // running (a "capture whichever game is running" profile). Allocating the
        // full multi-second CFR retention buffer (~11.5 MB at 30s/48k/stereo) for
        // every such source wastes memory on sources that never capture. Start app
        // sources with a small buffer and grow in-place to full capacity on first
        // captured audio (see AudioLoop). System/mic sources are always active, so
        // they keep full capacity immediately.
        constexpr size_t kAppAudioInitialRingBufferSeconds = 1;
        const size_t initialCapacity = (source.appCapture != nullptr)
                                           ? std::min(capacity, static_cast<size_t>(kMixerSampleRate) *
                                                                    kAppAudioInitialRingBufferSeconds * channels)
                                           : capacity;
        source.ringBuffer = std::make_unique<AudioRingBuffer>(initialCapacity);
        DLL_Log(
            "MediaEngine::Init RingBuffer created for source %zu. Cap=%zu floats (initial, full=%zu floats/%zus), "
            "rate=%d channels=%d mask=0x%x deferred=%d",
            sourceIdx, initialCapacity, capacity, ringBufferSeconds, kMixerSampleRate, channels, source.mixChannelMask,
            initialCapacity < capacity ? 1 : 0);

        source.syncResampler = std::make_unique<AudioResampler>();
        AudioResampler::InputFormat syncInFmt;
        syncInFmt.sampleRate = kMixerSampleRate;
        syncInFmt.channels = channels;
        syncInFmt.bitsPerSample = 32;
        syncInFmt.validBitsPerSample = 32;
        syncInFmt.isFloat = true;
        syncInFmt.blockAlign = channels * 4;
        syncInFmt.channelMask = source.mixChannelMask;
        AudioResampler::OutputFormat syncOutFmt;
        syncOutFmt.sampleRate = kMixerSampleRate;
        syncOutFmt.channels = channels;
        syncOutFmt.sampleFmt = AV_SAMPLE_FMT_FLT;
        syncOutFmt.channelMask = source.mixChannelMask;
        source.syncResampler->Init(syncInFmt, syncOutFmt);
        DLL_Log("MediaEngine::Init SyncResampler created for source %zu (rate=%d channels=%d)", sourceIdx,
                kMixerSampleRate, channels);
    }

    void AudioLoop() {
        DLL_Log("MediaEngine: Audio thread started (sources=%d)", (int)audioSources.size());
        int packetCount = 0;
        int mixCount = 0;

        // Check if any track has multiple sources (requires mixing)
        std::map<int, int> trackSourceCount;
        std::map<int, std::vector<size_t>> trackSourceIndices;  // track -> source indices
        for (size_t i = 0; i < audioSources.size(); i++) {
            auto& src = audioSources[i];
            trackSourceCount[src.track]++;
            trackSourceIndices[src.track].push_back(i);
        }

        // Per-track source summary (process names) and a runtime guard that surfaces
        // any duplicate app-audio capture feeding one track (identical streams comb).
        for (auto& kv : trackSourceIndices) {
            std::string summary;
            std::set<std::string> appIdentities;
            for (size_t idx : kv.second) {
                auto& s = audioSources[idx];
                std::string label;
                if (s.sourceType == AudioConfig::AppAudio) {
                    const char* pn = s.config.processName.empty() ? "<pid>" : s.config.processName.c_str();
                    label = std::string("app:") + pn;
                    if (!appIdentities.insert(AppAudioTrackKey(s.config, kv.first)).second) {
                        DLL_Log(
                            "AudioLoop: WARNING - track %d has duplicate app-audio capture for '%s' - identical "
                            "streams will comb-filter when mixed",
                            kv.first, pn);
                    }
                } else if (s.sourceType == AudioConfig::Microphone) {
                    label = "mic";
                } else {
                    label = "system";
                }
                if (!summary.empty())
                    summary += ", ";
                summary += label;
            }
            DLL_Log("AudioLoop: Track %d sources: [%s]", kv.first, summary.c_str());
        }

        for (auto& kv : trackSourceCount) {
            if (kv.second > 1) {
                DLL_Log("AudioLoop: Track %d has %d sources - REAL mixing enabled", kv.first, kv.second);
            }
        }
        constexpr int64_t kStartupFirstPacketGapCapSamples = 480;
        constexpr int64_t kStartupFirstPacketRebaseThresholdSamples = 2400;

        std::vector<int64_t> sourceTimestamps(audioSources.size(), 0);
        std::vector<bool> sourceLoggedPreStartDrop(audioSources.size(), false);
        std::map<int, int64_t> trackNextTimestamp;  // Track continuous timestamps for mixing
        std::vector<AudioPacket> sourceLastPackets(audioSources.size());
        std::vector<std::chrono::steady_clock::time_point> lastPacketTime(audioSources.size(),
                                                                          std::chrono::steady_clock::now());
        std::vector<AudioPacket> deferredFirstTimelinePackets(audioSources.size());
        std::vector<bool> deferredFirstTimelinePacketValid(audioSources.size(), false);
        std::vector<int64_t> deferredFirstTimelinePacketStartSamples(audioSources.size(), 0);
        std::vector<uint64_t> sourceCaptureEpochs(audioSources.size(), 0);
        std::vector<std::deque<AudioPacket>> pendingEpochPackets(audioSources.size());
        std::vector<std::deque<AudioPacket>> captureFanoutQueues(audioSources.size());
        std::vector<uint64_t> captureFanoutPacketCounts(audioSources.size(), 0);
        std::vector<uint64_t> batchedPreStartDiscardCounts(audioSources.size(), 0);
        constexpr int64_t kAudioWorkerSchedulingGapThresholdUs = 25000;
        auto lastAudioWorkerIteration = std::chrono::steady_clock::now();
        auto audioWorkerSchedulingDiagnosticsArmTime = lastAudioWorkerIteration + std::chrono::seconds(1);
        uint64_t audioWorkerSchedulingGapEvents = 0;
        int64_t audioWorkerSchedulingGapMaxUs = 0;
        int64_t lastAudioWorkerSchedulingGapLogMs = 0;
        int64_t sharedStartupRebaseOffsetSamples = -1;
        uint64_t appliedAudioResetGeneration = audioResetAcknowledgedGeneration.load(std::memory_order_acquire);
        bool audioOnlyStopTailFinalized = false;

        // Per-source A/V equalization: delay each audio source so every source sits at the same
        // (max) capture latency, matching the video content delay. Faster sources (e.g. a
        // near-zero-latency microphone vs a high-latency loopback endpoint) get leading silence
        // so they align with the delayed video AND with each other on mixed tracks. The slowest
        // source(s) get delay 0, so the common all-equal-latency case is unchanged (no regression).
        float maxAudioCaptureLatencyMs = 0.0f;
        for (const auto& eqSrc : audioSources) {
            if (eqSrc.config.captureLatencyMs > maxAudioCaptureLatencyMs) {
                maxAudioCaptureLatencyMs = eqSrc.config.captureLatencyMs;
            }
        }
        DLL_Log("[AVSyncAuto] audio_equalization: sources=%zu maxCaptureLatencyMs=%.3f confidence=%s reason=%s",
                audioSources.size(), static_cast<double>(maxAudioCaptureLatencyMs), config.avSyncConfidence.c_str(),
                config.avSyncReason.c_str());
        std::vector<int64_t> audioEqualizationDelaySamples(audioSources.size(), 0);
        for (size_t i = 0; i < audioSources.size(); ++i) {
            const double deltaMs = static_cast<double>(maxAudioCaptureLatencyMs) -
                                   static_cast<double>(audioSources[i].config.captureLatencyMs);
            audioEqualizationDelaySamples[i] =
                deltaMs > 0.0 ? static_cast<int64_t>(std::llround(deltaMs / 1000.0 * 48000.0)) : 0;
            if (audioEqualizationDelaySamples[i] > 0) {
                DLL_Log(
                    "[AudioLoop] A/V equalization: src=%zu captureLatencyMs=%.3f delaySamples=%lld (%.1f ms) to "
                    "match maxLatencyMs=%.3f",
                    i, static_cast<double>(audioSources[i].config.captureLatencyMs),
                    (long long)audioEqualizationDelaySamples[i],
                    static_cast<double>(audioEqualizationDelaySamples[i]) * 1000.0 / 48000.0,
                    static_cast<double>(maxAudioCaptureLatencyMs));
            }
        }

        auto sourceParticipatesInSharedStartupRebase = [this](size_t srcIdx) -> bool {
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
        };

        auto trySelectSharedStartupRebase = [&](bool finalStopDrain = false) -> bool {
            if (sharedStartupRebaseOffsetSamples >= 0) {
                return true;
            }

            size_t participants = 0;
            size_t readyParticipants = 0;
            int64_t earliestPacketStartSamples = std::numeric_limits<int64_t>::max();
            for (size_t i = 0; i < audioSources.size(); ++i) {
                if (!sourceParticipatesInSharedStartupRebase(i)) {
                    continue;
                }
                ++participants;
                if (!deferredFirstTimelinePacketValid[i] && sourceTimestamps[i] == 0) {
                    continue;
                }
                ++readyParticipants;
                if (deferredFirstTimelinePacketValid[i]) {
                    earliestPacketStartSamples =
                        std::min<int64_t>(earliestPacketStartSamples, deferredFirstTimelinePacketStartSamples[i]);
                }
            }

            if (participants == 0) {
                sharedStartupRebaseOffsetSamples = 0;
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
                sharedStartupRebaseOffsetSamples = 0;
                DLL_Log("[AudioLoop] Shared startup rebase disabled: participants=%zu had no deferred QPC baseline",
                        participants);
                return true;
            }

            sharedStartupRebaseOffsetSamples = ce::audio::ComputeSharedStartupFirstPacketRebaseOffset(
                earliestPacketStartSamples, kStartupFirstPacketGapCapSamples,
                kStartupFirstPacketRebaseThresholdSamples);
            DLL_Log(
                "[AudioLoop] Shared startup rebase selected offset=%lld samples earliest=%lld cap=%lld "
                "participants=%zu",
                (long long)sharedStartupRebaseOffsetSamples, (long long)earliestPacketStartSamples,
                (long long)kStartupFirstPacketGapCapSamples, participants);
            return true;
        };

        while (audioRunning) {
            const auto audioWorkerIterationNow = std::chrono::steady_clock::now();
            const int64_t audioWorkerSchedulingGapUs = std::chrono::duration_cast<std::chrono::microseconds>(
                                                           audioWorkerIterationNow - lastAudioWorkerIteration)
                                                           .count();
            lastAudioWorkerIteration = audioWorkerIterationNow;
            const uint64_t requestedResetGeneration = audioResetRequestedGeneration.load(std::memory_order_acquire);
            const bool audioTimelineCommitted =
                requestedResetGeneration <= audioResetCommittedGeneration.load(std::memory_order_acquire);
            if (audioTimelineCommitted && audioWorkerIterationNow >= audioWorkerSchedulingDiagnosticsArmTime &&
                recordingStartSystemQPCMs.load(std::memory_order_acquire) > 0 &&
                !audioStopDrainRequested.load(std::memory_order_acquire) &&
                audioWorkerSchedulingGapUs >= kAudioWorkerSchedulingGapThresholdUs) {
                ++audioWorkerSchedulingGapEvents;
                audioWorkerSchedulingGapMaxUs =
                    std::max(audioWorkerSchedulingGapMaxUs, audioWorkerSchedulingGapUs);
                const int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                          audioWorkerIterationNow.time_since_epoch())
                                          .count();
                if (audioWorkerSchedulingGapEvents <= 5 || nowMs - lastAudioWorkerSchedulingGapLogMs >= 1000) {
                    size_t maxRingSamples = 0;
                    size_t pendingFanoutPackets = 0;
                    size_t pendingEpochPacketCount = 0;
                    for (size_t srcIdx = 0; srcIdx < audioSources.size(); ++srcIdx) {
                        const auto& src = audioSources[srcIdx];
                        if (src.ringBuffer) {
                            const size_t channels = static_cast<size_t>(std::clamp(src.mixChannels, 1, 8));
                            maxRingSamples = std::max(maxRingSamples, src.ringBuffer->GetAvailable() / channels);
                        }
                        pendingFanoutPackets += captureFanoutQueues[srcIdx].size();
                        pendingEpochPacketCount += pendingEpochPackets[srcIdx].size();
                    }
                    DLL_Log(
                        "[AudioLoop] Scheduling gap: gap=%lldus threshold=%lldus max=%lldus events=%llu "
                        "maxRing=%zu samples fanoutPending=%zu epochPending=%zu; packet continuity counters remain "
                        "authoritative for audible-loss classification",
                        (long long)audioWorkerSchedulingGapUs, (long long)kAudioWorkerSchedulingGapThresholdUs,
                        (long long)audioWorkerSchedulingGapMaxUs,
                        static_cast<unsigned long long>(audioWorkerSchedulingGapEvents), maxRingSamples,
                        pendingFanoutPackets, pendingEpochPacketCount);
                    lastAudioWorkerSchedulingGapLogMs = nowMs;
                }
            }
            if (requestedResetGeneration > appliedAudioResetGeneration) {
                const bool preserveResetPackets = audioResetPreservePackets.load(std::memory_order_acquire);
                ApplyAudioTimelineReset(requestedResetGeneration,
                                        recordingStartSystemQPCMs.load(std::memory_order_acquire),
                                        preserveResetPackets);
                std::fill(sourceTimestamps.begin(), sourceTimestamps.end(), 0);
                std::fill(sourceLoggedPreStartDrop.begin(), sourceLoggedPreStartDrop.end(), false);
                std::fill(deferredFirstTimelinePacketValid.begin(), deferredFirstTimelinePacketValid.end(), false);
                std::fill(deferredFirstTimelinePacketStartSamples.begin(),
                          deferredFirstTimelinePacketStartSamples.end(), 0);
                std::fill(sourceCaptureEpochs.begin(), sourceCaptureEpochs.end(), 0);
                std::fill(sourceLastPackets.begin(), sourceLastPackets.end(), AudioPacket{});
                std::fill(lastPacketTime.begin(), lastPacketTime.end(), std::chrono::steady_clock::now());
                for (auto& queue : pendingEpochPackets) {
                    queue.clear();
                }
                if (!preserveResetPackets) {
                    for (auto& queue : captureFanoutQueues) {
                        queue.clear();
                    }
                }
                trackNextTimestamp.clear();
                sharedStartupRebaseOffsetSamples = -1;
                audioOnlyStopTailFinalized = false;
                lastAudioWorkerIteration = std::chrono::steady_clock::now();
                audioWorkerSchedulingDiagnosticsArmTime = lastAudioWorkerIteration + std::chrono::seconds(1);
                appliedAudioResetGeneration = requestedResetGeneration;
                audioResetAcknowledgedGeneration.store(requestedResetGeneration, std::memory_order_release);
                audioDrainCv.notify_all();
            }

            if (requestedResetGeneration > audioResetCommittedGeneration.load(std::memory_order_acquire) &&
                !audioStopDrainRequested.load(std::memory_order_acquire)) {
                std::unique_lock<std::mutex> lock(audioDrainMutex);
                audioDrainCv.wait(lock, [this, requestedResetGeneration]() {
                    return !audioRunning.load(std::memory_order_acquire) ||
                           audioResetCommittedGeneration.load(std::memory_order_acquire) >= requestedResetGeneration ||
                           audioStopDrainRequested.load(std::memory_order_acquire);
                });
                continue;
            }

            if (audioSyncPending.load(std::memory_order_acquire) &&
                preservePendingStartupAudioPackets.load(std::memory_order_acquire) &&
                !audioStopDrainRequested.load(std::memory_order_acquire)) {
                std::unique_lock<std::mutex> lock(audioDrainMutex);
                audioDrainCv.wait_for(lock, std::chrono::milliseconds(5), [this]() {
                    return !audioRunning.load(std::memory_order_acquire) ||
                           !audioSyncPending.load(std::memory_order_acquire) ||
                           audioStopDrainRequested.load(std::memory_order_acquire);
                });
                continue;
            }

            bool gotAnyPacket = false;
            auto now = std::chrono::steady_clock::now();

            // Step 1: Poll all sources and accumulate samples into buffers
            for (size_t srcIdx = 0; srcIdx < audioSources.size(); srcIdx++) {
                auto& src = audioSources[srcIdx];
                if (!src.sharedEncoderPtr)
                    continue;

                // Skip if neither a physical capture nor a routed packet is available.
                if (!src.capture && !src.appCapture && captureFanoutQueues[srcIdx].empty())
                    continue;

                AudioPacket packet;
                bool gotPacket = false;
                bool gotDeferredFirstTimelinePacket = false;

                const uint64_t requestedEpochReset = src.epochResetRequested->load(std::memory_order_acquire);
                const uint64_t acknowledgedEpochReset = src.epochResetAcknowledged->load(std::memory_order_acquire);
                if (requestedEpochReset != 0 && requestedEpochReset != acknowledgedEpochReset) {
                    // The pull owner is still encoding the previous epoch's ring/sync/post tail.
                    // Do not poll new capture data into route state until it acknowledges the reset.
                    continue;
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
                    if (!trySelectSharedStartupRebase(finalStopDrain)) {
                        gotAnyPacket = true;
                        continue;
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
