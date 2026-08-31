if False:

        dxgi_desktop_source_limited = make_session(
            "dxgi_desktop_source_limited",
            capture_method="dxgi_dup",
            media=(
                "[WGC CFR SUMMARY] Live=47626 Dup=31350 DupPct=65.8% NoFresh=658pm NoReserve=972pm "
                "DupReason(src=31347 def=0 timer=3 drain=0) SourceLimitedRepeats=31347 StarvedEpisodes=8 "
                "AntiFreezeFloor=1 AntiFreezeFloorSkippedSync=6130 BiasClampCount=0 "
                "longest=236828ms longestDup=19031 longestContiguousDup=15 (125ms) worstIn=8 worstDel=8\n"
                "[WGC CFR SMOOTHNESS SUMMARY] encoderLimitedDrops=0 maxDropTicks=0 cadenceEvents=394 "
                "phaseErrorMax=274760us shortfallMax=0.0ms staleDebtDrops=32 liveRebase=0/0 "
                "tooNewRepeats=31350 syncDelayHolds=31350 tooNewLeadMax=0us avDelay=28.5ms "
                "startupDelay=256.5ms scheduleOffset=227966us effectiveDelay=256.5ms "
                "lowSourceBypass=0 modeMismatch=0 sourceBacktrack=0 "
                "syncDelaySourceLimitedHolds=31350 syncDelayPolicyHolds=0 startupReserveFrames=11 "
                "startupReserveSpan=256479us startupDelayTarget=328511us startupReserveSelected=0 "
                "startupReserveReason=partial_span_timeout smoothBuf=1 smoothTargetMs=300 "
                "smoothFrames=27/45/45 smoothDelay=228.0ms smoothPoolSlots=64 sourceFramePoolBuffers=0 "
                "budgetSurfaces=94 syncFrames=5 extraFrames=45 retainedCap=58 reservedFreeSlots=6 "
                "safetySlots=4 retainedCapTrim=0 ingressAccepted=16745 ingressDecimated=0 "
                "ingressPlaySoft=0 ingressPlayCredit=0 ingressRetained=30/58 ingressLowWater=31 "
                "leasedMax=39 freeNow=64 freeMin=25 poolPressureTrim=0 poolSaturatedDrops=0 "
                "overwritePrevented=0 leaseMismatches=0 smoothVramMB=2025.0 smoothCapLimited=0 "
                "smoothReason=startup_attempt_source_rate_low\n"
                "[WGC CFR SMOOTHNESS BUFFER] smoothTargetDelay=299998us smoothActualDelay=227966us "
                "smoothDelayDeficit=72032us startupDelayTarget=328511us effectiveDelay=256479us "
                "startupDelayDeficit=72032us finalAvSync=exported_tracks_authoritative\n"
                "[WGC CFR SMOOTHNESS DELAY] delayReservoirLowWaterFrames=31 delayReservoirTargetFrames=32 "
                "delayReservoirLowWaterTicks=46304 realizedDelayAvg=253512us realizedDelayMin=229874us "
                "realizedDelayMax=261339us delayResidualAvg=2966/3255us delayResidualMax=26605us "
                "delayResidualP95=7000us delayResidualLateMax=26605us delayResidualEarlyMax=4860us "
                "rawResidualAvg=3360/5376us rawResidualMax=29286us rawResidualP95=11000us "
                "rawResidualLateMax=29286us rawResidualEarlyMax=10746us "
                "predictedResidualAvg=2966/3255us predictedResidualP95=7000us "
                "predictedResidualLateMax=26605us rawMinusPredictedAvg=393/393us "
                "rawMinusPredictedMax=6250us\n"
                "[WGC CFR SMOOTHNESS REPEAT] delaySoftLateAccepted=0 delayNearCapAccepted=1 "
                "delayRepeatSoftSafeCandidate=0 delaySyncProtectedRepeats=31350 "
                "delayPostSelectionRejectedSync=0\n"
                "[WGC CFR SMOOTHNESS VERDICT] delayPostSelectionRejectedSync=0 "
                "delayPostSelectionRescuedSync=0 sourceRepeatLowerBound=31350 excessRepeats=0 "
                "policyAddedRepeats=0 excessRepeatClusters=0 excessRepeatClusterMax=0 "
                "smoothnessNotMaximal=0 mixedPolicyFault=0 syncSourceRepeatLowerBound=31350 "
                "deliveryRepeatLowerBound=31347 policyNoSourceRepeats=0\n"
                "[WGC CFR QUALITY] duplicatePct=65.8 duplicates=31350/47626 worst1sUnique=10 "
                "worst1sRepeats=122 worst1sEmit=132 limiter=source_limited sourceLimitedRepeats=31347 "
                "poolPressure=0 freeMin=25 poolSaturatedDrops=0 ingressHard=0 ingressSoft=0 "
                "ingressDecimated=0 policyAddedRepeats=0 excessRepeats=0 smoothDelayDeficitUs=72032 "
                "startupDelayDeficitUs=72032 encoderOverload=0x0 muxBackpressure=0 backend=dxgi_dup "
                "finalAvSync=exported_tracks_authoritative\n"
                "[WGC CFR SOURCE COVERAGE] coverage=limited reason=source_and_delivery_holes bestEffort=1 "
                "outputFps=120 duplicates=31350/47626 sourceLimitedRepeats=31347 "
                "sourceRepeatLowerBound=31350 syncSourceRepeatLowerBound=31350 "
                "deliveryRepeatLowerBound=31347 excessRepeats=0 policyAddedRepeats=0 "
                "policyNoSourceRepeats=0 cleanEncoderMux=1 cleanPool=1 cleanSelection=1 "
                "encoderOverload=0x0 muxBackpressure=0 poolPressure=0 poolFreeMin=25 "
                "finalAvSync=exported_tracks_authoritative\n"
                "[VideoEncoder] Final packet timeline: target=396891667 us videoEnd=396891667 us "
                "audioMinEnd=396891666 us audioMaxEnd=396891666 us maxPacketDelta=1 us "
                "streams(v=1 a=3) audioPastTarget=0\n"
                "[VideoEncoder] Final metadata durations: target=396891667 us video=396891667 us "
                "audioMin=396891666 us audioMax=396891666 us maxDelta=1 us streams(v=1 a=3) "
                "overload(encoder=0 mux=0) backpressure=0\n"
            ),
        )
        report = classify_session_triage(dxgi_desktop_source_limited)
        assert "dxgi_dup_source_starvation" in report["verdicts"]
        assert "dxgi_dup_source_coverage_best_effort" in report["verdicts"]
        assert "dxgi_dup_source_limited_delay_variation" in report["contexts"]
        assert "dxgi_dup_av_sync_delay_residual" not in report["verdicts"]
        assert "dxgi_dup_audio_late_risk" not in report["verdicts"]
        assert "dxgi_dup_active_delay_realized_delay_unstable" not in report["verdicts"]
        assert "ce_visual_timeline_fault" not in report["verdicts"]
        assert not report["faults"]["visual_timeline"]
        assert report["faults"]["wgc_clean_source_limited_coverage"]
        assert report["faults"]["wgc_source_limited_delay_variation_context"]

        near_lower_bound_evidence = parse_media_triage(
            read_text_if_exists(dxgi_variable_fps_source_limited / "media.log")
        )
        near_lower_bound_summary = near_lower_bound_evidence["wgc_smoothness_summary"][0]
        near_lower_bound_summary.update(
            {
                "live": 0,
                "source_repeat_lower_bound": 107,
                "excess_repeats": 5,
                "realized_delay_min_us": 42497,
                "realized_delay_max_us": 56283,
            }
        )
        near_lower_bound_coverage = near_lower_bound_evidence["wgc_source_coverage"][0]
        near_lower_bound_coverage.update(
            {
                "best_effort": 0,
                "duplicates": 112,
                "live": 16117,
                "source_repeat_lower_bound": 107,
                "excess_repeats": 5,
                "clean_selection": 0,
            }
        )
        assert not has_wgc_clean_source_limited_coverage(near_lower_bound_evidence)
        assert has_wgc_source_limited_playout_maximal(near_lower_bound_evidence)
        assert has_wgc_source_limited_delay_variation_context(near_lower_bound_evidence)
        assert not has_wgc_active_delay_realized_delay_unstable(near_lower_bound_evidence)

        mixed_content_delay_evidence = parse_media_triage(
            read_text_if_exists(dxgi_variable_fps_source_limited / "media.log")
        )
        mixed_content_delay_summary = mixed_content_delay_evidence["wgc_smoothness_summary"][0]
        mixed_content_delay_summary.update(
            {
                "delay_near_cap_accepted": 22,
                "realized_delay_min_us": 38133,
                "realized_delay_max_us": 53972,
            }
        )
        mixed_near_cap_samples = (
            (1, 2),
            (45, 1),
            (46, 1),
            (170, 1),
            (174, 1),
            (360, 1),
            (373, 1),
            (406, 1),
            (420, 1),
            (750, 1),
            (953, 1),
            (955, 2),
            (960, 1),
            (964, 1),
            (967, 1),
            (968, 1),
            (970, 1),
            (972, 3),
        )

        def parse_near_cap_windows(samples):
            lines = []
            for second, count in samples:
                hour = second // 3600
                minute = (second % 3600) // 60
                second_of_minute = second % 60
                lines.append(
                    f"[2026-07-19 {hour:02d}:{minute:02d}:{second_of_minute:02d}.000] [INFO] "
                    f"[WGC CFR CADENCE EVENT] mode=normal_pressure nearCap={count}"
                )
            return parse_media_triage("\n".join(lines))["wgc_cadence_events"]

        mixed_content_delay_evidence["wgc_cadence_events"] = parse_near_cap_windows(
            mixed_near_cap_samples
        )
        assert mixed_content_delay_evidence["wgc_cadence_events"][0]["timestamp_us"] == 1000000
        mixed_window_pressure = wgc_near_cap_window_pressure(mixed_content_delay_evidence)
        assert mixed_window_pressure["accepted_total"] == 22
        assert mixed_window_pressure["max_accepted"] == 7
        assert wgc_near_cap_acceptance_is_isolated(
            mixed_content_delay_evidence, mixed_content_delay_summary
        )
        assert has_wgc_source_limited_delay_variation_context(mixed_content_delay_evidence)
        assert not has_wgc_active_delay_realized_delay_unstable(mixed_content_delay_evidence)
        mixed_content_delay_evidence["wgc_cadence_events"].pop()
        assert not wgc_near_cap_acceptance_is_isolated(
            mixed_content_delay_evidence, mixed_content_delay_summary
        )

        sustained_delay_evidence = parse_media_triage(
            read_text_if_exists(dxgi_variable_fps_source_limited / "media.log")
        )
        sustained_delay_summary = sustained_delay_evidence["wgc_smoothness_summary"][0]
        sustained_delay_summary.update(
            {
                "delay_near_cap_accepted": 12,
                "realized_delay_min_us": 38133,
                "realized_delay_max_us": 53972,
            }
        )
        sustained_delay_evidence["wgc_cadence_events"] = parse_near_cap_windows(
            (second, 2) for second in range(100, 106)
        )
        sustained_window_pressure = wgc_near_cap_window_pressure(sustained_delay_evidence)
        assert sustained_window_pressure["accepted_total"] == 12
        assert sustained_window_pressure["max_accepted"] == 12
        assert not wgc_near_cap_acceptance_is_isolated(sustained_delay_evidence, sustained_delay_summary)
        assert not has_wgc_source_limited_delay_variation_context(sustained_delay_evidence)
        assert has_wgc_active_delay_realized_delay_unstable(sustained_delay_evidence)

        wgc_pool_lifetime_fault = make_session(
            "wgc_pool_lifetime_fault",
            media=(
                "[WGC CFR SMOOTHNESS SUMMARY] encoderLimitedDrops=0 maxDropTicks=0 cadenceEvents=0 "
                "phaseErrorMax=0us shortfallMax=0.0ms staleDebtDrops=0 liveRebase=0/0 tooNewRepeats=0 "
                "syncDelayHolds=0 tooNewLeadMax=0us avDelay=33.3ms startupDelay=166.6ms "
                "scheduleOffset=0us effectiveDelay=166.6ms lowSourceBypass=0 modeMismatch=0 "
                "sourceBacktrack=0 syncDelaySourceLimitedHolds=0 syncDelayPolicyHolds=0 "
                "startupReserveFrames=24 startupReserveSpan=166000us startupDelayTarget=166600us "
                "startupReserveSelected=1 startupReserveReason=selected smoothBuf=1 smoothTargetMs=250 "
                "smoothFrames=16/16/30 smoothDelay=133.3ms smoothPoolSlots=24 sourceFramePoolBuffers=8 "
                "budgetSurfaces=32 syncFrames=4 safetySlots=4 leasedMax=20 freeMin=4 "
                "poolSaturatedDrops=0 overwritePrevented=0 leaseMismatches=1 smoothVramMB=2025.0 "
                "smoothCapLimited=1 smoothReason=vram_cap_limited\n"
            ),
        )
        report = classify_session_triage(wgc_pool_lifetime_fault)
        assert "wgc_pool_slot_lifetime_fault" in report["verdicts"]
        assert "ce_visual_timeline_fault" in report["verdicts"]
        assert report["faults"]["wgc_pool_slot_lifetime_fault"]

        wgc_pool_evidence_missing = make_session(
            "wgc_pool_evidence_missing",
            media=(
                "[WGC CFR SMOOTHNESS SUMMARY] encoderLimitedDrops=0 maxDropTicks=0 cadenceEvents=0 "
                "phaseErrorMax=0us shortfallMax=0.0ms staleDebtDrops=0 liveRebase=0/0 tooNewRepeats=0 "
                "syncDelayHolds=0 tooNewLeadMax=0us avDelay=33.3ms startupDelay=166.6ms "
                "scheduleOffset=0us effectiveDelay=166.6ms lowSourceBypass=0 modeMismatch=0 "
                "sourceBacktrack=0 syncDelaySourceLimitedHolds=0 syncDelayPolicyHolds=0 "
                "startupReserveFrames=20 startupReserveSpan=166000us startupDelayTarget=166600us "
                "startupReserveSelected=1 startupReserveReason=selected delayReservoirLowWaterFrames=4 "
                "delayReservoirTargetFrames=5 delayReservoirLowWaterTicks=0 realizedDelayAvg=166600us "
                "realizedDelayMin=165000us realizedDelayMax=168000us delayResidualAvg=0/1000us "
                "delayResidualMax=2000us delayResidualP95=1000us delayResidualLateMax=1000us "
                "delayResidualEarlyMax=1000us smoothBuf=1 smoothTargetMs=250 "
                "smoothFrames=16/16/30 smoothDelay=133.3ms smoothPoolSlots=20 smoothVramMB=2025.0 "
                "smoothCapLimited=1 smoothReason=vram_cap_limited\n"
                "[VideoEncoder] Final metadata durations: target=1000 us video=1000 us audioMin=1000 "
                "us audioMax=1000 us maxDelta=0 us streams(v=1 a=2) overload(encoder=0 mux=0) "
                "backpressure=0 peakMux=0KB peakPkts=0\n"
            ),
        )
        report = classify_session_triage(wgc_pool_evidence_missing)
        assert "wgc_pool_evidence_missing" in report["verdicts"]
        assert "ce_encoder_or_mux_backpressure" not in report["verdicts"]

        source_gap_windowed = make_session(
            "source_gap_windowed",
            media=(
                "[AVSyncApply] wgc_schedule_anchor: videoQpc=9700000 audioAnchorQpc=9700000 "
                "liveStartQpc=10000000 requestedDelayUs=0 startupDelayUs=0 scheduleOffsetUs=0\n"
                "[VideoEncoder] Final metadata durations: target=1000 us video=1000 us audioMin=1000 "
                "us audioMax=1000 us maxDelta=0 us streams(v=1 a=2) overload(encoder=0 mux=0) "
                "backpressure=0 peakMux=0KB peakPkts=0\n"
            ),
            hook="DetourPresent: heartbeat #1 gap=299ms presentOwner=0x0000 depth=0 slFG=0 tid=0x1234\n",
            perf=(
                "frame,qpc_us,total_us,capture_us,present_call_us,mux_queue_kb,overload_flags,api,qpc_delta_us\n"
                "1,25990000,100,20,40,0,0,DX12,0\n"
                "2,26000000,100,20,40,0,0,DX12,10000\n"
                "3,26010000,100,20,40,0,0,DX12,10000\n"
            ),
        )
        report = classify_session_triage(source_gap_windowed, recording_window="25:45")
        assert "source_present_gap" not in report["verdicts"]
        assert report["evidence"]["present_gap_source"] == "perf_recording_window"
        assert report["evidence"]["max_present_gap_ms"] <= 11.0

        window_audio_noise = make_session(
            "window_audio_noise",
            media=(
                "[2026-01-01 00:00:00.000] [INFO] [EncoderThread] Recording live "
                "(WGC, hiddenFrames=0, bufferedInject=0)\n"
                "[2026-01-01 00:00:00.100] [INFO] [AVSyncApply] wgc_schedule_anchor: "
                "videoQpc=99900000 audioAnchorQpc=100000000 liveStartQpc=100000000 "
                "requestedDelayUs=1000 startupDelayUs=1000 scheduleOffsetUs=0 "
                "selectionOffsetUs=0 renderDelayUs=1000 smoothExtraDelayUs=0 confidence=high reason=test\n"
                "[2026-01-01 00:00:02.500] [INFO] [Media] [WGC CFR] Source-starved episode: "
                "duration=120ms out=14 dup=12 minIn=60 minDel=120 freshMiss=1000pm minBuf=0\n"
                "[2026-01-01 00:00:05.500] [INFO] [Media] [PullAudio] WARNING: Source underrun - "
                "src 4 padding 400 samples with silence (available=0 needed=400 forceDrain=0)\n"
                "[2026-01-01 00:00:05.600] [INFO] [Media] [A/V ZERO DRIFT WARNING] Track 1 "
                "residual_samples=-480 residual_us=-10000 target_samples=4800 cursor_samples=4320\n"
                "[2026-01-01 00:00:06.000] [INFO] [Media] [VideoEncoder] Final packet timeline: "
                "target=6000000 us videoEnd=6000000 us audioMinEnd=6000000 us audioMaxEnd=6000000 us "
                "maxPacketDelta=0 us streams(v=1 a=1) audioPastTarget=0\n"
                "[2026-01-01 00:00:06.000] [INFO] [Media] [VideoEncoder] Final metadata durations: "
                "target=6000000 us video=6000000 us audioMin=6000000 us audioMax=6000000 us "
                "maxDelta=0 us streams(v=1 a=1) overload(encoder=0 mux=0) backpressure=0\n"
            ),
            perf=(
                "frame,qpc_us,total_us,capture_us,present_call_us,mux_queue_kb,overload_flags,api\n"
                "1,102000000,100,20,40,0,0,DX12\n"
                "2,103000000,100,20,40,0,0,DX12\n"
            ),
        )
        report = classify_session_triage(window_audio_noise, recording_window="2:4")
        assert report["evidence"]["recording_window"]["active"]
        assert "wgc_source_starvation" in report["verdicts"]
        assert "started_app_source_underrun" not in report["verdicts"]
        assert "ce_audio_timeline_fault" not in report["verdicts"]
        assert report["evidence"]["final_packet_timelines"][0]["max_packet_delta_us"] == 0

        wgc_starved = make_session(
            "wgc_starved",
            media=(
                "[WGC CFR] Source-starved episode: duration=1016ms out=121 dup=34 minIn=4 "
                "minDel=4 freshMiss=465pm minBuf=0\n"
                "[WGC CFR SUMMARY] Live=5791 Dup=151 DupPct=2.6% NoFresh=10pm NoReserve=0pm "
                "DupReason(src=151 def=0 timer=0 drain=0) SourceLimitedRepeats=151 "
                "StarvedEpisodes=319 longest=1109ms longestDup=34 worstIn=4 worstDel=4\n"
                "[VideoEncoder] Final metadata durations: target=1000 us video=1000 us "
                "audioMin=1000 us audioMax=1000 us maxDelta=0 us streams(v=1 a=2) "
                "overload(encoder=0 mux=0) backpressure=0 peakMux=0KB peakPkts=0\n"
            ),
        )
        report = classify_session_triage(wgc_starved)
        assert "wgc_source_starvation" in report["verdicts"]
        assert "ce_encoder_or_mux_backpressure" not in report["verdicts"]
        assert report["evidence"]["wgc_source_limits"]["detail_episode_count"] == 1
        assert report["evidence"]["wgc_source_limits"]["summary_starved_episodes"] == 319
        assert report["evidence"]["wgc_source_limits"]["summary_source_limited_repeats"] == 151

        wgc_false_capacity_label = make_session(
            "wgc_false_capacity_label",
            media=(
                "[WGC CFR ATTRIBUTION] fault_hint=ce_capacity_pressure qpc=1..2 duration=922ms out=110 dup=12 "
                "minIn=84 minDel=84 freshMiss=1000pm minBuf=1 cbGapMax=110189us encEmaMax=0.31ms "
                "muxBp=0 waitMax=0us muxMax=0KB overload=0x0 copyMax=1us copyHealth=ok fenceMax=0us "
                "fenceHealth=ok\n"
                "[WGC CFR SMOOTHNESS SUMMARY] encoderLimitedDrops=4 maxDropTicks=1 cadenceEvents=1 "
                "phaseErrorMax=0us shortfallMax=50.0ms staleDebtDrops=0 liveRebase=0/4 tooNewRepeats=0 "
                "syncDelayHolds=0 tooNewLeadMax=0us avDelay=29.4ms startupDelay=29.4ms "
                "scheduleOffset=0us effectiveDelay=29.4ms lowSourceBypass=0 modeMismatch=0 sourceBacktrack=0 "
                "syncDelaySourceLimitedHolds=0 syncDelayPolicyHolds=0 startupReserveFrames=4 "
                "startupReserveSpan=29000us startupDelayTarget=29400us startupReserveSelected=1 "
                "startupReserveReason=selected\n"
                "[VideoEncoder] Final metadata durations: target=1000 us video=1000 us audioMin=1000 "
                "us audioMax=1000 us maxDelta=0 us streams(v=1 a=2) overload(encoder=0 mux=0) "
                "backpressure=0 peakMux=0KB peakPkts=0\n"
            ),
        )
        report = classify_session_triage(wgc_false_capacity_label)
        assert "ce_encoder_or_mux_backpressure" not in report["verdicts"]
        assert "wgc_encoder_limited_judder" not in report["verdicts"]

        wgc_delivery_gap = make_session(
            "wgc_delivery_gap",
            media=(
                "[WGC CFR ATTRIBUTION] fault_hint=wgc_delivery_gap qpc=1..2 duration=250ms out=30 dup=12 "
                "minIn=120 minDel=84 freshMiss=400pm minBuf=2 cbGapMax=48000us encEmaMax=0.31ms "
                "muxBp=0 waitMax=0us muxMax=0KB overload=0x0 copyMax=1us copyHealth=ok fenceMax=0us "
                "fenceHealth=ok poolSat=0 overwritePrevented=0 ingressDecimated=0\n"
                "[VideoEncoder] Final metadata durations: target=1000 us video=1000 us audioMin=1000 "
                "us audioMax=1000 us maxDelta=0 us streams(v=1 a=2) overload(encoder=0 mux=0) "
                "backpressure=0 peakMux=0KB peakPkts=0\n"
            ),
        )
        report = classify_session_triage(wgc_delivery_gap)
        assert "wgc_delivery_gap" in report["verdicts"]
        assert "wgc_framepool_pressure" not in report["verdicts"]
        assert "ce_encoder_or_mux_backpressure" not in report["verdicts"]
        assert "ce_visual_timeline_fault" not in report["verdicts"]

        wgc_framepool_pressure = make_session(
            "wgc_framepool_pressure",
            media=(
                "[WGC CFR ATTRIBUTION] fault_hint=wgc_framepool_pressure qpc=1..2 duration=250ms out=30 dup=12 "
                "minIn=120 minDel=84 freshMiss=400pm minBuf=2 cbGapMax=48000us encEmaMax=0.31ms "
                "muxBp=0 waitMax=0us muxMax=0KB overload=0x0 copyMax=1us copyHealth=ok fenceMax=0us "
                "fenceHealth=ok poolSat=1 overwritePrevented=0 ingressDecimated=0\n"
                "[VideoEncoder] Final metadata durations: target=1000 us video=1000 us audioMin=1000 "
                "us audioMax=1000 us maxDelta=0 us streams(v=1 a=2) overload(encoder=0 mux=0) "
                "backpressure=0 peakMux=0KB peakPkts=0\n"
            ),
        )
        report = classify_session_triage(wgc_framepool_pressure)
        assert "wgc_framepool_pressure" in report["verdicts"]
        assert "ce_visual_timeline_fault" in report["verdicts"]
        assert "ce_encoder_or_mux_backpressure" not in report["verdicts"]

        wgc_delivery_limited_lower_bound = make_session(
            "wgc_delivery_limited_lower_bound",
            media=(
                "[WGC CFR SUMMARY] Live=1000 Dup=120 DupPct=12.0% NoFresh=120pm NoReserve=0pm "
                "DupReason(src=120 def=0 timer=0 drain=0) SourceLimitedRepeats=120 StarvedEpisodes=0 "
                "longest=0ms longestDup=0 worstIn=120 worstDel=84\n"
                "[WGC CFR SMOOTHNESS SUMMARY] encoderLimitedDrops=0 maxDropTicks=0 cadenceEvents=0 "
                "phaseErrorMax=0us shortfallMax=0.0ms staleDebtDrops=0 liveRebase=0/0 tooNewRepeats=0 "
                "syncDelayHolds=0 tooNewLeadMax=0us avDelay=0.0ms startupDelay=0.0ms "
                "scheduleOffset=0us effectiveDelay=0.0ms lowSourceBypass=0 modeMismatch=0 sourceBacktrack=0 "
                "syncDelaySourceLimitedHolds=0 syncDelayPolicyHolds=0 startupReserveFrames=0 "
                "startupReserveSpan=0us startupDelayTarget=0us startupReserveSelected=0 "
                "startupReserveReason=inactive\n"
                "[WGC CFR SMOOTHNESS VERDICT] delayPostSelectionRejectedSync=0 "
                "delayPostSelectionRescuedSync=0 sourceRepeatLowerBound=120 excessRepeats=0 "
                "policyAddedRepeats=0 excessRepeatClusters=0 excessRepeatClusterMax=0 "
                "smoothnessNotMaximal=0 mixedPolicyFault=0 syncSourceRepeatLowerBound=0 "
                "deliveryRepeatLowerBound=120\n"
                "[VideoEncoder] Final metadata durations: target=1000 us video=1000 us audioMin=1000 "
                "us audioMax=1000 us maxDelta=0 us streams(v=1 a=2) overload(encoder=0 mux=0) "
                "backpressure=0 peakMux=0KB peakPkts=0\n"
            ),
        )
        report = classify_session_triage(wgc_delivery_limited_lower_bound)
        smoothness = report["evidence"]["wgc_smoothness_summary"][0]
        assert smoothness["source_repeat_lower_bound"] == 120
        assert smoothness["sync_source_repeat_lower_bound"] == 0
        assert smoothness["delivery_repeat_lower_bound"] == 120
        assert smoothness["policy_added_repeats"] == 0
        assert "wgc_cfr_smoothness_not_maximal" not in report["verdicts"]
        assert "ce_visual_timeline_fault" not in report["verdicts"]

        inject_pacer = make_session(
            "inject_pacer",
            media=(
                "[Inject Perf] Input: 160 | Queued: 75 | DropFull: 0 | DropPace: 85 | PubFps: 240 | HostQ: 0 | "
                "EncQ: 0 | Dup: 5 | Late: 0 | Trim: 0 | SelDrop: 20 | Def: 0 | Encode: 308us | Fence: 2us | "
                "Mux: 0KB | Overload: 0x0\n"
                "[Inject CFR SUMMARY] Live=582 Dup=48 DupPct=8.2% DupReason(src=48 def=0 timer=0 drain=0) "
                "FreshCatchup=3 RepeatCatchup=1 StaleTrim=7 Recovery=0/1 PathMismatch=0/0 DefRequeued=0 "
                "DefDropped=0\n"
                "[Inject CFR SUMMARY] SourceFps=71.13..79.26 JitterMax=1373us SelMax=67148us EncEmaMax=0.55ms "
                "SustainMin=1818.8fps\n"
            ),
        )
        report = classify_session_triage(inject_pacer)
        assert "ce_capture_pacer_limited" in report["verdicts"]
        assert "ce_visual_timeline_fault" in report["verdicts"]
        assert report["faults"]["visual_timeline"]
        assert report["evidence"]["inject_pacing"]["drop_pace"] == 85
        assert report["evidence"]["inject_pacing"]["publication_fps"] == 240
        assert report["evidence"]["inject_pacing"]["summary_dup_src"] == 48
        assert report["evidence"]["inject_pacing"]["summary_recovery_episodes"] == 1

        inject_recovery_stalled = make_session(
            "inject_recovery_stalled",
            media="[Inject CFR] Recovery still active: duration=5000ms debt=36 start=36 best=35\n",
        )
        report = classify_session_triage(inject_recovery_stalled)
        assert report["evidence"]["visual_fault_counts"]["inject_cfr_recovery_stalled"] == 1
        assert report["faults"]["visual_timeline"]

        inject_planned_source_stall = make_session(
            "inject_planned_source_stall",
            media=(
                "[Inject Perf] Input: 2005 | Queued: 2004 | DropFull: 0 | DropPace: 1 | PubFps: 240 | HostQ: 0 | "
                "EncQ: 1 | Dup: 16 | Late: 0 | Trim: 1 | SelDrop: 1500 | Def: 0 | Encode: 308us | Fence: 2us | "
                "Mux: 0KB | Overload: 0x0\n"
                "[Inject CFR SUMMARY] Live=574 Dup=16 DupPct=2.7% DupReason(src=16 def=0 timer=0 drain=0) "
                "FreshCatchup=0 RepeatCatchup=0 StaleTrim=4 PathMismatch=0/0 DefRequeued=0 DefDropped=0\n"
                "[Inject CFR SUMMARY] SourceFps=24.00..240.83 JitterMax=22684us SelMax=528us EncEmaMax=0.59ms "
                "SustainMin=1701.7fps\n"
            ),
        )
        report = classify_session_triage(inject_planned_source_stall)
        assert "ce_capture_pacer_limited" not in report["verdicts"]
        assert "ce_visual_timeline_fault" not in report["verdicts"]

        inject_playout_churn = make_session(
            "inject_playout_churn",
            media=(
                "[Inject CFR SUMMARY] Live=14239 Dup=882 DupPct=6.1% "
                "DupReason(src=882 def=0 timer=0 drain=0) FreshCatchup=0 RepeatCatchup=0 "
                "StaleTrim=783 Recovery=0/0\n"
                "[Inject CFR SUMMARY] SourceFps=116.20..122.08 JitterMax=8000us SelMax=7000us\n"
            ),
        )
        report = classify_session_triage(inject_playout_churn)
        assert "inject_cfr_playout_churn" in report["verdicts"]
        assert "ce_visual_timeline_fault" in report["verdicts"]

        inject_target_quality = make_session(
            "inject_target_quality",
            media=(
                "[Inject CFR SUMMARY] Live=12000 Dup=0 DupPct=0.0% "
                "DupReason(src=0 def=0 timer=0 drain=0) FreshCatchup=0 RepeatCatchup=0 "
                "StaleTrim=0 Recovery=0/0\n"
                "[Inject CFR SUMMARY] SourceFps=119.80..120.20 JitterMax=300us SelMax=100us\n"
                "[Inject CFR QUALITY SUMMARY] TargetSelect=12000 Superseded=0 TargetHold=0 "
                "HoldWithCandidate=0 BufferCapTrim=0 TargetResidualMax=100us\n"
            ),
        )
        report = classify_session_triage(inject_target_quality)
        assert "inject_cfr_target_policy_hold" not in report["verdicts"]
        assert report["evidence"]["inject_pacing"]["target_select"] == 12000
        assert report["evidence"]["inject_pacing"]["target_residual_max_us"] == 100

        inject_post_hitch_phase_churn = make_session(
            "inject_post_hitch_phase_churn",
            media=(
                "[Inject CFR SUMMARY] Live=13545 Dup=354 DupPct=2.6% "
                "DupReason(src=354 def=0 timer=0 drain=0) FreshCatchup=0 RepeatCatchup=0 "
                "StaleTrim=0 Recovery=0/0\n"
                "[Inject CFR SUMMARY] SourceFps=98.08..122.39 JitterMax=796us SelMax=33907us\n"
                "[Inject CFR QUALITY SUMMARY] TargetSelect=13190 Superseded=255 TargetHold=354 "
                "HoldWithCandidate=267 BufferCapTrim=0 TargetResidualMax=220577us\n"
                "[2026-07-17 16:53:45.000] [INFO] [Inject CFR] Repeat pressure: dup=5 srcLimited=5 "
                "targetSelect=115 targetSuperseded=5 targetHold=5 holdWithCandidate=5 tickEmit=120 "
                "unique=115 sourceFps=119.95 overload=0x0\n"
                "[2026-07-17 16:53:50.000] [INFO] [Inject CFR] Repeat pressure: dup=6 srcLimited=6 "
                "targetSelect=114 targetSuperseded=5 targetHold=6 holdWithCandidate=6 tickEmit=120 "
                "unique=114 sourceFps=120.78 overload=0x0\n"
                "[2026-07-17 16:53:55.000] [INFO] [Inject CFR] Repeat pressure: dup=4 srcLimited=4 "
                "targetSelect=116 targetSuperseded=4 targetHold=4 holdWithCandidate=4 tickEmit=120 "
                "unique=116 sourceFps=121.22 overload=0x0\n"
                "[CFR PHASE LOCK SUMMARY] Backend=inject Enabled=1 Locked=1 Offset=4012us Stable=900 "
                "Unstable=0 Acquire=1 Rephase=1 Release=0 Multiplier=1\n"
            ),
        )
        report = classify_session_triage(inject_post_hitch_phase_churn)
        assert "inject_cfr_target_policy_hold" in report["verdicts"]
        assert report["evidence"]["inject_pacing"]["matched_rate_longest_run"] == 3
        assert report["evidence"]["cfr_phase_lock_summary"][0]["offset_us"] == 4012

        inject_cutscene_phase_retention = make_session(
            "inject_cutscene_phase_retention",
            media=(
                "[Inject CFR SUMMARY] Live=3092 Dup=809 DupPct=26.1% "
                "DupReason(src=783 def=1 timer=25 drain=0) FreshCatchup=0 RepeatCatchup=0 "
                "StaleTrim=0 Recovery=0/0\n"
                "[Inject CFR SUMMARY] SourceFps=114.46..159.77 JitterMax=1203us SelMax=34323us\n"
                "[Inject CFR QUALITY SUMMARY] TargetSelect=2282 Superseded=313 TargetHold=808 "
                "HoldWithCandidate=803 BufferCapTrim=786 TargetResidualMax=54452us "
                "PhaseReservePeak=8 PhaseShiftMax=81001us PreserveFrontTrim=120 "
                "DisplayPathTransitions=2\n"
                "[2026-08-31 07:46:33.000] [INFO] [Inject CFR] Repeat pressure: dup=111 srcLimited=111 "
                "targetSelect=11 targetSuperseded=1 targetHold=111 holdWithCandidate=111 tickEmit=122 "
                "unique=11 sourceFps=121.44 overload=0x0\n"
                "[2026-08-31 07:46:34.000] [INFO] [Inject CFR] Repeat pressure: dup=112 srcLimited=112 "
                "targetSelect=18 targetSuperseded=1 targetHold=112 holdWithCandidate=112 tickEmit=130 "
                "unique=18 sourceFps=121.44 overload=0x0\n"
                "[2026-08-31 07:46:35.000] [INFO] [Inject CFR] Repeat pressure: dup=116 srcLimited=116 "
                "targetSelect=16 targetSuperseded=0 targetHold=116 holdWithCandidate=116 tickEmit=132 "
                "unique=16 sourceFps=120.77 overload=0x0\n"
            ),
        )
        report = classify_session_triage(inject_cutscene_phase_retention)
        assert "inject_cfr_timestamp_retention_fault" in report["verdicts"]
        assert "ce_visual_timeline_fault" in report["verdicts"]
        assert report["evidence"]["inject_pacing"]["phase_shift_max_us"] == 81001
        assert report["evidence"]["inject_pacing"]["display_path_transitions"] == 2

        inject_variable_rate_resampling = make_session(
            "inject_variable_rate_resampling",
            media=(
                "[Inject CFR SUMMARY] Live=12000 Dup=2200 DupPct=18.3% "
                "DupReason(src=2200 def=0 timer=0 drain=0) FreshCatchup=0 RepeatCatchup=0 "
                "StaleTrim=0 Recovery=0/0\n"
                "[Inject CFR SUMMARY] SourceFps=72.00..119.00 JitterMax=5000us SelMax=8000us\n"
                "[Inject CFR QUALITY SUMMARY] TargetSelect=9800 Superseded=1800 TargetHold=2200 "
                "HoldWithCandidate=1800 BufferCapTrim=0 TargetResidualMax=8000us\n"
                "[2026-07-17 16:53:45.000] [INFO] [Inject CFR] Repeat pressure: dup=35 srcLimited=35 "
                "targetSelect=85 targetSuperseded=20 targetHold=35 holdWithCandidate=20 tickEmit=120 "
                "unique=85 sourceFps=85.00 overload=0x0\n"
                "[2026-07-17 16:53:50.000] [INFO] [Inject CFR] Repeat pressure: dup=20 srcLimited=20 "
                "targetSelect=100 targetSuperseded=15 targetHold=20 holdWithCandidate=15 tickEmit=120 "
                "unique=100 sourceFps=100.00 overload=0x0\n"
                "[2026-07-17 16:53:55.000] [INFO] [Inject CFR] Repeat pressure: dup=12 srcLimited=12 "
                "targetSelect=108 targetSuperseded=8 targetHold=12 holdWithCandidate=8 tickEmit=120 "
                "unique=108 sourceFps=108.00 overload=0x0\n"
            ),
        )
        report = classify_session_triage(inject_variable_rate_resampling)
        assert "inject_cfr_target_policy_hold" not in report["verdicts"]
        assert report["evidence"]["inject_pacing"]["matched_rate_pressure_rows"] == 0

        audio_worker_scheduling_stall = make_session(
            "audio_worker_scheduling_stall",
            media="[AudioLoop] Scheduling summary: events=2 maxGap=38124us threshold=25000us\n",
        )
        report = classify_session_triage(audio_worker_scheduling_stall)
        assert report["evidence"]["log_counts"]["audio_worker_scheduling_stall"] == 1
        assert not report["faults"]["audio_timeline"]

        mux_overload = make_session(
            "mux_overload",
            media=(
                "[VideoEncoder] Final metadata durations: target=1000 us video=1000 us "
                "audioMin=1000 us audioMax=1000 us maxDelta=0 us streams(v=1 a=2) "
                "overload(encoder=0 mux=1) backpressure=3 peakMux=20000KB peakPkts=50\n"
            ),
        )
        report = classify_session_triage(mux_overload)
        assert "ce_encoder_or_mux_backpressure" in report["verdicts"]

        writer_timeout = make_session(
            "writer_timeout",
            media=(
                "[VideoEncoder] Stop: WARNING - Writer thread did not finish in 5s (result=258), detaching.\n"
                "[VideoEncoder] Sync Stop: Finalizing file...\n"
            ),
        )
        report = classify_session_triage(writer_timeout)
        assert "ce_encoder_or_mux_backpressure" in report["verdicts"]
        assert report["evidence"]["mux_fault_counts"]["writer_finalize_timeout"] == 1
        assert report["evidence"]["writer_sync_after_timeout"]

        failed_output = make_session(
            "failed_output",
            media="[RECORDING FINALIZATION] status=failed outputSaved=0 finalizationComplete=1\n",
        )
        (failed_output / "recording_legacy_42.manifest").write_text(
            "recording_id=legacy\nmedia_pid=42\nmedia_log=media.log\nstatus=recording_failed\n"
            "output_saved=0\nfinalization_complete=1\n",
            encoding="utf-8",
        )
        report = classify_session_triage(failed_output)
        assert "ce_recording_output_not_saved" in report["verdicts"]
        assert report["faults"]["recording_output_not_saved"]
        assert report["evidence"]["recording_finalization"] == {
            "status": "recording_failed",
            "complete": True,
            "output_saved": False,
            "failed": True,
        }

        canceled_output = make_session(
            "canceled_output",
            media="[RECORDING FINALIZATION] status=canceled outputSaved=0 finalizationComplete=1\n",
        )
        (canceled_output / "recording_legacy_43.manifest").write_text(
            "recording_id=legacy\nmedia_pid=43\nmedia_log=media.log\nstatus=recording_canceled\n"
            "output_saved=0\nfinalization_complete=1\n",
            encoding="utf-8",
        )
        report = classify_session_triage(canceled_output)
        assert "ce_recording_output_not_saved" not in report["verdicts"]
        assert not report["faults"]["recording_output_not_saved"]

        writer_post_mux_hang = make_session(
            "writer_post_mux_hang",
            media=(
                "[VideoEncoder] Stop: ERROR writer_finalize_timeout result=258 phase=post_mux_probe elapsed=30000ms "
                "queueBytes=0 queuePackets=0; async writer retains FFmpeg context, skipping synchronous finalize\n"
                "[VideoEncoder] Final packet timeline: target=1000 us videoEnd=1000 us audioMinEnd=1000 us "
                "audioMaxEnd=1000 us maxPacketDelta=0 us audioPastTarget=0\n"
            ),
        )
        report = classify_session_triage(writer_post_mux_hang)
        assert "post_mux_probe_hang" in report["verdicts"]
        assert "ce_encoder_or_mux_backpressure" in report["verdicts"]
        assert not report["evidence"]["late_writer_finalize_recovered"]
        assert report["faults"]["post_mux_probe_hang"]

        writer_probe_timeout = make_session(
            "writer_probe_timeout",
            media=(
                "[VideoEncoder] mux_closed file='x.mkv' finalDurationUs=1000\n"
                "[VideoEncoder] post_mux_probe_start file='x.mkv' target=1000 timeout=5000ms\n"
                "[VideoEncoder] post_mux_probe_timeout file='x.mkv' elapsed=5000ms; cancelling validation probe\n"
                "[VideoEncoder] post_mux_probe_cancelled file='x.mkv' detached=1\n"
                "[VideoEncoder] Final packet timeline: target=1000 us videoEnd=1000 us audioMinEnd=1000 us "
                "audioMaxEnd=1000 us maxPacketDelta=0 us audioPastTarget=0\n"
            ),
        )
        report = classify_session_triage(writer_probe_timeout)
        assert "post_mux_probe_timeout" in report["verdicts"]
        assert "ce_encoder_or_mux_backpressure" not in report["verdicts"]
        assert report["faults"]["post_mux_probe_timeout"]
