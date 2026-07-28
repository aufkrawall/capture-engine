if False:

        wgc_encoder_judder = make_session(
            "wgc_encoder_judder",
            media=(
                "[Cadence Health] Phase=Live | WgcSelBias=260000us | Shortfall=32/266.7ms "
                "LeadExcess=116.0ms | Oldest=250.0ms BufNow=30 | WgcLiveRebase=1/120/1 | Over=0x1\n"
                "[WGC CFR CADENCE EVENT] mode=encoder_limited shortfall=32/266.7ms phaseErrorAvg=260000us "
                "phaseErrorMax=260000us rebaseWindow=1 encoderDropWindow=1 encoderDropTotal=4 "
                "tooNewRepeat=1 staleDrop=4 freshMiss=0pm bufNow=30 oldest=250.0ms enc=10.0ms "
                "sustain=80.0fps overload=0x1 cause=S0/D0/E1\n"
                "[EncoderThread] WGC CFR slot repeat: buffered frame is too new for scheduled slot "
                "(lead=28000us targetQpc=1000 firstQpc=1280 buffered=30 shortfall=32)\n"
                "[EncoderThread] WGC CFR stale visual debt drop: reason=live-buffer mode=encoder_limited dropped=4 "
                "floorQpc=1200 liveNowQpc=1600 maxDebt=1000us remaining=20 shortfall=32\n"
                "[EncoderThread] WGC CFR live scheduler rebase: mode=encoder_limited skippedTicks=1 excessTicks=2 "
                "requestedTicks=32 shortfallBefore=32 nextQpc=1000 nextAfterQpc=1100 liveNowQpc=1600 "
                "timelineCovered=42\n"
            ),
        )
        report = classify_session_triage(wgc_encoder_judder)
        assert "wgc_encoder_limited_judder" in report["verdicts"]
        assert "wgc_encoder_overload_policy_fault" not in report["verdicts"]
        assert "ce_visual_timeline_fault" in report["verdicts"]
        assert "ce_audio_timeline_fault" not in report["verdicts"]

        wgc_overload_policy_fault = make_session(
            "wgc_overload_policy_fault",
            media=(
                "[Cadence Health] Phase=Live | WgcSelBias=260000us | Shortfall=32/266.7ms "
                "LeadExcess=116.0ms | Oldest=250.0ms BufNow=30 | WgcLiveRebase=1/120/1 | Over=0x1\n"
                "[WGC CFR CADENCE EVENT] mode=normal_pressure shortfall=32/266.7ms phaseErrorAvg=260000us "
                "phaseErrorMax=260000us rebaseWindow=1 encoderDropWindow=0 encoderDropTotal=0 "
                "tooNewRepeat=1 staleDrop=4 freshMiss=0pm bufNow=30 oldest=250.0ms enc=10.0ms "
                "sustain=80.0fps overload=0x1 cause=S0/D0/E1\n"
                "[EncoderThread] WGC CFR slot repeat: buffered frame is too new for scheduled slot "
                "(lead=28000us targetQpc=1000 firstQpc=1280 buffered=30 shortfall=32)\n"
                "[EncoderThread] WGC CFR stale visual debt drop: reason=live-buffer mode=normal dropped=4 "
                "floorQpc=1200 liveNowQpc=1600 maxDebt=1000us remaining=20 shortfall=32\n"
                "[EncoderThread] WGC CFR live scheduler rebase: mode=normal skippedTicks=1 excessTicks=2 "
                "requestedTicks=32 shortfallBefore=32 nextQpc=1000 nextAfterQpc=1100 liveNowQpc=1600 "
                "timelineCovered=42\n"
                "[WGC CFR SMOOTHNESS SUMMARY] encoderLimitedDrops=0 maxDropTicks=0 cadenceEvents=1 "
                "phaseErrorMax=260000us shortfallMax=266.7ms staleDebtDrops=4 liveRebase=1/1 "
                "tooNewRepeats=1\n"
            ),
        )
        report = classify_session_triage(wgc_overload_policy_fault)
        assert "wgc_encoder_overload_policy_fault" in report["verdicts"]
        assert "ce_visual_timeline_fault" in report["verdicts"]
        assert "ce_audio_timeline_fault" not in report["verdicts"]

        wgc_sync_delay_smoothness_summary = make_session(
            "wgc_sync_delay_smoothness_summary",
            media=(
                "[WGC CFR SMOOTHNESS SUMMARY] encoderLimitedDrops=0 maxDropTicks=0 cadenceEvents=1 "
                "phaseErrorMax=9000us shortfallMax=16.7ms staleDebtDrops=0 liveRebase=0/0 "
                "tooNewRepeats=12 syncDelayHolds=10 tooNewLeadMax=42000us avDelay=35.0ms "
                "lowSourceBypass=0 modeMismatch=0 sourceBacktrack=0\n"
            ),
        )
        sync_delay_report = classify_session_triage(wgc_sync_delay_smoothness_summary)
        sync_delay_summary = sync_delay_report["evidence"]["wgc_smoothness_summary"][0]
        assert sync_delay_summary["too_new_repeats"] == 12
        assert sync_delay_summary["sync_delay_holds"] == 10
        assert sync_delay_summary["too_new_lead_max_us"] == 42000
        assert sync_delay_summary["av_delay_ms"] == 35.0
        assert "wgc_av_sync_delay_unrealized" in sync_delay_report["verdicts"]
        assert "ce_visual_timeline_fault" in sync_delay_report["verdicts"]

        wgc_sync_delay_realized = make_session(
            "wgc_sync_delay_realized",
            media=(
                "[WGC CFR SMOOTHNESS SUMMARY] encoderLimitedDrops=0 maxDropTicks=0 cadenceEvents=0 "
                "phaseErrorMax=4000us shortfallMax=0.0ms staleDebtDrops=0 liveRebase=0/0 "
                "tooNewRepeats=0 syncDelayHolds=0 tooNewLeadMax=0us avDelay=35.0ms "
                "startupDelay=35.0ms scheduleOffset=12000us effectiveDelay=35.0ms "
                "lowSourceBypass=0 modeMismatch=0 sourceBacktrack=0\n"
            ),
        )
        realized_report = classify_session_triage(wgc_sync_delay_realized)
        realized_summary = realized_report["evidence"]["wgc_smoothness_summary"][0]
        assert realized_summary["startup_delay_ms"] == 35.0
        assert realized_summary["effective_delay_ms"] == 35.0
        assert realized_summary["schedule_offset_us"] == 12000
        assert "wgc_av_sync_delay_unrealized" not in realized_report["verdicts"]
        assert "ce_visual_timeline_fault" not in realized_report["verdicts"]

        wgc_sync_delay_residual_fault = make_session(
            "wgc_sync_delay_residual_fault",
            media=(
                "[WGC CFR SMOOTHNESS SUMMARY] encoderLimitedDrops=0 maxDropTicks=0 cadenceEvents=18 "
                "phaseErrorMax=12047us shortfallMax=8.3ms staleDebtDrops=2 liveRebase=0/0 "
                "tooNewRepeats=105 syncDelayHolds=105 tooNewLeadMax=35632us avDelay=32.0ms "
                "startupDelay=32.0ms scheduleOffset=313291us effectiveDelay=32.0ms "
                "lowSourceBypass=0 modeMismatch=0 sourceBacktrack=0 "
                "syncDelaySourceLimitedHolds=20 syncDelayPolicyHolds=105 startupReserveFrames=26 "
                "startupReserveSpan=191938us startupDelayTarget=32000us startupReserveSelected=1 "
                "startupReserveReason=selected delayReservoirLowWaterFrames=4 delayReservoirTargetFrames=5 "
                "delayReservoirLowWaterTicks=27 realizedDelayAvg=23000us realizedDelayMin=16000us "
                "realizedDelayMax=32000us delayResidualAvg=9000/9000us delayResidualMax=16000us "
                "delayResidualP95=12000us delayResidualLateMax=16000us delayResidualEarlyMax=1000us\n"
            ),
        )
        residual_fault_report = classify_session_triage(wgc_sync_delay_residual_fault)
        residual_fault_summary = residual_fault_report["evidence"]["wgc_smoothness_summary"][0]
        assert residual_fault_summary["delay_residual_avg_abs_us"] == 9000
        assert residual_fault_summary["delay_residual_max_us"] == 16000
        assert "wgc_av_sync_delay_unrealized" not in residual_fault_report["verdicts"]
        assert "wgc_av_sync_delay_residual" in residual_fault_report["verdicts"]
        assert "wgc_sync_delay_policy_fault" in residual_fault_report["verdicts"]
        assert "ce_visual_timeline_fault" in residual_fault_report["verdicts"]

        wgc_sync_delay_residual_bounded = make_session(
            "wgc_sync_delay_residual_bounded",
            media=(
                "[WGC CFR SMOOTHNESS SUMMARY] encoderLimitedDrops=0 maxDropTicks=0 cadenceEvents=2 "
                "phaseErrorMax=5000us shortfallMax=0.0ms staleDebtDrops=0 liveRebase=0/0 "
                "tooNewRepeats=3 syncDelayHolds=3 tooNewLeadMax=7000us avDelay=32.0ms "
                "startupDelay=32.0ms scheduleOffset=24000us effectiveDelay=32.0ms "
                "lowSourceBypass=0 modeMismatch=0 sourceBacktrack=0 "
                "syncDelaySourceLimitedHolds=3 syncDelayPolicyHolds=0 startupReserveFrames=8 "
                "startupReserveSpan=64000us startupDelayTarget=32000us startupReserveSelected=1 "
                "startupReserveReason=selected delayReservoirLowWaterFrames=4 delayReservoirTargetFrames=5 "
                "delayReservoirLowWaterTicks=0 realizedDelayAvg=30000us realizedDelayMin=26000us "
                "realizedDelayMax=34000us delayResidualAvg=2000/2000us delayResidualMax=6000us "
                "delayResidualP95=4000us delayResidualLateMax=6000us delayResidualEarlyMax=2000us\n"
            ),
        )
        residual_bounded_report = classify_session_triage(wgc_sync_delay_residual_bounded)
        assert "wgc_av_sync_delay_residual" not in residual_bounded_report["verdicts"]
        assert "wgc_av_sync_delay_unrealized" not in residual_bounded_report["verdicts"]
        assert "ce_visual_timeline_fault" not in residual_bounded_report["verdicts"]
        # An 8 ms realized-delay spread is bounded: the instability classifier must stay quiet.
        assert "wgc_active_delay_realized_delay_unstable" not in residual_bounded_report["verdicts"]

        # GPU-bound under-delivery judder signature (Strange Brigade sbwgcjudder shape): residuals
        # and holds look bounded and the runtime reports smoothnessNotMaximal=0, but the realized
        # content delay rubber-bands 22.9..44.4 ms. The legacy analyzer trusted the runtime verdict
        # and called this "fine"; the instability classifier must now flag it distinctly.
        wgc_realized_delay_unstable = make_session(
            "wgc_realized_delay_unstable",
            media=(
                "[WGC CFR SMOOTHNESS SUMMARY] encoderLimitedDrops=0 maxDropTicks=0 cadenceEvents=101 "
                "phaseErrorMax=3260us shortfallMax=58.3ms staleDebtDrops=13 liveRebase=0/0 "
                "tooNewRepeats=544 syncDelayHolds=544 tooNewLeadMax=40208us avDelay=31.1ms "
                "startupDelay=31.1ms scheduleOffset=99714us effectiveDelay=31.1ms "
                "lowSourceBypass=0 modeMismatch=0 sourceBacktrack=0 "
                "syncDelaySourceLimitedHolds=544 syncDelayPolicyHolds=0 startupReserveFrames=34 "
                "startupReserveSpan=229296us startupDelayTarget=31111us startupReserveSelected=1 "
                "startupReserveReason=selected delayReservoirLowWaterFrames=4 delayReservoirTargetFrames=5 "
                "delayReservoirLowWaterTicks=1279 realizedDelayAvg=30756us realizedDelayMin=22875us "
                "realizedDelayMax=44419us delayResidualAvg=354/2199us delayResidualMax=13308us "
                "delayResidualP95=5000us delayResidualLateMax=8236us delayResidualEarlyMax=13308us\n"
                "[WGC CFR SMOOTHNESS VERDICT] delayPostSelectionRejectedSync=0 "
                "delayPostSelectionRescuedSync=0 sourceRepeatLowerBound=544 excessRepeats=0 "
                "policyAddedRepeats=0 excessRepeatClusters=0 excessRepeatClusterMax=0 "
                "smoothnessNotMaximal=0 mixedPolicyFault=0\n"
            ),
        )
        realized_unstable_report = classify_session_triage(wgc_realized_delay_unstable)
        realized_unstable_summary = realized_unstable_report["evidence"]["wgc_smoothness_summary"][0]
        assert realized_unstable_summary["realized_delay_min_us"] == 22875
        assert realized_unstable_summary["realized_delay_max_us"] == 44419
        # The runtime self-classified the run as fine; the spread must still be flagged.
        assert "wgc_cfr_smoothness_not_maximal" not in realized_unstable_report["verdicts"]
        assert "wgc_active_delay_realized_delay_unstable" in realized_unstable_report["verdicts"]

        # Full-collapse shape (real 20260625_135221 session): the realized content delay both inflated
        # (max 248 ms vs 31.6 ms target) AND fully collapsed to 0 (live-recovery latched and disabled
        # the delay for tens of seconds). A realizedDelayMin of 0 is the WORST case, but the legacy
        # spread guard (`delay_min <= 0`) treated it as "no data" and returned 0, silently hiding the
        # most severe instability. The classifier must now fire on min=0 with a valid large max.
        wgc_realized_delay_collapse = make_session(
            "wgc_realized_delay_collapse",
            media=(
                "[WGC CFR SMOOTHNESS SUMMARY] encoderLimitedDrops=0 maxDropTicks=0 cadenceEvents=10 "
                "phaseErrorMax=159206us shortfallMax=66.7ms staleDebtDrops=58 liveRebase=0/0 "
                "tooNewRepeats=13 syncDelayHolds=0 tooNewLeadMax=71540us avDelay=31.6ms "
                "startupDelay=31.6ms scheduleOffset=347201us effectiveDelay=31.6ms "
                "lowSourceBypass=1 modeMismatch=0 sourceBacktrack=0 "
                "syncDelaySourceLimitedHolds=0 syncDelayPolicyHolds=0 startupReserveFrames=32 "
                "startupReserveSpan=219477us startupDelayTarget=31558us startupReserveSelected=1 "
                "startupReserveReason=selected delayReservoirLowWaterFrames=4 delayReservoirTargetFrames=5 "
                "delayReservoirLowWaterTicks=735 realizedDelayAvg=64372us realizedDelayMin=0us "
                "realizedDelayMax=248196us delayResidualAvg=-32813/35303us delayResidualMax=216638us "
                "delayResidualP95=141000us delayResidualLateMax=37181us delayResidualEarlyMax=216638us\n"
            ),
        )
        realized_collapse_report = classify_session_triage(wgc_realized_delay_collapse)
        realized_collapse_summary = realized_collapse_report["evidence"]["wgc_smoothness_summary"][0]
        assert realized_collapse_summary["realized_delay_min_us"] == 0
        assert realized_collapse_summary["realized_delay_max_us"] == 248196
        assert wgc_realized_delay_spread_us(realized_collapse_summary) == 248196
        assert "wgc_active_delay_realized_delay_unstable" in realized_collapse_report["verdicts"]

        wgc_sync_delay_realized_source_pressure = make_session(
            "wgc_sync_delay_realized_source_pressure",
            media=(
                "[WGC CFR SMOOTHNESS SUMMARY] encoderLimitedDrops=0 maxDropTicks=0 cadenceEvents=12 "
                "phaseErrorMax=12047us shortfallMax=8.3ms staleDebtDrops=10 liveRebase=0/0 "
                "tooNewRepeats=48 syncDelayHolds=48 tooNewLeadMax=36084us avDelay=34.8ms "
                "startupDelay=34.8ms scheduleOffset=54224us effectiveDelay=34.8ms "
                "lowSourceBypass=0 modeMismatch=0 sourceBacktrack=0 "
                "syncDelaySourceLimitedHolds=48 syncDelayPolicyHolds=0 startupReserveFrames=6 "
                "startupReserveSpan=42000us startupDelayTarget=34800us startupReserveSelected=1 "
                "startupReserveReason=selected\n"
            ),
        )
        source_pressure_report = classify_session_triage(wgc_sync_delay_realized_source_pressure)
        source_pressure_summary = source_pressure_report["evidence"]["wgc_smoothness_summary"][0]
        assert source_pressure_summary["sync_delay_source_limited_holds"] == 48
        assert source_pressure_summary["sync_delay_policy_holds"] == 0
        assert source_pressure_summary["startup_reserve_selected"] == 1
        assert "wgc_av_sync_delay_unrealized" not in source_pressure_report["verdicts"]
        assert "wgc_sync_delay_reserve_pressure" in source_pressure_report["verdicts"]
        assert "ce_visual_timeline_fault" not in source_pressure_report["verdicts"]

        wgc_sync_delay_realized_legacy_pressure = make_session(
            "wgc_sync_delay_realized_legacy_pressure",
            media=(
                "[WGC CFR SMOOTHNESS SUMMARY] encoderLimitedDrops=0 maxDropTicks=0 cadenceEvents=12 "
                "phaseErrorMax=12047us shortfallMax=8.3ms staleDebtDrops=10 liveRebase=0/0 "
                "tooNewRepeats=48 syncDelayHolds=48 tooNewLeadMax=36084us avDelay=34.8ms "
                "startupDelay=34.8ms scheduleOffset=54224us effectiveDelay=34.8ms "
                "lowSourceBypass=0 modeMismatch=0 sourceBacktrack=0\n"
            ),
        )
        legacy_pressure_report = classify_session_triage(wgc_sync_delay_realized_legacy_pressure)
        assert "wgc_av_sync_delay_unrealized" not in legacy_pressure_report["verdicts"]
        assert "wgc_sync_delay_reserve_pressure" in legacy_pressure_report["verdicts"]
        assert "ce_visual_timeline_fault" not in legacy_pressure_report["verdicts"]

        wgc_sync_delay_goodish_source_limited = make_session(
            "wgc_sync_delay_goodish_source_limited",
            media=(
                "[WGC CFR SUMMARY] Live=13965 Dup=3006 DupPct=21.5% NoFresh=261pm NoReserve=296pm "
                "DupReason(src=3006 def=0 timer=0 drain=0) SourceLimitedRepeats=3006 StarvedEpisodes=281 "
                "longest=6469ms longestDup=401 worstIn=4 worstDel=4\n"
                "[WGC CFR SMOOTHNESS SUMMARY] encoderLimitedDrops=0 maxDropTicks=0 cadenceEvents=114 "
                "phaseErrorMax=103462us shortfallMax=0.0ms staleDebtDrops=1 liveRebase=0/0 "
                "tooNewRepeats=1676 syncDelayHolds=1676 tooNewLeadMax=160855us avDelay=32.0ms "
                "startupDelay=32.0ms scheduleOffset=102131us effectiveDelay=32.0ms "
                "lowSourceBypass=0 modeMismatch=0 sourceBacktrack=0 "
                "syncDelaySourceLimitedHolds=1291 syncDelayPolicyHolds=385 startupReserveFrames=2 "
                "startupReserveSpan=7961us startupDelayTarget=31995us startupReserveSelected=0 "
                "startupReserveReason=reserve_timeout delayReservoirLowWaterFrames=4 "
                "delayReservoirTargetFrames=5 delayReservoirLowWaterTicks=4140 realizedDelayAvg=31500us "
                "realizedDelayMin=22009us realizedDelayMax=237273us delayResidualAvg=494/2444us "
                "delayResidualMax=205278us delayResidualP95=5000us delayResidualLateMax=9986us "
                "delayResidualEarlyMax=205278us\n"
            ),
        )
        goodish_report = classify_session_triage(wgc_sync_delay_goodish_source_limited)
        assert "wgc_source_starvation" in goodish_report["verdicts"]
        assert "wgc_sync_delay_reserve_pressure" in goodish_report["verdicts"]
        assert "wgc_av_sync_delay_residual" not in goodish_report["verdicts"]
        assert "wgc_sync_delay_policy_fault" not in goodish_report["verdicts"]
        assert "ce_visual_timeline_fault" not in goodish_report["verdicts"]
        assert "ce_audio_timeline_fault" not in goodish_report["verdicts"]

        wgc_sync_delay_mixed_policy_pressure = make_session(
            "wgc_sync_delay_mixed_policy_pressure",
            media=(
                "[WGC CFR SUMMARY] Live=10869 Dup=2072 DupPct=19.0% NoFresh=230pm NoReserve=256pm "
                "DupReason(src=2072 def=0 timer=0 drain=0) SourceLimitedRepeats=2072 StarvedEpisodes=235 "
                "longest=4359ms longestDup=214 worstIn=4 worstDel=4\n"
                "[WGC CFR CADENCE EVENT] mode=normal_pressure shortfall=0/0.0ms phaseErrorAvg=427us "
                "phaseErrorMax=6782us rebaseWindow=0 encoderDropWindow=0 encoderDropTotal=0 "
                "tooNewRepeat=11 syncDelayHold=11 syncDelaySourceHold=1 syncDelayPolicyHold=10 "
                "tooNewLeadMax=12459us staleDrop=0 freshMiss=117pm bufNow=5 oldest=24.9ms enc=0.41ms "
                "sustain=2428.8fps overload=0x0 lowSourceBypass=0 modeMismatch=0 sourceBacktrack=0 "
                "avDelay=31.0ms delayResidualAvg=382/2320us delayResidualMax=76400us "
                "delayResidualP95=5000us delayResidualLateMax=9990us reservoir=4/5 lowTicks=14 cause=S0/D0/E0\n"
                "[WGC CFR SMOOTHNESS SUMMARY] encoderLimitedDrops=0 maxDropTicks=0 cadenceEvents=88 "
                "phaseErrorMax=34457us shortfallMax=0.0ms staleDebtDrops=42 liveRebase=0/0 "
                "tooNewRepeats=1104 syncDelayHolds=1104 tooNewLeadMax=165483us avDelay=31.0ms "
                "startupDelay=31.0ms scheduleOffset=300848us effectiveDelay=31.0ms "
                "lowSourceBypass=0 modeMismatch=0 sourceBacktrack=0 "
                "syncDelaySourceLimitedHolds=782 syncDelayPolicyHolds=322 startupReserveFrames=18 "
                "startupReserveSpan=243048us startupDelayTarget=31003us startupReserveSelected=1 "
                "startupReserveReason=selected delayReservoirLowWaterFrames=4 "
                "delayReservoirTargetFrames=5 delayReservoirLowWaterTicks=2783 realizedDelayAvg=30618us "
                "realizedDelayMin=21013us realizedDelayMax=107403us delayResidualAvg=384/2319us "
                "delayResidualMax=76400us delayResidualP95=5000us delayResidualLateMax=9990us "
                "delayResidualEarlyMax=76400us\n"
            ),
        )
        mixed_policy_report = classify_session_triage(wgc_sync_delay_mixed_policy_pressure)
        assert "wgc_source_starvation" in mixed_policy_report["verdicts"]
        assert "wgc_av_sync_delay_residual" in mixed_policy_report["verdicts"]
        assert "wgc_sync_delay_policy_fault" in mixed_policy_report["verdicts"]
        assert "ce_visual_timeline_fault" in mixed_policy_report["verdicts"]
        assert "ce_audio_timeline_fault" not in mixed_policy_report["verdicts"]

        wgc_sync_delay_fortidelay2_mixed_pressure = make_session(
            "wgc_sync_delay_fortidelay2_mixed_pressure",
            media=(
                "[WGC CFR SUMMARY] Live=8709 Dup=1545 DupPct=17.7% NoFresh=227pm NoReserve=258pm "
                "DupReason(src=1545 def=0 timer=0 drain=0) SourceLimitedRepeats=1545 StarvedEpisodes=184 "
                "longest=4250ms longestDup=200 worstIn=4 worstDel=4\n"
                "[WGC CFR SMOOTHNESS SUMMARY] encoderLimitedDrops=0 maxDropTicks=0 cadenceEvents=72 "
                "phaseErrorMax=5382us shortfallMax=0.0ms staleDebtDrops=32 liveRebase=0/0 "
                "tooNewRepeats=893 syncDelayHolds=893 tooNewLeadMax=163581us avDelay=27.5ms "
                "startupDelay=27.5ms scheduleOffset=308784us effectiveDelay=27.5ms "
                "lowSourceBypass=0 modeMismatch=0 sourceBacktrack=0 "
                "syncDelaySourceLimitedHolds=603 syncDelayPolicyHolds=290 startupReserveFrames=14 "
                "startupReserveSpan=152803us startupDelayTarget=27474us startupReserveSelected=1 "
                "startupReserveReason=selected delayReservoirLowWaterFrames=4 "
                "delayReservoirTargetFrames=5 delayReservoirLowWaterTicks=2254 realizedDelayAvg=26815us "
                "realizedDelayMin=17664us realizedDelayMax=220633us delayResidualAvg=658/2397us "
                "delayResidualMax=193159us delayResidualP95=5000us delayResidualLateMax=9810us "
                "delayResidualEarlyMax=193159us delayResidualRelaxedSelections=462 "
                "delayResidualRelaxedMax=9810us delayResidualRelaxedRejectedSync=41577 "
                "delayRepeatClusterPressure=175 delayRepeatClusterMax=92 mixedPolicyFault=1\n"
                "[STOP AUDIO TRACK] Track 1: encoded=3484000 expected=3484000 diff=+0 (+0.000 ms) "
                "sources=[1,3,5,7]\n"
                "[STOP AUDIO TRACK] Track 2: encoded=3484000 expected=3484000 diff=+0 (+0.000 ms) "
                "sources=[4,6,8]\n"
                "[STOP AUDIO TRACK] Track 3: encoded=3484000 expected=3484000 diff=+0 (+0.000 ms) "
                "sources=[0,2]\n"
                "[VideoEncoder] Final packet timeline: target=72583333 us videoEnd=72583333 us "
                "audioMinEnd=72583333 us audioMaxEnd=72583333 us maxPacketDelta=0 us streams(v=1 a=3) "
                "audioPastTarget=0\n"
                "[VideoEncoder] Final metadata durations: target=72583333 us video=72583333 us "
                "audioMin=72583333 us audioMax=72583333 us maxDelta=0 us streams(v=1 a=3) "
                "overload(encoder=0 mux=0) backpressure=0\n"
            ),
        )
        fortidelay2_report = classify_session_triage(wgc_sync_delay_fortidelay2_mixed_pressure)
        fortidelay2_summary = fortidelay2_report["evidence"]["wgc_smoothness_summary"][0]
        assert fortidelay2_summary["delay_relaxed_rejected_sync"] == 41577
        assert fortidelay2_summary["delay_repeat_cluster_pressure"] == 175
        assert fortidelay2_summary["delay_repeat_cluster_max_ticks"] == 92
        assert "wgc_source_starvation" in fortidelay2_report["verdicts"]
        assert "wgc_av_sync_delay_residual" in fortidelay2_report["verdicts"]
        assert "wgc_sync_delay_policy_fault" in fortidelay2_report["verdicts"]
        assert "ce_visual_timeline_fault" in fortidelay2_report["verdicts"]
        assert "ce_audio_timeline_fault" not in fortidelay2_report["verdicts"]

        wgc_sync_delay_20260620_044434_unstable_fps = make_session(
            "wgc_sync_delay_20260620_044434_unstable_fps",
            media=(
                "[WGC CFR SUMMARY] Live=29098 Dup=3369 DupPct=11.5% NoFresh=161pm NoReserve=183pm "
                "DupReason(src=3369 def=0 timer=0 drain=0) SourceLimitedRepeats=3369 StarvedEpisodes=757 "
                "longest=9313ms longestDup=573 worstIn=4 worstDel=4\n"
                "[WGC CFR SMOOTHNESS SUMMARY] encoderLimitedDrops=0 maxDropTicks=0 cadenceEvents=242 "
                "phaseErrorMax=110628us shortfallMax=0.0ms staleDebtDrops=1 liveRebase=0/0 "
                "tooNewRepeats=2927 syncDelayHolds=2927 tooNewLeadMax=95075us avDelay=28.3ms "
                "startupDelay=28.3ms scheduleOffset=404156us effectiveDelay=28.3ms "
                "lowSourceBypass=0 modeMismatch=0 sourceBacktrack=0 "
                "syncDelaySourceLimitedHolds=2159 syncDelayPolicyHolds=768 startupReserveFrames=1 "
                "startupReserveSpan=0us startupDelayTarget=28334us startupReserveSelected=0 "
                "startupReserveReason=source_startup_underfeed delayReservoirLowWaterFrames=4 "
                "delayReservoirTargetFrames=5 delayReservoirLowWaterTicks=5346 realizedDelayAvg=27764us "
                "realizedDelayMin=18412us realizedDelayMax=78381us delayResidualAvg=569/2322us "
                "delayResidualMax=50047us delayResidualP95=5000us delayResidualLateMax=9922us "
                "delayResidualEarlyMax=50047us rawResidualAvg=676/2502us rawResidualMax=63403us "
                "rawResidualP95=6000us rawResidualLateMax=9996us rawResidualEarlyMax=63403us "
                "predictedResidualAvg=569/2322us predictedResidualP95=5000us "
                "predictedResidualLateMax=9922us rawMinusPredictedAvg=107/107us "
                "rawMinusPredictedMax=66811us delayResidualRelaxedSelections=1447 "
                "delayResidualRelaxedMax=9922us delayResidualRelaxedRejectedSync=335410 "
                "delayRepeatClusterPressure=429 delayRepeatClusterMax=34 "
                "delayResidualRelaxedBetter=1445 delayResidualRelaxedCluster=1 "
                "delayResidualRelaxedRejectedHeadroom=103 delayResidualRelaxedRejectedCost=11644 "
                "delaySourceRecoveryHolds=841 delaySourceRecoveryTicks=14564 "
                "delayPostSelectionRejectedSync=0 delayPostSelectionRescuedSync=0 "
                "sourceRepeatLowerBound=2426 excessRepeats=508 policyAddedRepeats=508 "
                "excessRepeatClusters=0 excessRepeatClusterMax=0 smoothnessNotMaximal=1 "
                "mixedPolicyFault=1\n"
                "[STOP AUDIO TRACK] Track 1: encoded=11640400 expected=11640400 diff=+0 (+0.000 ms) "
                "sources=[1,3,5,7]\n"
                "[STOP AUDIO TRACK] Track 2: encoded=11640400 expected=11640400 diff=+0 (+0.000 ms) "
                "sources=[4,6,8]\n"
                "[STOP AUDIO TRACK] Track 3: encoded=11640400 expected=11640400 diff=+0 (+0.000 ms) "
                "sources=[0,2]\n"
                "[VideoEncoder] Final packet timeline: target=242508333 us videoEnd=242508333 us "
                "audioMinEnd=242508333 us audioMaxEnd=242508333 us maxPacketDelta=0 us streams(v=1 a=3) "
                "audioPastTarget=0\n"
                "[VideoEncoder] Final metadata durations: target=242508333 us video=242508333 us "
                "audioMin=242508333 us audioMax=242508333 us maxDelta=0 us streams(v=1 a=3) "
                "overload(encoder=0 mux=0) backpressure=0\n"
            ),
        )
        delay_20260620_report = classify_session_triage(wgc_sync_delay_20260620_044434_unstable_fps)
        delay_20260620_summary = delay_20260620_report["evidence"]["wgc_smoothness_summary"][0]
        assert delay_20260620_summary["source_repeat_lower_bound"] == 2426
        assert delay_20260620_summary["policy_added_repeats"] == 508
        assert delay_20260620_summary["delay_repeat_cluster_pressure"] == 429
        assert "wgc_source_starvation" in delay_20260620_report["verdicts"]
        assert "wgc_av_sync_delay_residual" in delay_20260620_report["verdicts"]
        assert "wgc_sync_delay_policy_fault" in delay_20260620_report["verdicts"]
        assert "wgc_cfr_smoothness_not_maximal" in delay_20260620_report["verdicts"]
        assert "ce_visual_timeline_fault" in delay_20260620_report["verdicts"]
        assert "ce_audio_timeline_fault" not in delay_20260620_report["verdicts"]

        wgc_sync_delay_fortineu_safe_candidate_pressure = make_session(
            "wgc_sync_delay_fortineu_safe_candidate_pressure",
            media=(
                "[WGC CFR SUMMARY] Live=31799 Dup=3055 DupPct=9.6% NoFresh=130pm NoReserve=158pm "
                "DupReason(src=3055 def=0 timer=0 drain=0) SourceLimitedRepeats=3055 StarvedEpisodes=663 "
                "longest=2703ms longestDup=172 worstIn=4 worstDel=4\n"
                "[WGC CFR SMOOTHNESS SUMMARY] encoderLimitedDrops=0 maxDropTicks=0 cadenceEvents=210 "
                "phaseErrorMax=214038us shortfallMax=8.3ms staleDebtDrops=0 liveRebase=0/0 "
                "tooNewRepeats=2644 syncDelayHolds=2644 tooNewLeadMax=110473us avDelay=33.6ms "
                "startupDelay=33.6ms scheduleOffset=-169584us effectiveDelay=33.6ms "
                "lowSourceBypass=0 modeMismatch=0 sourceBacktrack=0 "
                "syncDelaySourceLimitedHolds=1709 syncDelayPolicyHolds=935 startupReserveFrames=1 "
                "startupReserveSpan=0us startupDelayTarget=33600us startupReserveSelected=0 "
                "startupReserveReason=source_startup_underfeed delayReservoirLowWaterFrames=5 "
                "delayReservoirTargetFrames=6 delayReservoirLowWaterTicks=5043 realizedDelayAvg=33175us "
                "realizedDelayMin=22000us realizedDelayMax=79000us delayResidualAvg=405/2210us "
                "delayResidualMax=45594us delayResidualP95=5000us delayResidualLateMax=9997us "
                "delayResidualEarlyMax=45594us rawResidualAvg=504/2407us rawResidualMax=50000us "
                "rawResidualP95=5000us rawResidualLateMax=9958us rawResidualEarlyMax=50000us "
                "predictedResidualAvg=405/2210us predictedResidualP95=5000us "
                "predictedResidualLateMax=9997us rawMinusPredictedAvg=98/100us "
                "rawMinusPredictedMax=1000us delayResidualRelaxedSelections=1218 "
                "delayResidualRelaxedMax=9958us delayResidualRelaxedRejectedSync=479466 "
                "delayRepeatClusterPressure=295 delayRepeatClusterMax=56 "
                "delayResidualRelaxedBetter=1217 delayResidualRelaxedCluster=0 "
                "delayResidualRelaxedRejectedHeadroom=7584 delayResidualRelaxedRejectedCost=9124 "
                "delaySoftLateRejected=7496 delaySoftLateAccepted=1978 "
                "delayOlderFrameAvoidedRepeat=27484 delaySourceLimitedRepeats=1945 "
                "delayRepeatRescue=0/935 delayRepeatRescueRejected=0/0/935 "
                "delayRepeatSafeCandidate=699 delayRepeatNoSafeCandidate=236 "
                "delayRepeatWindowClass=0/699/236 delayRepeatReserveMax=6/50000us "
                "delaySourceRecoveryHolds=675 delaySourceRecoveryTicks=12971 "
                "delayPostSelectionRejectedSync=0 delayPostSelectionRescuedSync=0 "
                "sourceRepeatLowerBound=1945 excessRepeats=699 policyAddedRepeats=699 "
                "excessRepeatClusters=12 excessRepeatClusterMax=1 smoothnessNotMaximal=1 "
                "mixedPolicyFault=1\n"
                "[STOP AUDIO TRACK] Track 1: encoded=12720000 expected=12720000 diff=+0 (+0.000 ms) "
                "sources=[1,3,5,7]\n"
                "[VideoEncoder] Final packet timeline: target=265000000 us videoEnd=265000000 us "
                "audioMinEnd=265000000 us audioMaxEnd=265000000 us maxPacketDelta=0 us streams(v=1 a=3) "
                "audioPastTarget=0\n"
                "[VideoEncoder] Final metadata durations: target=265000000 us video=265000000 us "
                "audioMin=265000000 us audioMax=265000000 us maxDelta=0 us streams(v=1 a=3) "
                "overload(encoder=0 mux=0) backpressure=0\n"
            ),
        )
        fortineu_report = classify_session_triage(wgc_sync_delay_fortineu_safe_candidate_pressure)
        fortineu_summary = fortineu_report["evidence"]["wgc_smoothness_summary"][0]
        assert fortineu_summary["delay_repeat_safe_candidate"] == 699
        assert fortineu_summary["delay_repeat_rescue_attempts"] == 935
        assert "wgc_repeat_despite_safe_candidate" in fortineu_report["verdicts"]
        assert "wgc_cfr_smoothness_not_maximal" in fortineu_report["verdicts"]
        assert "ce_visual_timeline_fault" in fortineu_report["verdicts"]
        assert "ce_audio_timeline_fault" not in fortineu_report["verdicts"]

        wgc_sync_delay_20260620_171522_safe_candidate_pressure = make_session(
            "wgc_sync_delay_20260620_171522_safe_candidate_pressure",
            media=(
                "[WGC CFR SUMMARY] Live=12599 Dup=2501 DupPct=19.8% NoFresh=224pm NoReserve=269pm "
                "DupReason(src=2501 def=0 timer=0 drain=0) SourceLimitedRepeats=2501 StarvedEpisodes=162 "
                "longest=6703ms longestDup=404 worstIn=4 worstDel=4\n"
                "[WGC CFR SMOOTHNESS SUMMARY] encoderLimitedDrops=0 maxDropTicks=0 cadenceEvents=102 "
                "phaseErrorMax=249880us shortfallMax=0.0ms staleDebtDrops=25 liveRebase=0/0 "
                "tooNewRepeats=1617 syncDelayHolds=1617 tooNewLeadMax=229934us avDelay=32.4ms "
                "startupDelay=32.4ms scheduleOffset=519552us effectiveDelay=32.4ms "
                "lowSourceBypass=0 modeMismatch=0 sourceBacktrack=0 "
                "syncDelaySourceLimitedHolds=1363 syncDelayPolicyHolds=254 startupReserveFrames=23 "
                "startupReserveSpan=205902us startupDelayTarget=32399us startupReserveSelected=1 "
                "startupReserveReason=partial_span_timeout delayReservoirLowWaterFrames=4 "
                "delayReservoirTargetFrames=5 delayReservoirLowWaterTicks=3399 realizedDelayAvg=32057us "
                "realizedDelayMin=22425us realizedDelayMax=219012us delayResidualAvg=341/2366us "
                "delayResidualMax=186613us delayResidualP95=5000us delayResidualLateMax=9974us "
                "delayResidualEarlyMax=186613us rawResidualAvg=449/2423us rawResidualMax=32695us "
                "rawResidualP95=5000us rawResidualLateMax=9993us rawResidualEarlyMax=32695us "
                "predictedResidualAvg=341/2366us predictedResidualP95=5000us "
                "predictedResidualLateMax=9974us rawMinusPredictedAvg=108/108us "
                "rawMinusPredictedMax=193046us delayResidualRelaxedSelections=504 "
                "delayResidualRelaxedMax=9974us delayResidualRelaxedRejectedSync=164724 "
                "delayRepeatClusterPressure=562 delayRepeatClusterMax=167 "
                "delayResidualRelaxedBetter=504 delayResidualRelaxedCluster=0 "
                "delayResidualRelaxedRejectedHeadroom=3119 delayResidualRelaxedRejectedCost=3210 "
                "delaySoftLateRejected=2976 delaySoftLateAccepted=646 "
                "delayOlderFrameAvoidedRepeat=9548 delaySourceLimitedRepeats=1438 "
                "delayRepeatRescue=29/1646 delayRepeatRescueRejected=265/4498/357 "
                "delayRepeatPromoted=29/1646 delayRepeatPromoteRejectedSoft=2976 "
                "delayRepeatSafeAfterPromote=569 delayRepeatSafeCandidate=569 "
                "delayRepeatNoSafeCandidate=1048 delayRepeatWindowClass=65/246/1306 "
                "delayRepeatReserveMax=8/87055us delaySourceRecoveryHolds=131 "
                "delaySourceRecoveryTicks=5995 delayPostSelectionRejectedSync=0 "
                "delayPostSelectionRescuedSync=0 sourceRepeatLowerBound=1438 excessRepeats=254 "
                "policyAddedRepeats=254 excessRepeatClusters=5 excessRepeatClusterMax=1 "
                "smoothnessNotMaximal=1 mixedPolicyFault=0\n"
                "[STOP AUDIO TRACK] Track 1: encoded=5040000 expected=5040000 diff=+0 (+0.000 ms) "
                "sources=[1,3,5,7]\n"
                "[STOP AUDIO TRACK] Track 2: encoded=5040000 expected=5040000 diff=+0 (+0.000 ms) "
                "sources=[4,6,8]\n"
                "[STOP AUDIO TRACK] Track 3: encoded=5040000 expected=5040000 diff=+0 (+0.000 ms) "
                "sources=[0,2]\n"
                "[VideoEncoder] Final packet timeline: target=105000000 us videoEnd=105000000 us "
                "audioMinEnd=105000000 us audioMaxEnd=105000000 us maxPacketDelta=0 us streams(v=1 a=3) "
                "audioPastTarget=0\n"
                "[VideoEncoder] Final metadata durations: target=105000000 us video=105000000 us "
                "audioMin=105000000 us audioMax=105000000 us maxDelta=0 us streams(v=1 a=3) "
                "overload(encoder=0 mux=0) backpressure=0\n"
            ),
        )
        latest_safe_candidate_report = classify_session_triage(
            wgc_sync_delay_20260620_171522_safe_candidate_pressure
        )
        latest_safe_candidate_summary = latest_safe_candidate_report["evidence"]["wgc_smoothness_summary"][0]
        assert latest_safe_candidate_summary["policy_added_repeats"] == 254
        assert latest_safe_candidate_summary["delay_repeat_safe_after_promotion"] == 569
        assert latest_safe_candidate_summary["delay_repeat_promoted_before_repeat"] == 29
        assert "wgc_repeat_despite_safe_candidate" in latest_safe_candidate_report["verdicts"]
        assert "wgc_cfr_smoothness_not_maximal" in latest_safe_candidate_report["verdicts"]
        assert "ce_visual_timeline_fault" in latest_safe_candidate_report["verdicts"]
        assert "ce_audio_timeline_fault" not in latest_safe_candidate_report["verdicts"]

        wgc_sync_delay_20260620_173427_truncated_smoothness = make_session(
            "wgc_sync_delay_20260620_173427_truncated_smoothness",
            media=(
                "[WGC CFR SUMMARY] Live=12371 Dup=2251 DupPct=18.1% NoFresh=214pm NoReserve=246pm "
                "DupReason(src=2251 def=0 timer=0 drain=0) SourceLimitedRepeats=2251 StarvedEpisodes=229 "
                "longest=4672ms longestDup=194 worstIn=4 worstDel=4\n"
                "[WGC CFR SMOOTHNESS SUMMARY] encoderLimitedDrops=0 maxDropTicks=0 cadenceEvents=101 "
                "phaseErrorMax=245788us shortfallMax=0.0ms staleDebtDrops=2 liveRebase=0/0 "
                "tooNewRepeats=1314 syncDelayHolds=1314 tooNewLeadMax=165307us avDelay=32.3ms "
                "startupDelay=32.3ms scheduleOffset=292441us effectiveDelay=32.3ms "
                "lowSourceBypass=0 modeMismatch=0 sourceBacktrack=0 "
                "syncDelaySourceLimitedHolds=1093 syncDelayPolicyHolds=221 startupReserveFrames=15 "
                "startupReserveSpan=107502us startupDelayTarget=32281us startupReserveSelected=1 "
                "startupReserveReason=partial_span_timeout delayReservoirLowWaterFrames=4 "
                "delayReservoirTargetFrames=5 delayReservoirLowWaterTicks=3049 realizedDelayAvg=31990us "
                "realizedDelayMin=22427us realizedDelayMax=193734us delayResidualAvg=290/2377us "
                "delayResidualMax=161453us delayResidualP95=5000us delayResidualLateMax=9854us "
                "delayResidualEarlyMax=161453us rawResidualAvg=370/2403us rawResidualMax=79471us "
                "rawResidualP95=5000us rawResidualLateMax=9958us rawResidualEarlyMax=79471us "
                "predictedResidualAvg=290/2377us predictedResidualP95=5000us "
                "predictedResidualLateMax=9854us rawMinusPredictedAvg=80/80us "
                "rawMinusPredictedMax=163331us delayResidualRelaxedSelections=515 "
                "delayResidualRelaxedMax=9854us delayResidualRelaxedRejectedSync=156642 "
                "delayRepeatClusterPressure=375 delayRepeatClusterMax=110 "
                "delayResidualRelaxedBetter=465 delayResidualRelaxedCluster=1 "
                "delayResidualRelaxedRejectedHeadroom=3032 delayResidualRelaxedRejectedCost=3006 "
                "delaySoftLateRejected=2804 delaySoftLateAccepted=496 "
                "delayOlderFrameAvoidedRepeat=9545 delaySourceLimitedRepeats=1097 "
                "delayRepeatRescue=0/1314 delayRepeatRescueRejected=3893/325/335 "
                "delayRepeatPromoted=0/1314 delayRepeatPromoteRejectedSoft=273 "
                "delayRepeatSafeAfterPromote=593 delayRepeatSafeCandidate=593 "
                "delayRepeatNoSafeCandidate=721 delayRepeatWindowClass=65/204/1045 "
                "delayRepeatReserveMax=8/107104us delaySourceRecoveryHolds=251 "
                "delaySourceRecoveryTicks=6845 delayPostSelectionRejectedSync=0 "
                "delayPostSelectionRescuedSync=0 sourceRepeatLowerBound=1097 excessRepeats=221 "
                "policyAddedRepeats=221 excessRe\n"
                "[STOP AUDIO TRACK] Track 1: encoded=4948800 expected=4948800 diff=+0 (+0.000 ms) "
                "sources=[1,3,5,7]\n"
                "[STOP AUDIO TRACK] Track 2: encoded=4948800 expected=4948800 diff=+0 (+0.000 ms) "
                "sources=[4,6,8]\n"
                "[STOP AUDIO TRACK] Track 3: encoded=4948800 expected=4948800 diff=+0 (+0.000 ms) "
                "sources=[0,2]\n"
                "[VideoEncoder] Final packet timeline: target=103100000 us videoEnd=103100000 us "
                "audioMinEnd=103100000 us audioMaxEnd=103100000 us maxPacketDelta=0 us streams(v=1 a=3) "
                "audioPastTarget=0\n"
                "[VideoEncoder] Final metadata durations: target=103100000 us video=103100000 us "
                "audioMin=103100000 us audioMax=103100000 us maxDelta=0 us streams(v=1 a=3) "
                "overload(encoder=0 mux=0) backpressure=0\n"
            ),
        )
        truncated_smoothness_report = classify_session_triage(
            wgc_sync_delay_20260620_173427_truncated_smoothness
        )
        truncated_smoothness_summary = truncated_smoothness_report["evidence"]["wgc_smoothness_summary"][0]
        assert truncated_smoothness_summary["policy_added_repeats"] == 221
        assert truncated_smoothness_summary["wgc_smoothness_evidence_incomplete"] == 1
        assert "wgc_smoothness_evidence_incomplete" in truncated_smoothness_report["verdicts"]
        assert "wgc_cfr_smoothness_not_maximal" in truncated_smoothness_report["verdicts"]
        assert "ce_visual_timeline_fault" in truncated_smoothness_report["verdicts"]
        assert "ce_audio_timeline_fault" not in truncated_smoothness_report["verdicts"]
