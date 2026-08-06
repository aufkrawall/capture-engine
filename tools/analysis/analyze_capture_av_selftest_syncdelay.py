if False:

        wgc_sync_delay_20260620_173427_split_smoothness = make_session(
            "wgc_sync_delay_20260620_173427_split_smoothness",
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
                "startupReserveReason=partial_span_timeout\n"
                "[WGC CFR SMOOTHNESS DELAY] delayReservoirLowWaterFrames=4 delayReservoirTargetFrames=5 "
                "delayReservoirLowWaterTicks=3049 realizedDelayAvg=31990us realizedDelayMin=22427us "
                "realizedDelayMax=193734us delayResidualAvg=290/2377us delayResidualMax=161453us "
                "delayResidualP95=5000us delayResidualLateMax=9854us delayResidualEarlyMax=161453us "
                "rawResidualAvg=370/2403us rawResidualMax=79471us rawResidualP95=5000us "
                "rawResidualLateMax=9958us rawResidualEarlyMax=79471us "
                "predictedResidualAvg=290/2377us predictedResidualP95=5000us "
                "predictedResidualLateMax=9854us rawMinusPredictedAvg=80/80us "
                "rawMinusPredictedMax=163331us\n"
                "[WGC CFR SMOOTHNESS REPEAT] delayResidualRelaxedSelections=515 "
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
                "delaySourceRecoveryTicks=6845\n"
                "[WGC CFR SMOOTHNESS VERDICT] delayPostSelectionRejectedSync=0 "
                "delayPostSelectionRescuedSync=0 sourceRepeatLowerBound=1097 excessRepeats=221 "
                "policyAddedRepeats=221 excessRepeatClusters=0 excessRepeatClusterMax=0 "
                "smoothnessNotMaximal=1 mixedPolicyFault=0\n"
                "[STOP AUDIO TRACK] Track 1: encoded=4948800 expected=4948800 diff=+0 (+0.000 ms) "
                "sources=[1,3,5,7]\n"
                "[VideoEncoder] Final packet timeline: target=103100000 us videoEnd=103100000 us "
                "audioMinEnd=103100000 us audioMaxEnd=103100000 us maxPacketDelta=0 us streams(v=1 a=3) "
                "audioPastTarget=0\n"
            ),
        )
        split_smoothness_report = classify_session_triage(wgc_sync_delay_20260620_173427_split_smoothness)
        split_smoothness_summary = split_smoothness_report["evidence"]["wgc_smoothness_summary"][0]
        assert split_smoothness_summary["policy_added_repeats"] == 221
        assert split_smoothness_summary.get("wgc_smoothness_evidence_incomplete", 0) == 0
        assert "wgc_smoothness_evidence_incomplete" not in split_smoothness_report["verdicts"]
        assert "wgc_cfr_smoothness_not_maximal" in split_smoothness_report["verdicts"]
        assert "ce_visual_timeline_fault" in split_smoothness_report["verdicts"]
        assert "ce_audio_timeline_fault" not in split_smoothness_report["verdicts"]

        wgc_sync_delay_20260619_162335 = make_session(
            "wgc_sync_delay_20260619_162335",
            media=(
                "[WGC CFR SUMMARY] Live=10617 Dup=1814 DupPct=17.0% NoFresh=239pm NoReserve=260pm "
                "DupReason(src=1814 def=0 timer=0 drain=0) SourceLimitedRepeats=1814 StarvedEpisodes=227 "
                "longest=3156ms longestDup=189 worstIn=4 worstDel=4\n"
                "[WGC CFR SMOOTHNESS SUMMARY] encoderLimitedDrops=0 maxDropTicks=0 cadenceEvents=86 "
                "phaseErrorMax=6773us shortfallMax=0.0ms staleDebtDrops=41 liveRebase=0/0 "
                "tooNewRepeats=1213 syncDelayHolds=1213 tooNewLeadMax=140542us avDelay=32.4ms "
                "startupDelay=32.4ms scheduleOffset=303131us effectiveDelay=32.4ms "
                "lowSourceBypass=0 modeMismatch=0 sourceBacktrack=0 "
                "syncDelaySourceLimitedHolds=972 syncDelayPolicyHolds=241 startupReserveFrames=26 "
                "startupReserveSpan=189883us startupDelayTarget=32432us startupReserveSelected=1 "
                "startupReserveReason=selected delayReservoirLowWaterFrames=4 "
                "delayReservoirTargetFrames=5 delayReservoirLowWaterTicks=2528 realizedDelayAvg=31820us "
                "realizedDelayMin=20115us realizedDelayMax=199459us delayResidualAvg=611/2420us "
                "delayResidualMax=167027us delayResidualP95=5000us delayResidualLateMax=12317us "
                "delayResidualEarlyMax=167027us delayResidualRelaxedSelections=585 "
                "delayResidualRelaxedMax=12317us delayResidualRelaxedRejectedSync=66052 "
                "delayRepeatClusterPressure=218 delayRepeatClusterMax=189 "
                "delayResidualRelaxedBetter=580 delayResidualRelaxedCluster=3 "
                "delayResidualRelaxedRejectedHeadroom=391 delayResidualRelaxedRejectedCost=5314 "
                "delaySourceRecoveryHolds=379 delaySourceRecoveryTicks=6658 mixedPolicyFault=0\n"
                "[STOP AUDIO TRACK] Track 1: encoded=4246800 expected=4246800 diff=+0 (+0.000 ms) "
                "sources=[1,3,5,7]\n"
                "[STOP AUDIO TRACK] Track 2: encoded=4246800 expected=4246800 diff=+0 (+0.000 ms) "
                "sources=[4,6,8]\n"
                "[STOP AUDIO TRACK] Track 3: encoded=4246800 expected=4246800 diff=+0 (+0.000 ms) "
                "sources=[0,2]\n"
                "[VideoEncoder] Final packet timeline: target=88475000 us videoEnd=88475000 us "
                "audioMinEnd=88475000 us audioMaxEnd=88475000 us maxPacketDelta=0 us streams(v=1 a=3) "
                "audioPastTarget=0\n"
                "[VideoEncoder] Final metadata durations: target=88475000 us video=88475000 us "
                "audioMin=88475000 us audioMax=88475000 us maxDelta=0 us streams(v=1 a=3) "
                "overload(encoder=0 mux=0) backpressure=0\n"
            ),
        )
        delay_162335_report = classify_session_triage(wgc_sync_delay_20260619_162335)
        assert "wgc_source_starvation" in delay_162335_report["verdicts"]
        assert "wgc_av_sync_delay_residual" in delay_162335_report["verdicts"]
        assert "wgc_sync_delay_policy_fault" in delay_162335_report["verdicts"]
        assert "ce_visual_timeline_fault" in delay_162335_report["verdicts"]
        assert "ce_audio_timeline_fault" not in delay_162335_report["verdicts"]

        wgc_raw_timestamp_domain_mismatch = make_session(
            "wgc_raw_timestamp_domain_mismatch",
            media=(
                "[WGC CFR SMOOTHNESS SUMMARY] encoderLimitedDrops=0 maxDropTicks=0 cadenceEvents=4 "
                "phaseErrorMax=6000us shortfallMax=0.0ms staleDebtDrops=0 liveRebase=0/0 "
                "tooNewRepeats=12 syncDelayHolds=12 tooNewLeadMax=9000us avDelay=32.0ms "
                "startupDelay=32.0ms scheduleOffset=12000us effectiveDelay=32.0ms "
                "lowSourceBypass=0 modeMismatch=0 sourceBacktrack=0 "
                "syncDelaySourceLimitedHolds=12 syncDelayPolicyHolds=0 startupReserveFrames=5 "
                "startupReserveSpan=42000us startupDelayTarget=32000us startupReserveSelected=1 "
                "startupReserveReason=selected delayReservoirLowWaterFrames=4 "
                "delayReservoirTargetFrames=5 delayReservoirLowWaterTicks=0 realizedDelayAvg=31500us "
                "realizedDelayMin=29000us realizedDelayMax=34000us delayResidualAvg=500/2200us "
                "delayResidualMax=6000us delayResidualP95=5000us delayResidualLateMax=6000us "
                "delayResidualEarlyMax=2000us rawResidualAvg=6200/6200us rawResidualMax=14000us "
                "rawResidualP95=12000us rawResidualLateMax=14000us rawResidualEarlyMax=1000us "
                "predictedResidualAvg=500/2200us predictedResidualP95=5000us "
                "predictedResidualLateMax=6000us rawMinusPredictedAvg=5700/5700us "
                "rawMinusPredictedMax=11000us delayResidualRelaxedSelections=4 "
                "delayResidualRelaxedMax=6000us delayResidualRelaxedRejectedSync=0 "
                "delayRepeatClusterPressure=0 delayRepeatClusterMax=0 "
                "delayResidualRelaxedBetter=4 delayResidualRelaxedCluster=0 "
                "delayResidualRelaxedRejectedHeadroom=0 delayResidualRelaxedRejectedCost=0 "
                "delaySourceRecoveryHolds=0 delaySourceRecoveryTicks=0 delayPostSelectionRejectedSync=0 "
                "mixedPolicyFault=0\n"
            ),
        )
        raw_domain_report = classify_session_triage(wgc_raw_timestamp_domain_mismatch)
        raw_domain_summary = raw_domain_report["evidence"]["wgc_smoothness_summary"][0]
        assert raw_domain_summary["raw_residual_late_max_us"] == 14000
        assert "wgc_timestamp_domain_mismatch" in raw_domain_report["verdicts"]
        assert "wgc_av_sync_delay_residual" in raw_domain_report["verdicts"]
        assert "ce_visual_timeline_fault" in raw_domain_report["verdicts"]

        wgc_post_selection_reject = make_session(
            "wgc_post_selection_reject",
            media=(
                "[WGC CFR SMOOTHNESS SUMMARY] encoderLimitedDrops=0 maxDropTicks=0 cadenceEvents=4 "
                "phaseErrorMax=6000us shortfallMax=0.0ms staleDebtDrops=0 liveRebase=0/0 "
                "tooNewRepeats=14 syncDelayHolds=14 tooNewLeadMax=11000us avDelay=32.0ms "
                "startupDelay=32.0ms scheduleOffset=12000us effectiveDelay=32.0ms "
                "lowSourceBypass=0 modeMismatch=0 sourceBacktrack=0 "
                "syncDelaySourceLimitedHolds=0 syncDelayPolicyHolds=14 startupReserveFrames=5 "
                "startupReserveSpan=42000us startupDelayTarget=32000us startupReserveSelected=1 "
                "startupReserveReason=selected delayReservoirLowWaterFrames=4 "
                "delayReservoirTargetFrames=5 delayReservoirLowWaterTicks=0 realizedDelayAvg=31500us "
                "realizedDelayMin=29000us realizedDelayMax=34000us delayResidualAvg=500/2200us "
                "delayResidualMax=6000us delayResidualP95=5000us delayResidualLateMax=6000us "
                "delayResidualEarlyMax=2000us rawResidualAvg=500/2200us rawResidualMax=6000us "
                "rawResidualP95=5000us rawResidualLateMax=6000us rawResidualEarlyMax=2000us "
                "predictedResidualAvg=500/2200us predictedResidualP95=5000us "
                "predictedResidualLateMax=6000us rawMinusPredictedAvg=0/0us rawMinusPredictedMax=0us "
                "delayResidualRelaxedSelections=4 delayResidualRelaxedMax=6000us "
                "delayResidualRelaxedRejectedSync=3 delayRepeatClusterPressure=2 delayRepeatClusterMax=3 "
                "delayResidualRelaxedBetter=4 delayResidualRelaxedCluster=0 "
                "delayResidualRelaxedRejectedHeadroom=0 delayResidualRelaxedRejectedCost=0 "
                "delaySourceRecoveryHolds=0 delaySourceRecoveryTicks=0 delayPostSelectionRejectedSync=3 "
                "mixedPolicyFault=0\n"
            ),
        )
        post_reject_report = classify_session_triage(wgc_post_selection_reject)
        assert "wgc_active_delay_post_selection_reject" in post_reject_report["verdicts"]
        assert "wgc_cfr_smoothness_not_maximal" in post_reject_report["verdicts"]
        assert "wgc_sync_delay_policy_fault" in post_reject_report["verdicts"]
        assert "ce_visual_timeline_fault" in post_reject_report["verdicts"]

        wgc_source_limited_lower_bound = make_session(
            "wgc_source_limited_lower_bound",
            media=(
                "[WGC CFR SUMMARY] Live=2400 Dup=260 DupPct=10.8% NoFresh=160pm NoReserve=140pm "
                "DupReason(src=260 def=0 timer=0 drain=0) SourceLimitedRepeats=260 StarvedEpisodes=60 "
                "longest=900ms longestDup=18 worstIn=90 worstDel=90\n"
                "[WGC CFR SMOOTHNESS SUMMARY] encoderLimitedDrops=0 maxDropTicks=0 cadenceEvents=12 "
                "phaseErrorMax=7000us shortfallMax=0.0ms staleDebtDrops=0 liveRebase=0/0 "
                "tooNewRepeats=260 syncDelayHolds=260 tooNewLeadMax=40000us avDelay=31.7ms "
                "startupDelay=31.7ms scheduleOffset=40000us effectiveDelay=31.7ms "
                "lowSourceBypass=0 modeMismatch=0 sourceBacktrack=0 "
                "syncDelaySourceLimitedHolds=260 syncDelayPolicyHolds=0 startupReserveFrames=5 "
                "startupReserveSpan=41000us startupDelayTarget=31700us startupReserveSelected=1 "
                "startupReserveReason=selected delayReservoirLowWaterFrames=4 "
                "delayReservoirTargetFrames=5 delayReservoirLowWaterTicks=120 realizedDelayAvg=31000us "
                "realizedDelayMin=25000us realizedDelayMax=36000us delayResidualAvg=700/2300us "
                "delayResidualMax=7000us delayResidualP95=5000us delayResidualLateMax=7000us "
                "delayResidualEarlyMax=4000us rawResidualAvg=800/2400us rawResidualMax=8000us "
                "rawResidualP95=5000us rawResidualLateMax=8000us rawResidualEarlyMax=4000us "
                "predictedResidualAvg=700/2300us predictedResidualP95=5000us "
                "predictedResidualLateMax=7000us rawMinusPredictedAvg=100/500us "
                "rawMinusPredictedMax=1000us delayResidualRelaxedSelections=80 "
                "delayResidualRelaxedMax=7000us delayResidualRelaxedRejectedSync=0 "
                "delayRepeatClusterPressure=0 delayRepeatClusterMax=0 "
                "delayResidualRelaxedBetter=40 delayResidualRelaxedCluster=40 "
                "delayResidualRelaxedRejectedHeadroom=0 delayResidualRelaxedRejectedCost=0 "
                "delaySoftLateRejected=0 delaySoftLateAccepted=0 "
                "delayOlderFrameAvoidedRepeat=80 delaySourceLimitedRepeats=260 "
                "delaySourceRecoveryHolds=80 delaySourceRecoveryTicks=400 "
                "delayPostSelectionRejectedSync=0 delayPostSelectionRescuedSync=0 "
                "sourceRepeatLowerBound=260 excessRepeats=0 policyAddedRepeats=0 "
                "excessRepeatClusters=0 excessRepeatClusterMax=0 smoothnessNotMaximal=0 "
                "mixedPolicyFault=0\n"
            ),
        )
        lower_bound_report = classify_session_triage(wgc_source_limited_lower_bound)
        lower_bound_summary = lower_bound_report["evidence"]["wgc_smoothness_summary"][0]
        assert lower_bound_summary["source_repeat_lower_bound"] == 260
        assert lower_bound_summary["excess_repeats"] == 0
        assert lower_bound_summary["delay_older_frame_avoided_repeat"] == 80
        assert lower_bound_summary["delay_source_limited_repeats"] == 260
        assert "wgc_source_starvation" in lower_bound_report["verdicts"]
        assert "wgc_sync_delay_reserve_pressure" in lower_bound_report["verdicts"]
        assert "wgc_cfr_smoothness_not_maximal" not in lower_bound_report["verdicts"]
        assert "ce_visual_timeline_fault" not in lower_bound_report["verdicts"]

        wgc_small_policy_repeat_not_optimal = make_session(
            "wgc_small_policy_repeat_not_optimal",
            media=(
                "[WGC CFR SUMMARY] Live=7000 Dup=320 DupPct=4.6% NoFresh=45pm NoReserve=20pm "
                "DupReason(src=238 def=0 timer=0 drain=0) SourceLimitedRepeats=238 StarvedEpisodes=8 "
                "longest=120ms longestDup=9 worstIn=102 worstDel=102\n"
                "[WGC CFR SMOOTHNESS SUMMARY] encoderLimitedDrops=0 maxDropTicks=0 cadenceEvents=25 "
                "phaseErrorMax=6000us shortfallMax=0.0ms staleDebtDrops=0 liveRebase=0/0 "
                "tooNewRepeats=320 syncDelayHolds=320 tooNewLeadMax=7000us avDelay=30.0ms "
                "startupDelay=230.0ms scheduleOffset=0us effectiveDelay=230.0ms "
                "lowSourceBypass=0 modeMismatch=0 sourceBacktrack=0 "
                "syncDelaySourceLimitedHolds=238 syncDelayPolicyHolds=82 startupReserveFrames=24 "
                "startupReserveSpan=200000us startupDelayTarget=270000us startupReserveSelected=0 "
                "startupReserveReason=partial_span_timeout\n"
                "[WGC CFR SMOOTHNESS VERDICT] delayPostSelectionRejectedSync=0 "
                "delayPostSelectionRescuedSync=0 sourceRepeatLowerBound=238 excessRepeats=82 "
                "policyAddedRepeats=82 excessRepeatClusters=0 excessRepeatClusterMax=4 "
                "smoothnessNotMaximal=0 mixedPolicyFault=0\n"
            ),
        )
        small_policy_report = classify_session_triage(wgc_small_policy_repeat_not_optimal)
        assert "wgc_cfr_smoothness_not_maximal" in small_policy_report["verdicts"]

        wgc_post_stall_recovery_repeat = make_session(
            "wgc_post_stall_recovery_repeat",
            media=(
                "[WGC CFR SUMMARY] Live=1200 Dup=160 DupPct=13.3% NoFresh=130pm NoReserve=90pm "
                "DupReason(src=160 def=0 timer=0 drain=0) SourceLimitedRepeats=120 StarvedEpisodes=8 "
                "longest=640ms longestDup=16 worstIn=70 worstDel=70\n"
                "[WGC CFR SMOOTHNESS SUMMARY] encoderLimitedDrops=0 maxDropTicks=0 cadenceEvents=12 "
                "phaseErrorMax=7000us shortfallMax=0.0ms staleDebtDrops=0 liveRebase=0/0 "
                "tooNewRepeats=160 syncDelayHolds=160 tooNewLeadMax=38000us avDelay=32.0ms "
                "startupDelay=32.0ms scheduleOffset=0us effectiveDelay=32.0ms "
                "lowSourceBypass=0 modeMismatch=0 sourceBacktrack=0 "
                "syncDelaySourceLimitedHolds=120 syncDelayPolicyHolds=40 startupReserveFrames=5 "
                "startupReserveSpan=42000us startupDelayTarget=32000us startupReserveSelected=1 "
                "startupReserveReason=selected\n"
                "[WGC CFR SMOOTHNESS DELAY] delayReservoirLowWaterFrames=4 delayReservoirTargetFrames=5 "
                "delayReservoirLowWaterTicks=80 realizedDelayAvg=32000us realizedDelayMin=28000us "
                "realizedDelayMax=39000us delayResidualAvg=600/2100us delayResidualMax=7000us "
                "delayResidualP95=5000us delayResidualLateMax=7000us delayResidualEarlyMax=4000us "
                "rawResidualAvg=700/2200us rawResidualMax=8000us rawResidualP95=5000us "
                "rawResidualLateMax=8000us rawResidualEarlyMax=4000us predictedResidualAvg=600/2100us "
                "predictedResidualP95=5000us predictedResidualLateMax=7000us "
                "rawMinusPredictedAvg=100/300us rawMinusPredictedMax=900us\n"
                "[WGC CFR SMOOTHNESS REPEAT] delayResidualRelaxedSelections=40 "
                "delayResidualRelaxedMax=6000us delayResidualRelaxedRejectedSync=0 "
                "delayRepeatClusterPressure=40 delayRepeatClusterMax=8 delayResidualRelaxedBetter=20 "
                "delayResidualRelaxedCluster=20 delayResidualRelaxedRejectedHeadroom=0 "
                "delayResidualRelaxedRejectedCost=0 delaySoftLateRejected=0 delaySoftLateAccepted=0 "
                "delayOlderFrameAvoidedRepeat=20 delaySourceLimitedRepeats=120 "
                "delayRepeatRescue=0/40 delayRepeatRescueRejected=0/0/40 "
                "delayRepeatPromoted=0/40 delayRepeatPromoteRejectedSoft=0 "
                "delayRepeatSafeAfterPromote=40 delayRepeatSafeCandidate=40 delayRepeatNoSafeCandidate=120 "
                "delayRepeatSoftSafeCandidate=40 delayRepeatNoSoftSafeCandidate=120 "
                "delayRepeatWindowClass=0/40/120 delayRepeatWindowState=0/0/120/120/40 "
                "delayPostStallSafeFrames=60 delayRepeatReserveMax=5/42000us "
                "delaySourceRecoveryHolds=120 delaySourceRecoveryTicks=600\n"
                "[WGC CFR SMOOTHNESS VERDICT] delayPostSelectionRejectedSync=0 "
                "delayPostSelectionRescuedSync=0 sourceRepeatLowerBound=120 excessRepeats=40 "
                "policyAddedRepeats=40 excessRepeatClusters=8 excessRepeatClusterMax=8 "
                "smoothnessNotMaximal=0 mixedPolicyFault=0\n"
            ),
        )
        post_stall_report = classify_session_triage(wgc_post_stall_recovery_repeat)
        post_stall_summary = post_stall_report["evidence"]["wgc_smoothness_summary"][0]
        assert post_stall_summary["delay_repeat_state_post_stall"] == 40
        assert post_stall_summary["delay_post_stall_safe_frames"] == 60
        assert post_stall_summary["delay_repeat_soft_safe_candidate"] == 40
        assert "wgc_post_stall_recovery_fault" in post_stall_report["verdicts"]
        assert "ce_visual_timeline_fault" in post_stall_report["verdicts"]

        wgc_hard_only_repeat_not_soft_safe = make_session(
            "wgc_hard_only_repeat_not_soft_safe",
            media=(
                "[WGC CFR SUMMARY] Live=1200 Dup=160 DupPct=13.3% NoFresh=130pm NoReserve=90pm "
                "DupReason(src=160 def=0 timer=0 drain=0) SourceLimitedRepeats=160 StarvedEpisodes=8 "
                "longest=640ms longestDup=16 worstIn=70 worstDel=70\n"
                "[WGC CFR SMOOTHNESS SUMMARY] encoderLimitedDrops=0 maxDropTicks=0 cadenceEvents=12 "
                "phaseErrorMax=7000us shortfallMax=0.0ms staleDebtDrops=0 liveRebase=0/0 "
                "tooNewRepeats=160 syncDelayHolds=160 tooNewLeadMax=38000us avDelay=32.0ms "
                "startupDelay=32.0ms scheduleOffset=0us effectiveDelay=32.0ms "
                "lowSourceBypass=0 modeMismatch=0 sourceBacktrack=0 "
                "syncDelaySourceLimitedHolds=160 syncDelayPolicyHolds=0 startupReserveFrames=5 "
                "startupReserveSpan=42000us startupDelayTarget=32000us startupReserveSelected=1 "
                "startupReserveReason=selected\n"
                "[WGC CFR SMOOTHNESS DELAY] delayReservoirLowWaterFrames=4 delayReservoirTargetFrames=5 "
                "delayReservoirLowWaterTicks=80 realizedDelayAvg=32000us realizedDelayMin=28000us "
                "realizedDelayMax=39000us delayResidualAvg=600/2100us delayResidualMax=7000us "
                "delayResidualP95=5000us delayResidualLateMax=7000us delayResidualEarlyMax=4000us "
                "rawResidualAvg=700/2200us rawResidualMax=8000us rawResidualP95=5000us "
                "rawResidualLateMax=8000us rawResidualEarlyMax=4000us predictedResidualAvg=600/2100us "
                "predictedResidualP95=5000us predictedResidualLateMax=7000us "
                "rawMinusPredictedAvg=100/300us rawMinusPredictedMax=900us\n"
                "[WGC CFR SMOOTHNESS REPEAT] delayResidualRelaxedSelections=0 "
                "delayResidualRelaxedMax=0us delayResidualRelaxedRejectedSync=0 "
                "delayRepeatClusterPressure=0 delayRepeatClusterMax=0 delayResidualRelaxedBetter=0 "
                "delayResidualRelaxedCluster=0 delayResidualRelaxedRejectedHeadroom=0 "
                "delayResidualRelaxedRejectedCost=0 delaySoftLateRejected=40 delaySoftLateAccepted=0 "
                "delayOlderFrameAvoidedRepeat=0 delaySourceLimitedRepeats=160 "
                "delayRepeatRescue=0/160 delayRepeatRescueRejected=0/40/0 "
                "delayRepeatPromoted=0/160 delayRepeatPromoteRejectedSoft=40 "
                "delayRepeatSafeAfterPromote=40 delayRepeatSafeCandidate=40 delayRepeatNoSafeCandidate=120 "
                "delayRepeatSoftSafeCandidate=0 delayRepeatNoSoftSafeCandidate=160 "
                "delayRepeatWindowClass=0/40/120 delayRepeatWindowState=0/0/120/120/40 "
                "delayPostStallSafeFrames=0 delayRepeatReserveMax=5/42000us "
                "delaySourceRecoveryHolds=120 delaySourceRecoveryTicks=600\n"
                "[WGC CFR SMOOTHNESS VERDICT] delayPostSelectionRejectedSync=0 "
                "delayPostSelectionRescuedSync=0 sourceRepeatLowerBound=160 excessRepeats=0 "
                "policyAddedRepeats=0 excessRepeatClusters=0 excessRepeatClusterMax=0 "
                "smoothnessNotMaximal=0 mixedPolicyFault=0\n"
            ),
        )
        hard_only_report = classify_session_triage(wgc_hard_only_repeat_not_soft_safe)
        hard_only_summary = hard_only_report["evidence"]["wgc_smoothness_summary"][0]
        assert hard_only_summary["delay_repeat_safe_candidate"] == 40
        assert hard_only_summary["delay_repeat_soft_safe_candidate"] == 0
        assert "wgc_repeat_despite_safe_candidate" not in hard_only_report["verdicts"]
        assert "wgc_post_stall_recovery_fault" not in hard_only_report["verdicts"]

        wgc_sync_delay_fortibad_audio_late_risk = make_session(
            "wgc_sync_delay_fortibad_audio_late_risk",
            media=(
                "[WGC CFR SUMMARY] Live=25926 Dup=2489 DupPct=9.6% NoFresh=120pm NoReserve=140pm "
                "DupReason(src=2489 def=0 timer=0 drain=0) SourceLimitedRepeats=2489 StarvedEpisodes=579 "
                "longest=1922ms longestDup=190 worstIn=4 worstDel=4\n"
                "[WGC CFR SMOOTHNESS SUMMARY] encoderLimitedDrops=0 maxDropTicks=0 cadenceEvents=221 "
                "phaseErrorMax=83180us shortfallMax=0.0ms staleDebtDrops=1 liveRebase=0/0 "
                "tooNewRepeats=2147 syncDelayHolds=2147 tooNewLeadMax=150000us avDelay=29.5ms "
                "startupDelay=29.5ms scheduleOffset=0us effectiveDelay=29.5ms "
                "lowSourceBypass=0 modeMismatch=0 sourceBacktrack=0 "
                "syncDelaySourceLimitedHolds=2147 syncDelayPolicyHolds=0 startupReserveFrames=8 "
                "startupReserveSpan=58000us startupDelayTarget=29500us startupReserveSelected=1 "
                "startupReserveReason=selected\n"
                "[WGC CFR SMOOTHNESS DELAY] delayReservoirLowWaterFrames=4 delayReservoirTargetFrames=5 "
                "delayReservoirLowWaterTicks=1600 realizedDelayAvg=29100us realizedDelayMin=21000us "
                "realizedDelayMax=112680us delayResidualAvg=351/2249us delayResidualMax=83180us "
                "delayResidualP95=5000us delayResidualLateMax=9858us delayResidualEarlyMax=83180us "
                "rawResidualAvg=389/2344us rawResidualMax=83180us rawResidualP95=5000us "
                "rawResidualLateMax=9858us rawResidualEarlyMax=83180us predictedResidualAvg=351/2249us "
                "predictedResidualP95=5000us predictedResidualLateMax=9858us "
                "rawMinusPredictedAvg=38/300us rawMinusPredictedMax=1000us\n"
                "[WGC CFR SMOOTHNESS REPEAT] delayResidualRelaxedSelections=14717 "
                "delayResidualRelaxedMax=9858us delayResidualRelaxedRejectedSync=0 "
                "delayRepeatClusterPressure=0 delayRepeatClusterMax=0 delayResidualRelaxedBetter=12000 "
                "delayResidualRelaxedCluster=2717 delayResidualRelaxedRejectedHeadroom=0 "
                "delayResidualRelaxedRejectedCost=0 delaySoftLateRejected=14717 delaySoftLateAccepted=2211 "
                "delayOlderFrameAvoidedRepeat=16 delaySourceLimitedRepeats=2147 "
                "delayRepeatRescue=16/2163 delayRepeatRescueRejected=0/1233/0 "
                "delayRepeatPromoted=16/2163 delayRepeatPromoteRejectedSoft=1233 "
                "delayRepeatSafeAfterPromote=0 delayRepeatSafeCandidate=1329 delayRepeatNoSafeCandidate=818 "
                "delayRepeatSoftSafeCandidate=0 delayRepeatNoSoftSafeCandidate=2147 "
                "delayRepeatWindowClass=0/0/2147 delayRepeatWindowState=0/0/2147/2147/0 "
                "delayPostStallSafeFrames=0 delayRepeatReserveMax=5/58000us "
                "delaySourceRecoveryHolds=0 delaySourceRecoveryTicks=0 "
                "delayNearCapAccepted=2211 delayHardOnlyCandidates=1329 "
                "delaySyncProtectedRepeats=2147 delayOldestSoftSafeAgeMax=0us\n"
                "[WGC CFR SMOOTHNESS VERDICT] delayPostSelectionRejectedSync=0 "
                "delayPostSelectionRescuedSync=0 sourceRepeatLowerBound=2147 excessRepeats=0 "
                "policyAddedRepeats=0 excessRepeatClusters=0 excessRepeatClusterMax=0 "
                "smoothnessNotMaximal=0 mixedPolicyFault=0\n"
                "[STOP AUDIO TRACK] Track 1: encoded=1000 expected=1000 diff=+0 (+0.000 ms) sources=[1]\n"
                "[VideoEncoder] Final packet timeline: target=1000 us videoEnd=1000 us "
                "audioMinEnd=1000 us audioMaxEnd=1000 us maxPacketDelta=0 us streams(v=1 a=1) audioPastTarget=0\n"
                "[VideoEncoder] Final metadata durations: target=1000 us video=1000 us "
                "audioMin=1000 us audioMax=1000 us maxDelta=0 us streams(v=1 a=1) "
                "overload(encoder=0 mux=0) backpressure=0\n"
            ),
        )
        fortibad_risk_report = classify_session_triage(wgc_sync_delay_fortibad_audio_late_risk)
        fortibad_risk_summary = fortibad_risk_report["evidence"]["wgc_smoothness_summary"][0]
        assert fortibad_risk_summary["delay_soft_late_accepted"] == 2211
        assert fortibad_risk_summary["delay_near_cap_accepted"] == 2211
        assert fortibad_risk_summary["policy_added_repeats"] == 0
        assert "wgc_audio_late_risk" in fortibad_risk_report["verdicts"]
        assert "wgc_cfr_smoothness_not_maximal" not in fortibad_risk_report["verdicts"]
        assert "ce_visual_timeline_fault" in fortibad_risk_report["verdicts"]
        assert "ce_audio_timeline_fault" not in fortibad_risk_report["verdicts"]

        wgc_sync_delay_source_limited_ceiling = make_session(
            "wgc_sync_delay_source_limited_ceiling",
            media=(
                "[WGC CFR SUMMARY] Live=131396 Dup=11732 DupPct=8.9% NoFresh=119pm NoReserve=123pm "
                "DupReason(src=11731 def=0 timer=1 drain=0) SourceLimitedRepeats=11731 StarvedEpisodes=3084 "
                "longest=7547ms longestDup=319 worstIn=24 worstDel=24\n"
                "[WGC CFR SMOOTHNESS SUMMARY] encoderLimitedDrops=0 maxDropTicks=0 cadenceEvents=1069 "
                "phaseErrorMax=7246us shortfallMax=0.0ms staleDebtDrops=12 liveRebase=0/0 "
                "tooNewRepeats=11302 syncDelayHolds=11302 tooNewLeadMax=140610us avDelay=33.0ms "
                "startupDelay=33.0ms scheduleOffset=81368us effectiveDelay=33.0ms "
                "lowSourceBypass=0 modeMismatch=0 sourceBacktrack=0 "
                "syncDelaySourceLimitedHolds=11302 syncDelayPolicyHolds=0 startupReserveFrames=33 "
                "startupReserveSpan=228518us startupDelayTarget=33045us startupReserveSelected=1 "
                "startupReserveReason=selected\n"
                "[WGC CFR SMOOTHNESS DELAY] delayReservoirLowWaterFrames=4 delayReservoirTargetFrames=5 "
                "delayReservoirLowWaterTicks=16239 realizedDelayAvg=32792us realizedDelayMin=23269us "
                "realizedDelayMax=195397us delayResidualAvg=252/2164us delayResidualMax=162352us "
                "delayResidualP95=5000us delayResidualLateMax=9776us delayResidualEarlyMax=162352us "
                "rawResidualAvg=264/2276us rawResidualMax=49465us rawResidualP95=5000us "
                "rawResidualLateMax=9981us rawResidualEarlyMax=49465us predictedResidualAvg=252/2164us "
                "predictedResidualP95=5000us predictedResidualLateMax=9776us "
                "rawMinusPredictedAvg=11/11us rawMinusPredictedMax=167508us\n"
                "[WGC CFR SMOOTHNESS REPEAT] delayResidualRelaxedSelections=4141 "
                "delayResidualRelaxedMax=9776us delayResidualRelaxedRejectedSync=1916996 "
                "delayRepeatClusterPressure=641 delayRepeatClusterMax=176 delayResidualRelaxedBetter=3126 "
                "delayResidualRelaxedCluster=1 delayResidualRelaxedRejectedHeadroom=97704 "
                "delayResidualRelaxedRejectedCost=2108 delaySoftLateRejected=97640 delaySoftLateAccepted=0 "
                "delayOlderFrameAvoidedRepeat=115574 delaySourceLimitedRepeats=11328 "
                "delayRepeatRescue=49/11351 delayRepeatRescueRejected=42801/7965/212 "
                "delayRepeatPromoted=49/11351 delayRepeatPromoteRejectedSoft=7960 "
                "delayRepeatSafeAfterPromote=7531 delayRepeatSafeCandidate=7531 "
                "delayRepeatNoSafeCandidate=3771 delayRepeatSoftSafeCandidate=0 "
                "delayRepeatNoSoftSafeCandidate=11302 delayRepeatWindowClass=1012/6329/3961 "
                "delayRepeatWindowState=1012/6329/3961/3771/3082 delayPostStallSafeFrames=45288 "
                "delayRepeatReserveMax=9/56199us delaySourceRecoveryHolds=3804 "
                "delaySourceRecoveryTicks=50346 delayNearCapAccepted=2104 delayHardOnlyCandidates=7531 "
                "delaySyncProtectedRepeats=11302 delayOldestSoftSafeAgeMax=0us\n"
                "[WGC CFR SMOOTHNESS VERDICT] delayPostSelectionRejectedSync=0 "
                "delayPostSelectionRescuedSync=0 sourceRepeatLowerBound=11328 excessRepeats=0 "
                "policyAddedRepeats=0 excessRepeatClusters=0 excessRepeatClusterMax=0 "
                "smoothnessNotMaximal=0 mixedPolicyFault=0\n"
                "[STOP AUDIO TRACK] Track 1: encoded=52558800 expected=52558800 diff=+0 (+0.000 ms) sources=[1]\n"
                "[VideoEncoder] Final packet timeline: target=1094975000 us videoEnd=1094975000 us "
                "audioMinEnd=1094975000 us audioMaxEnd=1094975000 us maxPacketDelta=0 us "
                "streams(v=1 a=1) audioPastTarget=0\n"
                "[VideoEncoder] Final metadata durations: target=1094975000 us video=1094975000 us "
                "audioMin=1094975000 us audioMax=1094975000 us maxDelta=0 us streams(v=1 a=1) "
                "overload(encoder=0 mux=0) backpressure=0\n"
            ),
        )
        ceiling_report = classify_session_triage(wgc_sync_delay_source_limited_ceiling)
        ceiling_limits = ceiling_report["evidence"]["wgc_source_limits"]
        assert ceiling_limits["summary_duplicate"] == 11732
        assert ceiling_limits["summary_longest_dup_ticks"] == 319
        assert "wgc_source_limited_smoothness_ceiling" in ceiling_report["verdicts"]
        assert "wgc_audio_late_risk" not in ceiling_report["verdicts"]
        assert "wgc_cfr_smoothness_not_maximal" not in ceiling_report["verdicts"]
        assert "ce_visual_timeline_fault" not in ceiling_report["verdicts"]
        assert "ce_audio_timeline_fault" not in ceiling_report["verdicts"]

        wgc_sync_delay_20260619_214948 = make_session(
            "wgc_sync_delay_20260619_214948",
            media=(
                "[WGC CFR SUMMARY] Live=39375 Dup=4230 DupPct=9.7% NoFresh=180pm NoReserve=190pm "
                "DupReason(src=4230 def=0 timer=0 drain=0) SourceLimitedRepeats=4230 StarvedEpisodes=980 "
                "longest=6300ms longestDup=178 worstIn=4 worstDel=4\n"
                "[WGC CFR SMOOTHNESS SUMMARY] encoderLimitedDrops=0 maxDropTicks=0 cadenceEvents=221 "
                "phaseErrorMax=100000us shortfallMax=0.0ms staleDebtDrops=1 liveRebase=0/0 "
                "tooNewRepeats=3433 syncDelayHolds=3433 tooNewLeadMax=170000us avDelay=31.7ms "
                "startupDelay=31.7ms scheduleOffset=300000us effectiveDelay=31.7ms "
                "lowSourceBypass=0 modeMismatch=0 sourceBacktrack=0 "
                "syncDelaySourceLimitedHolds=2299 syncDelayPolicyHolds=1134 startupReserveFrames=20 "
                "startupReserveSpan=180000us startupDelayTarget=31700us startupReserveSelected=1 "
                "startupReserveReason=selected delayReservoirLowWaterFrames=4 "
                "delayReservoirTargetFrames=5 delayReservoirLowWaterTicks=6344 realizedDelayAvg=31183us "
                "realizedDelayMin=20000us realizedDelayMax=190000us delayResidualAvg=519/2333us "
                "delayResidualMax=160000us delayResidualP95=5000us delayResidualLateMax=9978us "
                "delayResidualEarlyMax=160000us rawResidualAvg=588/2450us rawResidualMax=170000us "
                "rawResidualP95=5000us rawResidualLateMax=9999us rawResidualEarlyMax=170000us "
                "predictedResidualAvg=519/2333us predictedResidualP95=5000us "
                "predictedResidualLateMax=9978us rawMinusPredictedAvg=69/600us "
                "rawMinusPredictedMax=1000us delayResidualRelaxedSelections=1719 "
                "delayResidualRelaxedMax=9978us delayResidualRelaxedRejectedSync=476062 "
                "delayRepeatClusterPressure=483 delayRepeatClusterMax=178 "
                "delayResidualRelaxedBetter=1713 delayResidualRelaxedCluster=3 "
                "delayResidualRelaxedRejectedHeadroom=252 delayResidualRelaxedRejectedCost=14635 "
                "delaySourceRecoveryHolds=874 delaySourceRecoveryTicks=15609 "
                "delayPostSelectionRejectedSync=1 delayPostSelectionRescuedSync=0 "
                "sourceRepeatLowerBound=2299 excessRepeats=1134 policyAddedRepeats=1134 "
                "excessRepeatClusters=483 excessRepeatClusterMax=178 smoothnessNotMaximal=1 "
                "mixedPolicyFault=1\n"
                "[STOP AUDIO TRACK] Track 1: encoded=1000 expected=1000 diff=+0 (+0.000 ms) sources=[1]\n"
                "[VideoEncoder] Final packet timeline: target=1000 us videoEnd=1000 us "
                "audioMinEnd=1000 us audioMaxEnd=1000 us maxPacketDelta=0 us streams(v=1 a=1) audioPastTarget=0\n"
                "[VideoEncoder] Final metadata durations: target=1000 us video=1000 us "
                "audioMin=1000 us audioMax=1000 us maxDelta=0 us streams(v=1 a=1) "
                "overload(encoder=0 mux=0) backpressure=0\n"
            ),
        )
        delay_214948_report = classify_session_triage(wgc_sync_delay_20260619_214948)
        delay_214948_summary = delay_214948_report["evidence"]["wgc_smoothness_summary"][0]
        assert delay_214948_summary["policy_added_repeats"] == 1134
        assert delay_214948_summary["excess_repeat_cluster_max_ticks"] == 178
        assert "wgc_source_starvation" in delay_214948_report["verdicts"]
        assert "wgc_active_delay_post_selection_reject" in delay_214948_report["verdicts"]
        assert "wgc_cfr_smoothness_not_maximal" in delay_214948_report["verdicts"]
        assert "wgc_sync_delay_policy_fault" in delay_214948_report["verdicts"]
        assert "ce_visual_timeline_fault" in delay_214948_report["verdicts"]
        assert "ce_audio_timeline_fault" not in delay_214948_report["verdicts"]

        wgc_sync_delay_long_run_p5_mixed_pressure = make_session(
            "wgc_sync_delay_long_run_p5_mixed_pressure",
            media=(
                "[WGC CFR SUMMARY] Live=150230 Dup=13764 DupPct=9.1% NoFresh=142pm NoReserve=148pm "
                "DupReason(src=13764 def=0 timer=0 drain=0) SourceLimitedRepeats=13764 StarvedEpisodes=3743 "
                "longest=11406ms longestDup=528 worstIn=24 worstDel=24\n"
                "[WGC CFR SMOOTHNESS SUMMARY] encoderLimitedDrops=0 maxDropTicks=0 cadenceEvents=1235 "
                "phaseErrorMax=279304us shortfallMax=0.0ms staleDebtDrops=1 liveRebase=0/0 "
                "tooNewRepeats=13140 syncDelayHolds=13140 tooNewLeadMax=195524us avDelay=32.3ms "
                "startupDelay=32.3ms scheduleOffset=-236210us effectiveDelay=32.3ms "
                "lowSourceBypass=0 modeMismatch=0 sourceBacktrack=0 "
                "syncDelaySourceLimitedHolds=5859 syncDelayPolicyHolds=7281 startupReserveFrames=1 "
                "startupReserveSpan=0us startupDelayTarget=32276us startupReserveSelected=0 "
                "startupReserveReason=reserve_timeout delayReservoirLowWaterFrames=4 "
                "delayReservoirTargetFrames=5 delayReservoirLowWaterTicks=22377 realizedDelayAvg=31715us "
                "realizedDelayMin=18476us realizedDelayMax=62767us delayResidualAvg=560/2297us "
                "delayResidualMax=30491us delayResidualP95=5000us delayResidualLateMax=13800us "
                "delayResidualEarlyMax=30491us delayResidualRelaxedSelections=7917 "
                "delayResidualRelaxedMax=13800us delayResidualRelaxedRejectedSync=44 "
                "delayRepeatClusterPressure=7281 delayRepeatClusterMax=120 "
                "delayResidualRelaxedBetter=2400 delayResidualRelaxedCluster=5517 "
                "delayResidualRelaxedRejectedHeadroom=120 delayResidualRelaxedRejectedCost=64 "
                "delaySourceRecoveryHolds=3200 delaySourceRecoveryTicks=8000 mixedPolicyFault=1\n"
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
                "[VideoEncoder] Final metadata durations: target=1251925000 us video=1251925000 us "
                "audioMin=1251925000 us audioMax=1251925000 us maxDelta=0 us streams(v=1 a=3) "
                "overload(encoder=0 mux=0) backpressure=0\n"
            ),
        )
        long_run_report = classify_session_triage(wgc_sync_delay_long_run_p5_mixed_pressure)
        long_run_summary = long_run_report["evidence"]["wgc_smoothness_summary"][0]
        assert long_run_summary["delay_relaxed_rejected_sync"] == 44
        assert long_run_summary["delay_repeat_cluster_pressure"] == 7281
        assert long_run_summary["delay_repeat_cluster_max_ticks"] == 120
        assert long_run_summary["delay_relaxed_better_target"] == 2400
        assert long_run_summary["delay_relaxed_repeat_cluster"] == 5517
        assert long_run_summary["delay_relaxed_rejected_headroom"] == 120
        assert long_run_summary["delay_relaxed_rejected_cost"] == 64
        assert long_run_summary["delay_source_recovery_holds"] == 3200
        assert long_run_summary["delay_source_recovery_ticks"] == 8000
        assert "wgc_source_starvation" in long_run_report["verdicts"]
        assert "wgc_av_sync_delay_residual" in long_run_report["verdicts"]
        assert "wgc_sync_delay_policy_fault" in long_run_report["verdicts"]
        assert "sparse_app_source_silence" in long_run_report["verdicts"]
        assert "ce_visual_timeline_fault" in long_run_report["verdicts"]
        assert "ce_audio_timeline_fault" not in long_run_report["verdicts"]
        assert long_run_report["evidence"]["exported_av_sync_ok"]

        wgc_sync_delay_policy_fault = make_session(
            "wgc_sync_delay_policy_fault",
            media=(
                "[WGC CFR SMOOTHNESS SUMMARY] encoderLimitedDrops=0 maxDropTicks=0 cadenceEvents=90 "
                "phaseErrorMax=200000us shortfallMax=41.7ms staleDebtDrops=0 liveRebase=0/0 "
                "tooNewRepeats=1280 syncDelayHolds=1280 tooNewLeadMax=261706us avDelay=34.8ms "
                "startupDelay=34.8ms scheduleOffset=58559us effectiveDelay=34.8ms "
                "lowSourceBypass=0 modeMismatch=0 sourceBacktrack=0\n"
            ),
        )
        policy_fault_report = classify_session_triage(wgc_sync_delay_policy_fault)
        assert "wgc_av_sync_delay_unrealized" not in policy_fault_report["verdicts"]
        assert "wgc_sync_delay_policy_fault" in policy_fault_report["verdicts"]
        assert "ce_visual_timeline_fault" in policy_fault_report["verdicts"]

        source_starved_with_overload = make_session(
            "source_starved_with_overload",
            media=(
                "[WGC CFR] Source-starved episode: duration=240ms out=29 dup=29 minIn=0 minDel=120 "
                "freshMiss=1000pm minBuf=0\n"
                "[Cadence Health] Phase=Live | WgcSelBias=260000us | Shortfall=32/266.7ms "
                "LeadExcess=116.0ms | Oldest=250.0ms BufNow=0 | WgcLiveRebase=1/120/1 | Over=0x1\n"
                "[EncoderThread] WGC CFR slot repeat: buffered frame is too new for scheduled slot "
                "(lead=28000us targetQpc=1000 firstQpc=1280 buffered=0 shortfall=32)\n"
            ),
        )
        report = classify_session_triage(source_starved_with_overload)
        assert "wgc_source_starvation" in report["verdicts"]
        assert "wgc_encoder_limited_judder" not in report["verdicts"]
