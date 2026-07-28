if False:

        multi_app_stall = make_session(
            "multi_app_stall",
            media=(
                "[STOP AUDIO TRACK] Track 1: encoded=1360000 expected=12490400 "
                "diff=-11130400 (-231883.333 ms) sources=[1,3,5,7,9]\n"
                "[STOP AUDIO TRACK] Track 2: encoded=1360000 expected=12490400 "
                "diff=-11130400 (-231883.333 ms) sources=[4,6,8,10]\n"
                "[STOP AUDIO TRACK] Track 3: encoded=12490400 expected=12490400 diff=0 (0.000 ms) sources=[2]\n"
            ),
        )
        report = classify_session_triage(multi_app_stall)
        assert "multi_app_audio_track_stall" in report["verdicts"]
        assert "ce_audio_timeline_fault" in report["verdicts"]
        assert report["evidence"]["stop_audio_shortfalls"]["multi_source_short_count"] == 2

        # `logs/audiodeath`: exact track lengths and perfect packet timing while every source
        # was actually silent, because the exported cursor ran past the live capture edge and
        # each later packet was destroyed as timeline overlap. Only the destroyed-sample
        # counter can surface it, so it must be a strict verdict on its own.
        audio_ingest_starvation = make_session(
            "audio_ingest_starvation",
            media=(
                "[STOP AUDIO TRACK] Track 1: encoded=11888000 expected=11888000 diff=+0 (+0.000 ms) "
                "realMixed=3056000 fullSilence=8832000 partialSilence=1091920 sources=[1,9]\n"
                "[STOP AUDIO INGEST] Source 1: track=1 starve=0 resync=0/0 reservoirPeakMs=0 process=-\n"
                "[STOP AUDIO INGEST] Source 9: track=1 starve=10109800 resync=0/0 reservoirPeakMs=500 "
                "process=FortniteClient-Win64-Shipping.exe\n"
            ),
        )
        report = classify_session_triage(audio_ingest_starvation)
        assert "audio_ingest_starvation" in report["verdicts"]
        assert "audio_ingest_starvation_resync" not in report["verdicts"]
        assert "ce_audio_timeline_fault" in report["verdicts"]
        starvation = report["evidence"]["audio_ingest_starvation"]
        assert starvation["destroyed_samples"] == 10109800
        assert starvation["reservoir_peak_ms"] == 500
        assert len(starvation["affected_sources"]) == 1

        # A healthy run reports the same line with zero destroyed samples and must stay clean,
        # including when the adaptive reservoir legitimately deepened during the recording.
        audio_ingest_healthy = make_session(
            "audio_ingest_healthy",
            media=(
                "[STOP AUDIO INGEST] Source 0: track=1 starve=0 resync=0/0 reservoirPeakMs=95 process=-\n"
                "[STOP AUDIO INGEST] Source 1: track=2 starve=0 resync=0/0 reservoirPeakMs=95 process=-\n"
            ),
        )
        healthy_report = classify_session_triage(audio_ingest_healthy)
        assert "audio_ingest_starvation" not in healthy_report["verdicts"]
        assert healthy_report["evidence"]["audio_ingest_starvation"]["destroyed_samples"] == 0
        assert healthy_report["evidence"]["audio_ingest_starvation"]["reservoir_peak_ms"] == 95

        # Pre-counter logs (such as the original `logs/audiodeath` capture) still expose the
        # failure through the per-source packet-overlap total, so triage of old sessions must
        # not silently pass. Ordinary boundary de-duplication stays below the thresholds.
        audio_ingest_legacy = make_session(
            "audio_ingest_legacy",
            media=(
                "[STOP AUDIO] Source 9: track=1 encoded=11888000 trim=cov:0 latTotal:0 liveUncat:0 cat:0 "
                "normal:0 pad:0 qgap:0 qjoin:0 qjoinKeep:0 ringPeak=46560 ringUnderruns=0 "
                "process=FortniteClient-Win64-Shipping.exe\n"
                "[STOP AUDIO DETAIL] Source 9: ratePpm=+0.00 compDelta=0 sat=0 trimRate(latTotal=0.0/min "
                "boot=0.0/min cov=0.0/min tier2=0.0/min retain=0.0/min) totals(boot=0 tier2=0 retain=0 cat=0 "
                "catEvents=0 liveUncat=0 post=0 overlap=10109800 ovf=0)\n"
            ),
        )
        legacy_report = classify_session_triage(audio_ingest_legacy)
        assert "audio_ingest_starvation" in legacy_report["verdicts"]
        assert legacy_report["evidence"]["audio_ingest_starvation"]["legacy_overlap_evidence"]
        assert legacy_report["evidence"]["audio_ingest_starvation"]["destroyed_samples"] == 10109800

        audio_ingest_legacy_clean = make_session(
            "audio_ingest_legacy_clean",
            media=(
                "[STOP AUDIO] Source 1: track=1 encoded=11888000 trim=cov:0 latTotal:0 liveUncat:0 cat:0 "
                "normal:0 pad:0 qgap:1426 qjoin:0 qjoinKeep:0 ringPeak=38880 ringUnderruns=0 process=-\n"
                "[STOP AUDIO DETAIL] Source 1: ratePpm=+0.00 compDelta=0 sat=0 trimRate(latTotal=0.0/min "
                "boot=0.0/min cov=0.0/min tier2=0.0/min retain=0.0/min) totals(boot=0 tier2=0 retain=0 cat=0 "
                "catEvents=0 liveUncat=0 post=0 overlap=161 ovf=0)\n"
            ),
        )
        legacy_clean_report = classify_session_triage(audio_ingest_legacy_clean)
        assert "audio_ingest_starvation" not in legacy_clean_report["verdicts"]
        assert legacy_clean_report["evidence"]["audio_ingest_starvation"]["destroyed_samples"] == 0

        # The last-resort re-anchor is bounded and content-costly, so it is its own verdict.
        audio_ingest_resync = make_session(
            "audio_ingest_resync",
            media=("[STOP AUDIO INGEST] Source 3: track=1 starve=96000 resync=3360/1 reservoirPeakMs=500 process=-\n"),
        )
        resync_report = classify_session_triage(audio_ingest_resync)
        assert "audio_ingest_starvation" in resync_report["verdicts"]
        assert "audio_ingest_starvation_resync" in resync_report["verdicts"]

        late_app_backlog = make_session(
            "late_app_backlog",
            media=(
                "[PullAudio] Source primed - src=9 track=1 buffered=359086 realBuffered=358483 needed=1200 "
                "lateStart=7459ms\n"
                "[PullAudio] App source gap silence - src 9 added 480 samples to track 1\n"
                "[PullAudio] WARNING: Source underrun - src 9 padding 480 samples with silence "
                "(available=0 needed=480 forceDrain=0)\n"
            ),
        )
        report = classify_session_triage(late_app_backlog)
        assert "late_app_source_backlog" in report["verdicts"]
        assert "started_app_source_underrun" in report["verdicts"]
        assert "ce_audio_timeline_fault" in report["verdicts"]
        assert report["faults"]["late_app_source_backlog"]

        exported_sync_with_app_underrun = make_session(
            "exported_sync_with_app_underrun",
            media=(
                "[PullAudio] WARNING: Source underrun - src 9 padding 480 samples with silence "
                "(available=0 needed=480 forceDrain=0)\n"
                "[PullAudio] App source gap silence - src 9 added 480 samples to track 1\n"
                "[STOP AUDIO] Source 9: track=1 encoded=48000 trim=cov:0 latTotal:0 liveUncat:0 "
                "pad:480 qgap:0 qjoin:0 qjoinKeep:0 ringPeak=1200 ringUnderruns=1 process=game.exe\n"
                "[VideoEncoder] Final packet timeline: target=60000000 us videoEnd=60000000 us "
                "audioMinEnd=60000000 us audioMaxEnd=60000000 us maxPacketDelta=0 us audioPastTarget=0\n"
                "[VideoEncoder] Final metadata durations: target=60000000 us video=60000000 us "
                "audioMin=60000000 us audioMax=60000000 us maxDelta=0 us streams(v=1 a=3) "
                "overload(encoder=0 mux=0) backpressure=0\n"
            ),
        )
        report = classify_session_triage(exported_sync_with_app_underrun)
        assert "started_app_source_underrun" in report["verdicts"]
        assert "ce_audio_timeline_fault" not in report["verdicts"]
        assert report["evidence"]["exported_av_sync_ok"]

        late_app_live_join = make_session(
            "late_app_live_join",
            media=(
                "[AudioLoop] Late app source live join src=9 track=1 process=dx12_av_sync_late.exe "
                "packetStart=358003 trackCursor=358483 joinCursor=358483 suppressedGap=358003 "
                "preservedGap=0 qpcStart=123\n"
                "[PullAudio] Source primed - src=9 track=1 buffered=1200 realBuffered=1200 needed=1200 "
                "lateStart=7459ms\n"
                "[STOP AUDIO] Source 9: track=1 encoded=48000 trim=cov:0 latTotal:0 liveUncat:0 "
                "pad:0 qgap:0 qjoin:358003 qjoinKeep:0 ringPeak=1200 ringUnderruns=0 process=dx12_av_sync_late.exe\n"
            ),
        )
        report = classify_session_triage(late_app_live_join)
        assert "late_app_source_backlog" not in report["verdicts"]
        assert "ce_audio_timeline_fault" not in report["verdicts"]
        assert report["evidence"]["started_app_source_health"]["late_join_live_count"] == 1

        late_non_app_prime = make_session(
            "late_non_app_prime",
            media=(
                "[PullAudio] Source primed - src=0 realBuffered=1389 samples "
                "synthetic(ring=0 inflight=0 post=0) lateStart=4635ms\n"
                "[AudioLoop] Late app source live join src=13 track=1 process=game.exe "
                "packetStart=191057 trackCursor=191520 joinCursor=191520 suppressedGap=191057 "
                "preservedGap=0 qpcStart=123\n"
                "[PullAudio] Source primed - src=13 realBuffered=1457 samples "
                "synthetic(ring=0 inflight=0 post=0) lateStart=3981ms\n"
                "[STOP AUDIO] Source 0: track=3 encoded=48000 trim=cov:0 latTotal:0 liveUncat:0 "
                "cat:0 normal:0 pad:0 qgap:1695 qjoin:0 qjoinKeep:0 ringPeak=20738 "
                "ringUnderruns=0 process=-\n"
                "[STOP AUDIO] Source 13: track=1 encoded=48000 trim=cov:0 latTotal:0 liveUncat:0 "
                "cat:0 normal:0 pad:0 qgap:945 qjoin:191057 qjoinKeep:0 ringPeak=30545 "
                "ringUnderruns=0 process=game.exe\n"
            ),
        )
        report = classify_session_triage(late_non_app_prime)
        assert "late_app_source_backlog" not in report["verdicts"]
        assert report["evidence"]["started_app_source_health"]["late_source_backlog_count"] == 0

        explicit_late_app_prime = make_session(
            "explicit_late_app_prime",
            media=(
                "[PullAudio] Source primed - src=9 realBuffered=1200 samples "
                "synthetic(ring=0 inflight=0 post=0) lateStart=7459ms app=1\n"
            ),
        )
        report = classify_session_triage(explicit_late_app_prime)
        assert "late_app_source_backlog" in report["verdicts"]

        sparse_app_source_silence = make_session(
            "sparse_app_source_silence",
            media=(
                "[PullAudio] App source gap silence: track=1 src=5 buffered=0 requested=400 "
                "target=219800ms encoded=10550000. Source contributes silence for missing range.\n"
                "[PullAudio] WARNING: Source underrun - src 5 padding 400 samples with silence "
                "(available=0 needed=400 forceDrain=0)\n"
                "[STOP AUDIO TRACK] Track 1: encoded=60092400 expected=60092400 diff=+0 (+0.000 ms) "
                "sources=[1,3,5,7]\n"
                "[STOP AUDIO] Source 5: track=1 encoded=60092400 trim=cov:0 latTotal:0 liveUncat:0 "
                "pad:49648177 qgap:480 qjoin:5232636 qjoinKeep:480 ringPeak=0 ringUnderruns=124120 "
                "process=Brave.exe\n"
                "[VideoEncoder] Final packet timeline: target=1251925000 us videoEnd=1251925000 us "
                "audioMinEnd=1251925000 us audioMaxEnd=1251925000 us maxPacketDelta=0 us "
                "streams(v=1 a=3) audioPastTarget=0\n"
            ),
        )
        report = classify_session_triage(sparse_app_source_silence)
        assert "sparse_app_source_silence" in report["verdicts"]
        assert "started_app_source_underrun" not in report["verdicts"]
        assert "ce_audio_timeline_fault" not in report["verdicts"]
        assert report["evidence"]["audio_fault_counts"]["audio_underrun"] == 1
        assert report["evidence"]["strict_audio_fault_counts"]["audio_underrun"] == 0
        assert len(report["evidence"]["started_app_source_health"]["sparse_silence_sources"]) == 1

        app_latency_new_summary = make_session(
            "app_latency_new_summary",
            media=(
                "[STOP AUDIO LATENCY] Source 5 track=1 appAudioDelay avg=226ms max=332ms "
                "targetAvg=141ms excessAvg=85ms excessMax=191ms "
                "buckets(<50/50-150/150-300/300-600/>600ms)=0%/0%/100%/0%/0% "
                ">=150ms=100% drainObservations=60/120 transitions=2 maxComp=0.5000% "
                "queueOverrun=0/0 underruns=0 trims(lat=0 normal=0 cat=0/0). "
                "Lower/more-uniform excess is better; high excess means audio content ran behind video.\n"
                "[VideoEncoder] Final packet timeline: target=51441667 us videoEnd=51441667 us "
                "audioMinEnd=51441667 us audioMaxEnd=51441667 us maxPacketDelta=0 us "
                "streams(v=1 a=3) audioPastTarget=0\n"
            ),
        )
        report = classify_session_triage(app_latency_new_summary)
        assert "audio_app_latency_elevated" in report["verdicts"]
        assert "ce_audio_timeline_fault" not in report["verdicts"]
        assert report["faults"]["audio_app_latency_elevated"]
        assert report["evidence"]["app_audio_latency"]["elevated_source_count"] == 1
        assert report["evidence"]["app_audio_latency"]["worst_excess_avg_ms"] == 85.0

        app_latency_warning_within_slack = make_session(
            "app_latency_warning_within_slack",
            media=(
                "[2026-07-18 23:30:00.000] [INFO] [AppLatency] WARNING: app audio src=9 track=1 "
                "delayMs=289 targetMs=299 excessMs=0 rbAvail=13912 drain=0 reason=within_slack "
                "compDelta=0 comp=0.0000% rateCompActive=0 underruns=0 queuePending=0 "
                "queueOverrun=0/0. Content backlog should drain toward the video target without trims.\n"
                "[STOP AUDIO LATENCY] Source 9 track=1 appAudioDelay avg=243ms max=289ms "
                "targetAvg=292ms excessAvg=1ms excessMax=77ms "
                "buckets(<50/50-150/150-300/300-600/>600ms)=0%/0%/100%/0%/0% "
                ">=150ms=100% drainObservations=1267/47620 transitions=529 maxComp=0.3125% "
                "liveObservations=47620 stopDrainObservations=1 stopDrainAvg=1131ms "
                "stopDrainMax=1131ms queueOverrun=0/0 underruns=0 trims(lat=0 normal=0 cat=0/0). "
                "Lower/more-uniform excess is better; high excess means audio content ran behind video.\n"
                "[VideoEncoder] Final packet timeline: target=396891667 us videoEnd=396891667 us "
                "audioMinEnd=396891666 us audioMaxEnd=396891666 us maxPacketDelta=1 us "
                "streams(v=1 a=3) audioPastTarget=0\n"
            ),
        )
        report = classify_session_triage(app_latency_warning_within_slack)
        assert "audio_app_latency_elevated" not in report["verdicts"]
        assert "app_audio_latency_within_slack" in report["contexts"]
        assert not report["faults"]["audio_app_latency_elevated"]
        assert report["evidence"]["app_audio_latency"]["warning_count"] == 1
        assert report["evidence"]["app_audio_latency"]["elevated_source_count"] == 0
        assert report["evidence"]["app_audio_latency"]["warning_only_context"]
        assert not report["evidence"]["app_audio_latency"]["fault_evidence"]

        app_latency_integrity_fault = make_session(
            "app_latency_integrity_fault",
            media=read_text_if_exists(app_latency_warning_within_slack / "media.log").replace(
                "queueOverrun=0/0 underruns=0 trims(lat=0 normal=0 cat=0/0)",
                "queueOverrun=1/480 underruns=0 trims(lat=0 normal=0 cat=0/0)",
            ),
        )
        report = classify_session_triage(app_latency_integrity_fault)
        assert "audio_app_latency_elevated" in report["verdicts"]
        assert "app_audio_latency_within_slack" not in report["contexts"]
        assert report["faults"]["audio_app_latency_elevated"]
        assert report["evidence"]["app_audio_latency"]["integrity_fault"]
        assert report["evidence"]["app_audio_latency"]["queue_overrun_packets"] == 1
        assert report["evidence"]["app_audio_latency"]["queue_overrun_frames"] == 480

        app_latency_old_fallback = make_session(
            "app_latency_old_fallback",
            media=(
                "[AppLatency] WARNING: app audio src=6 track=2 is 351ms behind video "
                "(rbAvail=16874 samples, draining=0). Read-stall backlog; should drain toward live.\n"
                "[STOP AUDIO LATENCY] Source 6 track=2 appAudioDelay avg=329ms max=527ms "
                "buckets(<50/50-150/150-300/300-600/>600ms)=0%/0%/0%/100%/0% "
                ">=150ms=100% drainingSamples=0/100. Lower/more-uniform is better; high 300-600ms "
                "means audio ran noticeably behind video.\n"
                "[VideoEncoder] Final packet timeline: target=51441667 us videoEnd=51441667 us "
                "audioMinEnd=51441667 us audioMaxEnd=51441667 us maxPacketDelta=0 us "
                "streams(v=1 a=3) audioPastTarget=0\n"
            ),
        )
        report = classify_session_triage(app_latency_old_fallback)
        assert "audio_app_latency_elevated" in report["verdicts"]
        assert "ce_audio_timeline_fault" not in report["verdicts"]
        assert report["evidence"]["app_audio_latency"]["warning_count"] == 1
        assert report["evidence"]["app_audio_latency"]["elevated_source_count"] == 1

        app_latency_stop_drain_context = make_session(
            "app_latency_stop_drain_context",
            media=(
                "[2026-07-17 15:55:00.000] [INFO] [EncoderThread] Recording live (inject)\n"
                "[2026-07-17 15:56:30.690] [INFO] [Media] Stopping recording...\n"
                "[2026-07-17 15:56:31.000] [WARN] [AppLatency] WARNING: app audio src=6 track=2 "
                "delayMs=351 targetMs=60 excessMs=291\n"
                "[2026-07-17 15:56:31.033] [INFO] [STOP AUDIO LATENCY] Source 6 track=2 "
                "appAudioDelay avg=329ms max=527ms buckets(<50/50-150/150-300/300-600/>600ms)="
                "0%/0%/0%/100%/0% >=150ms=100% drainingSamples=0/100\n"
                "[VideoEncoder] Final packet timeline: target=51441667 us videoEnd=51441667 us "
                "audioMinEnd=51441667 us audioMaxEnd=51441667 us maxPacketDelta=0 us "
                "streams(v=1 a=3) audioPastTarget=0\n"
            ),
        )
        report = classify_session_triage(app_latency_stop_drain_context)
        assert "audio_app_latency_elevated" not in report["verdicts"]
        assert "app_audio_stop_drain_latency" in report["contexts"]
        assert report["evidence"]["app_audio_latency"]["warning_count"] == 0
        assert report["evidence"]["app_audio_latency"]["stop_drain_warning_count"] == 1

        crash_session = make_session(
            "crash_session",
            media=(
                "[VideoEncoder] Final metadata durations: target=1000 us video=1000 us "
                "audioMin=1000 us audioMax=1000 us maxDelta=0 us streams(v=1 a=2) "
                "overload(encoder=0 mux=0) backpressure=0 peakMux=0KB peakPkts=0\n"
            ),
        )
        (crash_session / "crash.log").write_text("[03:34:44] CRASH DETECTED - Handling exception\n", encoding="utf-8")
        report = classify_session_triage(crash_session)
        assert "ce_process_crash" in report["verdicts"]
        assert report["faults"]["ce_process_crash"]

        audio_fault = make_session("audio_fault", media="[PullAudio] WARNING: Source underrun track=1\n")
        report = classify_session_triage(audio_fault)
        assert "ce_audio_timeline_fault" in report["verdicts"]

        stop_tail_padding = make_session(
            "stop_tail_padding",
            media=(
                "[PullAudio] WARNING: Source underrun - src 2 padding 800 samples with silence "
                "(available=0 needed=800 forceDrain=1)\n"
            ),
        )
        report = classify_session_triage(stop_tail_padding)
        assert "ce_audio_timeline_fault" not in report["verdicts"]
        assert report["evidence"]["log_counts"]["audio_stop_tail_padding"] == 1

        historical_stop_force_drain_backlog = make_session(
            "historical_stop_force_drain_backlog",
            media=(
                "[AppDrain] state src=11 track=1 active=0 reason=force_drain delayMs=2830 "
                "targetMs=142 excessMs=2688 rb=135868 target=6816 delta=0 comp=0.0000% "
                "forceDrain=1 startupSettled=1 startupProtected=0\n"
                "[PullAudio] WARNING: Extreme drift detected (129052 samples src=11) - may "
                "indicate sync issue\n"
                "[VideoEncoder] Final packet timeline: target=1000000 us videoEnd=1000000 us "
                "audioMinEnd=1000000 us audioMaxEnd=1000000 us maxPacketDelta=0 us "
                "streams(v=1 a=1) audioPastTarget=0\n"
            ),
        )
        report = classify_session_triage(historical_stop_force_drain_backlog)
        assert report["evidence"]["log_counts"]["audio_extreme_drift"] == 0
        assert report["evidence"]["log_counts"]["audio_stop_force_drain_backlog"] == 1
        assert "audio_stop_force_drain_backlog" in report["contexts"]
        assert "ce_audio_timeline_fault" not in report["verdicts"]

        current_stop_force_drain_counts = analyze_log_text(
            "[PullAudio] Stop force-drain backlog: drift=129052 samples src=11 forceDrain=1 "
            "(post-target backlog is excluded from output)\n"
        )["counts"]
        assert current_stop_force_drain_counts["audio_extreme_drift"] == 0
        assert current_stop_force_drain_counts["audio_stop_force_drain_backlog"] == 1

        live_extreme_drift = make_session(
            "live_extreme_drift",
            media=(
                "[AppDrain] state src=11 track=1 active=1 reason=excess_backlog delayMs=2830 "
                "targetMs=142 excessMs=2688 rb=135868 target=6816 delta=240 comp=0.5000% "
                "forceDrain=0 startupSettled=1 startupProtected=0\n"
                "[PullAudio] WARNING: Extreme drift detected (129052 samples src=11) "
                "forceDrain=0 - may indicate sync issue\n"
            ),
        )
        report = classify_session_triage(live_extreme_drift)
        assert report["evidence"]["log_counts"]["audio_extreme_drift"] == 1
        assert report["evidence"]["log_counts"]["audio_stop_force_drain_backlog"] == 0
        assert "ce_audio_timeline_fault" in report["verdicts"]

        legacy_unstructured_extreme_drift = analyze_log_text(
            "[PullAudio] WARNING: Extreme drift detected - legacy diagnostic\n"
        )["counts"]
        assert legacy_unstructured_extreme_drift["audio_extreme_drift"] == 1
        assert legacy_unstructured_extreme_drift["audio_stop_force_drain_backlog"] == 0

        zero_drift = make_session(
            "zero_drift",
            media=(
                "[A/V ZERO DRIFT WARNING] Track 1 residual_samples=+1 residual_us=+21 "
                "target_samples=96000 cursor_samples=96001 target_us=2000000 cursor_us=2000021\n"
            ),
        )
        report = classify_session_triage(zero_drift)
        assert "ce_audio_timeline_fault" in report["verdicts"]
        assert report["evidence"]["zero_drift_warnings"][0]["residual_samples"] == 1

        one_us = make_session(
            "one_us",
            media=(
                "[VideoEncoder] WARNING: Post-mux audio duration mismatch "
                "(target=48266667 audioMinEnd=48266666 audioMaxEnd=48266666 maxDelta=1)\n"
            ),
        )
        report = classify_session_triage(one_us)
        assert "ce_audio_timeline_fault" not in report["verdicts"]
        assert report["evidence"]["rounding_evidence"]["post_mux_one_us_or_less_is_info"]

        multi_recording = root / "multi_recording"
        multi_recording.mkdir()
        (multi_recording / "captureengine.log").write_text(
            "[Controller] Starting recording...\n[Controller] Starting recording...\n", encoding="utf-8"
        )
        for recording_id, pid in (("r0001", 101), ("r0002", 202)):
            media_name = f"media_{recording_id}_{pid}.log"
            media_text = "[Media] Starting recording...\n"
            if recording_id == "r0001":
                media_text += (
                    "[2026-07-18 12:00:00.000] [INFO] [AVSyncApply] liveStartQpc=1000000\n"
                    "[2026-07-18 12:00:02.000] [INFO] [Media] Stopping recording...\n"
                )
            (multi_recording / media_name).write_text(media_text, encoding="utf-8")
            (multi_recording / f"recording_{recording_id}_{pid}.manifest").write_text(
                f"recording_id={recording_id}\nmedia_pid={pid}\nmedia_log={media_name}\n", encoding="utf-8"
            )
        (multi_recording / "perf_metrics_42.csv").write_text(
            "qpc_us,qpc_delta_us,total_us,capture_us,present_call_us,mux_queue_kb,overload_flags\n"
            "1100000,1000,0,0,0,0,0\n"
            "1500000,400000,0,0,0,0,0\n"
            "5000000,3500000,0,0,0,0,0\n",
            encoding="utf-8",
        )
        try:
            classify_session_triage(multi_recording)
            raise AssertionError("ambiguous multi-recording session was silently accepted")
        except ValueError as exc:
            assert "multiple recordings" in str(exc)
        report = classify_session_triage(multi_recording, recording_id="r0001")
        assert report["recording_id"] == "r0001"
        assert report["media_pid"] == 101
        assert not report["evidence"]["recording_evidence_incomplete"]
        assert report["recording_window"]["reason"] == "derived_selected_recording_bounds"
        assert report["evidence"]["max_present_gap_ms"] == 400.0

        overwritten_legacy = root / "overwritten_legacy"
        overwritten_legacy.mkdir()
        (overwritten_legacy / "captureengine.log").write_text(
            "[Controller] Starting recording...\n[Controller] Starting recording...\n", encoding="utf-8"
        )
        (overwritten_legacy / "media.log").write_text("[Media] Starting recording...\n", encoding="utf-8")
        report = classify_session_triage(overwritten_legacy)
        assert "recording_evidence_missing_or_overwritten" in report["contexts"]
        assert report["evidence"]["controller_recording_start_count"] == 2
        assert report["evidence"]["discovered_recording_evidence_count"] == 1

        analyzer_globals = globals()
        original_exact_analyzer = analyzer_globals["analyze_completed_capture_exact"]
        original_metadata_analyzer = analyzer_globals["analyze_completed_capture_metadata"]
        completed_capture_calls = []

        def fake_exact_analyzer(_ffprobe, _ffmpeg, _capture_path, _threshold):
            completed_capture_calls.append("exact")
            return {"analysis_mode": "exact"}

        def fake_metadata_analyzer(_ffprobe, _capture_path):
            completed_capture_calls.append("metadata")
            return {"analysis_mode": "metadata"}

        try:
            analyzer_globals["analyze_completed_capture_exact"] = fake_exact_analyzer
            analyzer_globals["analyze_completed_capture_metadata"] = fake_metadata_analyzer
            metadata_dispatch = analyze_completed_capture(
                Path("ffprobe"), Path("ffmpeg"), Path("capture.mkv"), False
            )
            exact_dispatch = analyze_completed_capture(
                Path("ffprobe"), Path("ffmpeg"), Path("capture.mkv"), True
            )
        finally:
            analyzer_globals["analyze_completed_capture_exact"] = original_exact_analyzer
            analyzer_globals["analyze_completed_capture_metadata"] = original_metadata_analyzer
        assert metadata_dispatch["analysis_mode"] == "metadata"
        assert exact_dispatch["analysis_mode"] == "exact"
        assert completed_capture_calls == ["metadata", "exact"]

        metadata_attachment = {
            "analysis_mode": "metadata",
            "authoritative": False,
            "all_tracks_exact": None,
            "endpoint_durations_identical": None,
            "cfr_packet_coverage_exact": None,
        }
        metadata_report = {
            "verdicts": ["unknown"],
            "faults": {"audio_timeline": False, "visual_timeline": False},
        }
        attach_completed_capture_report(metadata_report, metadata_attachment)
        assert metadata_report["completed_capture"] is metadata_attachment
        assert metadata_report["verdicts"] == ["unknown"]
        assert not metadata_report["faults"]["audio_timeline"]
        assert not metadata_report["faults"]["visual_timeline"]
