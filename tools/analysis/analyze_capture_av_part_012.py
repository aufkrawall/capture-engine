

def self_test():
    complete_coverage = summarize_cfr_packet_coverage([0.0, 1 / 120, 2 / 120], [1 / 120] * 3, 120.0)
    assert complete_coverage["complete"]
    assert complete_coverage["expected_packets"] == 3
    sparse_coverage = summarize_cfr_packet_coverage([0.0, 1 / 120, 6 / 120], [1 / 120] * 3, 120.0)
    assert not sparse_coverage["complete"]
    assert sparse_coverage["expected_packets"] == 7
    assert sparse_coverage["missing_packets"] == 4
    assert math.isclose(sparse_coverage["max_gap_ticks"], 5.0, abs_tol=1e-9)
    assert sparse_coverage["packet_count"] == 3
    assert Fraction(sparse_coverage["expected_packets"], 120) * 48000 == 2800

    manifest_warning = (
        "(process:42): GLib-GIO-WARNING **: Failed to open application manifest "
        "`C:\\Windows\\SystemApps\\Example\\AppxManifest.xml' for package #1: error code 0x2"
    )
    actionable_stderr, ignored_stderr = split_decoder_stderr(manifest_warning)
    assert actionable_stderr == ""
    assert ignored_stderr == manifest_warning
    actionable_stderr, ignored_stderr = split_decoder_stderr(manifest_warning + "\n[aac] invalid data")
    assert actionable_stderr == "[aac] invalid data"
    assert ignored_stderr == manifest_warning

    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)

        def make_session(name, media="", hook="", perf="", capture_method="wgc"):
            session = root / name
            session.mkdir()
            (session / "session_manifest.txt").write_text(
                f"build_version=test\ncapture_method={capture_method}\nnotes=test fixture\n", encoding="utf-8"
            )
            (session / "media.log").write_text(media, encoding="utf-8")
            if hook:
                (session / "hook_debug.log").write_text(hook, encoding="utf-8")
            if perf:
                (session / "perf_metrics_1.csv").write_text(perf, encoding="utf-8")
            return session

        finite_wgc_producer = make_session(
            "finite_wgc_producer",
            media=(
                "[Cadence Health] Live=120 Unique=72 Dup=48 WgcThr=120 Adj=1 SrcFps=72.00\n"
                "[WGC CFR] Producer contract: backend=wgc outputFps=120 producerTargetFps=120 "
                "minUpdateInterval100ns=83333 policy=finite localThrottleFps=0\n"
            ),
        )
        report = classify_session_triage(finite_wgc_producer)
        assert report["evidence"]["log_counts"]["wgc_cfr_producer_contract_fault"] == 2
        assert "wgc_producer_rate_contract_fault" in report["verdicts"]
        assert "ce_visual_timeline_fault" in report["verdicts"]

        app_active_no_data = make_session(
            "app_active_no_data",
            media=(
                "[AppAudioCapture] Capture mode contract: selected=polling preference=polling-first "
                "eventFallbackAllowed=1\n"
                "[STOP AUDIO] Source 1 (app-active-no-data): track=2 process=brave.exe\n"
            ),
        )
        report = classify_session_triage(app_active_no_data)
        assert "app_audio_active_no_data" in report["verdicts"]
        assert report["evidence"]["log_counts"]["audio_app_capture_mode_contract"] == 1

        source_gap = make_session(
            "source_gap",
            media=(
                "[VideoEncoder] Final metadata durations: target=1000 us video=1000 us "
                "audioMin=1000 us audioMax=1000 us maxDelta=0 us streams(v=1 a=2) "
                "overload(encoder=0 mux=0) backpressure=0 peakMux=0KB peakPkts=0\n"
            ),
            hook="DetourPresent: heartbeat #3514 gap=299ms presentOwner=0x0000 depth=0 slFG=0 tid=0x1234\n",
            perf=(
                "frame,qpc_us,total_us,capture_us,present_call_us,mux_queue_kb,overload_flags,api\n"
                "1,1000000,100,20,40,0,0,DX12\n2,1299402,196,64,50,0,0,DX12\n"
            ),
        )
        report = classify_session_triage(source_gap)
        assert "source_present_gap" in report["verdicts"]
        assert "ce_encoder_or_mux_backpressure" not in report["verdicts"]

        pre_live_gap = make_session(
            "pre_live_gap",
            media=(
                "[2026-07-17 16:52:14.000] [INFO] [EncoderThread] Recording live (inject)\n"
                "[2026-07-17 16:52:16.000] [INFO] [Media] Stopping recording...\n"
            ),
            perf=(
                "frame,qpc_us,total_us,capture_us,present_call_us,mux_queue_kb,overload_flags,api,"
                "source_frame_index,qpc_delta_us\n"
                "1,1000000,100,20,40,0,0,DX12,0,0\n"
                "2,1500000,100,20,40,0,0,DX12,0,500000\n"
                "3,2000000,100,20,40,0,0,DX12,1,500000\n"
                "4,2010000,100,20,40,0,0,DX12,2,10000\n"
                "5,2020000,100,20,40,0,0,DX12,3,10000\n"
                "6,2520000,100,20,40,0,0,DX12,3,500000\n"
            ),
        )
        report = classify_session_triage(pre_live_gap)
        assert "source_present_gap" not in report["verdicts"]
        assert report["evidence"]["present_gap_source"] == "perf_live_source"
        assert report["evidence"]["max_present_gap_ms"] == 10.0

        live_source_gap = make_session(
            "live_source_gap",
            media="[2026-07-17 16:52:14.000] [INFO] [EncoderThread] Recording live (inject)\n",
            perf=(
                "frame,qpc_us,total_us,capture_us,present_call_us,mux_queue_kb,overload_flags,api,"
                "source_frame_index,qpc_delta_us\n"
                "1,1000000,100,20,40,0,0,DX12,0,0\n"
                "2,1500000,100,20,40,0,0,DX12,1,500000\n"
                "3,1510000,100,20,40,0,0,DX12,2,10000\n"
                "4,1710000,100,20,40,0,0,DX12,3,200000\n"
            ),
        )
        report = classify_session_triage(live_source_gap)
        assert "source_present_gap" in report["verdicts"]
        assert report["evidence"]["max_present_gap_ms"] == 200.0

        contention_attribution = make_session(
            "contention_attribution",
            media=(
                "[WGC CFR] Source-starved episode: duration=100ms out=12 dup=6 minIn=40 minDel=40 "
                "freshMiss=500pm minBuf=0\n"
                "[WGC Perf] Input: 100 | Queued: 90 | MinIn250/500: 80/90 | MinDel250/500: 80/90 | "
                "FreshMiss: 100pm | Backend: DxgiDuplication DupMissed: 7 | Overload: 0x1\n"
                "[Inject Contention SUMMARY] CaptureLock=1 CpuLease=2 GpuBusy=3 RingFull=4 EventSignals=50 "
                "PubToIngest=2000/25000us\n"
                "[VideoEncoder] Final metadata durations: target=1000 us video=1000 us audioMin=1000 us "
                "audioMax=1000 us maxDelta=0 us streams(v=1 a=1) overload(encoder=1 mux=0) backpressure=0\n"
            ),
        )
        contention_report = classify_session_triage(contention_attribution)
        for verdict in (
            "dxgi_dup_upstream_producer_starvation",
            "duplication_consumer_starvation",
            "capture_gpu_queue_starvation",
            "hardware_encoder_starvation",
            "media_cpu_starvation",
        ):
            assert verdict in contention_report["verdicts"]

        startup_contention = make_session(
            "startup_contention",
            media=(
                "[2026-07-17 15:54:31.784] [INFO] [EncoderThread] Recording live (inject)\n"
                "[2026-07-17 15:54:32.540] [INFO] [Inject Contention] CaptureLock=0 CpuLease=1 GpuBusy=0 "
                "RingFull=0 EventSignals=78 PubToIngest=16237/291415us\n"
                "[2026-07-17 15:54:34.541] [INFO] [Inject Contention] CaptureLock=0 CpuLease=1 GpuBusy=0 "
                "RingFull=0 EventSignals=309 PubToIngest=33/59us\n"
            ),
        )
        report = classify_session_triage(startup_contention)
        assert "media_cpu_starvation" not in report["verdicts"]
        assert "inject_startup_publication_backlog" in report["contexts"]
        assert report["evidence"]["inject_contention_context"]["startup_max_us"] == 291415
        assert report["evidence"]["inject_contention_context"]["settled_max_us"] == 59

        overlay_context = make_session(
            "overlay_context",
            media="[VideoEncoder] Final packet timeline: target=1000 us videoEnd=1000 us audioMinEnd=1000 us "
            "audioMaxEnd=1000 us maxPacketDelta=0 us streams(v=1 a=1) audioPastTarget=0\n",
            hook="[2026-07-17 15:54:31.000] Steam overlay module detected\n",
        )
        report = classify_session_triage(overlay_context)
        assert "external_overlay_context" not in report["verdicts"]
        assert "external_overlay_present" in report["contexts"]

        smooth_buffer_item = {}
        update_wgc_smoothness_item_from_line(
            smooth_buffer_item,
            "[WGC CFR SMOOTHNESS SUMMARY] smoothBuf=1 smoothTargetMs=250 smoothFrames=16/16/30 "
            "smoothDelay=133.3ms smoothPoolSlots=20 smoothVramMB=2025.0 smoothCapLimited=1 "
            "smoothReason=vram_cap_limited",
        )
        assert smooth_buffer_item["smoothness_buffer_reason"] == "vram_cap_limited"
        assert smooth_buffer_item["smoothness_buffer_vram_mb"] == 2025.0

        smooth_buffer_with_pool_item = {}
        update_wgc_smoothness_item_from_line(
            smooth_buffer_with_pool_item,
            "[WGC CFR SMOOTHNESS SUMMARY] smoothBuf=1 smoothTargetMs=250 smoothFrames=16/16/30 "
            "smoothDelay=133.3ms smoothPoolSlots=24 sourceFramePoolBuffers=8 budgetSurfaces=32 "
            "syncFrames=4 safetySlots=4 leasedMax=22 freeMin=2 poolSaturatedDrops=0 "
            "overwritePrevented=9 leaseMismatches=0 smoothVramMB=2025.0 smoothCapLimited=1 "
            "smoothReason=vram_cap_limited",
        )
        assert smooth_buffer_with_pool_item["pool_lifetime_evidence"] == 1
        assert smooth_buffer_with_pool_item["smoothness_source_frame_pool_buffers"] == 8
        assert smooth_buffer_with_pool_item["smoothness_buffer_pool_slots"] == 24
        assert smooth_buffer_with_pool_item["smoothness_budget_surfaces"] == 32
        assert smooth_buffer_with_pool_item["pool_lease_max"] == 22
        assert smooth_buffer_with_pool_item["pool_overwrite_prevented"] == 9

        # Smoothness FLOOR line (video-only / low-confidence path): parsed into the same item for
        # triage visibility. Additive only -- no failure gate depends on it.
        smooth_floor_item = {}
        update_wgc_smoothness_item_from_line(
            smooth_floor_item,
            "[WGC CFR SMOOTHNESS FLOOR] smoothFloorSource=auto smoothFloorConfigured=1 smoothFloorMs=0 "
            "smoothFloorRequestedUs=42000 smoothFloorDelayUs=42000 smoothFloorClampedBy=none "
            "smoothFloorRealizedTargetUs=42000 measuredDeliveryGapUs(avg/max)=9000/50000 "
            "measuredSourceJitterUs(avg/max)=800/1200 realizedDelay(min/avg/max)Us=38000/41000/44000 "
            "residualLateMaxUs=3000 avContentDelayActive=0",
        )
        assert smooth_floor_item["smooth_floor_source"] == "auto"
        assert smooth_floor_item["smooth_floor_delay_us"] == 42000
        assert smooth_floor_item["smooth_floor_clamped_by"] == "none"
        assert smooth_floor_item["smooth_floor_delivery_gap_max_us"] == 50000
        assert smooth_floor_item["smooth_floor_realized_min_us"] == 38000
        assert smooth_floor_item["smooth_floor_av_content_delay_active"] == 0

        # WGC SUMMARY: new longestContiguousDup field parses, and worstIn/worstDel stay correct after
        # the group renumbering. This is the true visible-freeze metric (episode metrics overstate it).
        new_summary = WGC_SUMMARY_RE.search(
            "[WGC CFR SUMMARY] Live=23460 Dup=3083 DupPct=13.1% NoFresh=179pm NoReserve=206pm Pacer=42 "
            "DupReason(src=3072 def=0 timer=11 drain=0) SourceLimitedRepeats=3072 StarvedEpisodes=1451 "
            "longest=4110ms longestDup=292 longestContiguousDup=115 (958ms) worstIn=4 worstDel=4"
        )
        assert new_summary is not None
        assert parse_int(new_summary.group(10)) == 4110  # episode ms (overstates freeze)
        assert parse_int(new_summary.group(11)) == 292  # episode dups (overstates freeze)
        assert parse_int(new_summary.group(12)) == 115  # true contiguous freeze frames
        assert parse_int(new_summary.group(13)) == 958  # true contiguous freeze ms
        assert parse_int(new_summary.group(14)) == 4  # worstIn still correct
        assert parse_int(new_summary.group(15)) == 4  # worstDel still correct
        # Backward-compat: older logs without the field still parse; contiguous defaults to 0.
        old_summary = WGC_SUMMARY_RE.search(
            "[WGC CFR SUMMARY] Live=1000 Dup=120 DupPct=12.0% NoFresh=120pm NoReserve=0pm "
            "DupReason(src=120 def=0 timer=0 drain=0) SourceLimitedRepeats=120 StarvedEpisodes=5 "
            "longest=400ms longestDup=40 worstIn=84 worstDel=84"
        )
        assert old_summary is not None
        assert parse_int(old_summary.group(12)) == 0  # absent -> 0
        assert parse_int(old_summary.group(14)) == 84  # worstIn still correct on old format
        assert parse_int(old_summary.group(15)) == 84

        wgc_pool_safe_drop = make_session(
            "wgc_pool_safe_drop",
            media=(
                "[WGC Perf] Input: 120 | Queued: 118 | DropFull: 0 | DropPace: 0 | DropThrottle: 0 | "
                "DropStale: 0 (DupTs=0 OOO=0) | DropCursor: 0 | DropPool: 2 | HostQ: 20 | EncQ: 22 | Dup: 2 | "
                "Late: 0 | SrcAvg: 8333us | JitAvg: 200us | JitMax: 900us | Src->Copy: 40/80us | Deliv: 118 | "
                "MinIn250/500: 118/119 | MinDel250/500: 118/119 | FreshMiss: 0pm | BufAvg: 900pm | BufMin: 4 | "
                "NoFresh: 0 | NoReserve: 0 | SchedSelAvg: 0us SchedSelBias: 0us | WgcSelAvg: 0us WgcSelBias: 0us | "
                "CbGap: 8333/9000us CbProc: 50/90us CbDrainMax: 0 | Copy: 50us | "
                "SlotAge: 8200us FastSlot: 0 | PoolLease: max=24 freeMin=0 satDrop=2 overwritePrevented=24 "
                "mismatch=0 sourceFramePoolBuffers=8 copyPoolSlots=24 budgetSurfaces=32 syncFrames=4 "
                "extraFrames=16 safetySlots=4 | KMFail: 0/0 | Flush: 0/0 | Dedicated: 1 | Encode: 300us | "
                "Fence: 0us | Throttle: 120 | Mux: 0KB | Overload: 0x0\n"
                "[WGC CFR SMOOTHNESS SUMMARY] encoderLimitedDrops=0 maxDropTicks=0 cadenceEvents=0 "
                "phaseErrorMax=0us shortfallMax=0.0ms staleDebtDrops=0 liveRebase=0/0 tooNewRepeats=0 "
                "syncDelayHolds=0 tooNewLeadMax=0us avDelay=33.3ms startupDelay=166.6ms "
                "scheduleOffset=0us effectiveDelay=166.6ms lowSourceBypass=0 modeMismatch=0 "
                "sourceBacktrack=0 syncDelaySourceLimitedHolds=0 syncDelayPolicyHolds=0 "
                "startupReserveFrames=24 startupReserveSpan=166000us startupDelayTarget=166600us "
                "startupReserveSelected=1 startupReserveReason=selected smoothBuf=1 smoothTargetMs=250 "
                "smoothFrames=16/16/30 smoothDelay=133.3ms smoothPoolSlots=24 sourceFramePoolBuffers=8 "
                "budgetSurfaces=32 syncFrames=4 safetySlots=4 leasedMax=24 freeMin=0 "
                "poolSaturatedDrops=2 overwritePrevented=24 leaseMismatches=0 smoothVramMB=2025.0 "
                "smoothCapLimited=1 smoothReason=vram_cap_limited\n"
                "[VideoEncoder] Final metadata durations: target=1000 us video=1000 us audioMin=1000 "
                "us audioMax=1000 us maxDelta=0 us streams(v=1 a=2) overload(encoder=0 mux=0) "
                "backpressure=0 peakMux=0KB peakPkts=0\n"
            ),
        )
        report = classify_session_triage(wgc_pool_safe_drop)
        assert "wgc_pool_saturated_safe_drop" in report["verdicts"]
        assert "wgc_copy_pool_pressure" in report["verdicts"]
        assert "wgc_pool_slot_lifetime_fault" not in report["verdicts"]
        assert "ce_encoder_or_mux_backpressure" not in report["verdicts"]
        assert "ce_visual_timeline_fault" in report["verdicts"]
        assert report["faults"]["wgc_pool_saturated_safe_drop"]
        assert not report["faults"]["wgc_pool_slot_lifetime_fault"]

        wgc_ingress_decimated = make_session(
            "wgc_ingress_decimated",
            media=(
                "[WGC Perf] Input: 144 | Queued: 120 | DropFull: 0 | DropPace: 0 | DropThrottle: 0 | "
                "DropStale: 0 (DupTs=0 OOO=0) | DropCursor: 0 | DropPool: 0 | DropIngress: 24 | "
                "HostQ: 18 | EncQ: 1 | Dup: 0 | Late: 0 | SrcAvg: 6944us | JitAvg: 100us | JitMax: 500us | "
                "Src->Copy: 40/80us | Deliv: 120 | MinIn250/500: 144/144 | MinDel250/500: 120/120 | "
                "FreshMiss: 0pm | BufAvg: 900pm | BufMin: 18 | NoFresh: 0 | NoReserve: 0 | "
                "SchedSelAvg: 0us SchedSelBias: 0us | WgcSelAvg: 0us WgcSelBias: 0us | "
                "CbGap: 6944/7100us CbProc: 50/90us CbDrainMax: 0 | Copy: 50us | "
                "SlotAge: 8200us FastSlot: 0 | PoolLease: max=20 freeMin=4 satDrop=0 overwritePrevented=0 "
                "mismatch=0 sourceFramePoolBuffers=8 copyPoolSlots=24 budgetSurfaces=32 syncFrames=4 "
                "extraFrames=13 retainedCap=18 reservedFree=6 safetySlots=4 | "
                "Ingress: accepted=120 decimated=24 retained=18/18 lowWater=17 reason=wgc_ingress_decimated | "
                "KMFail: 0/0 | Flush: 0/0 | Dedicated: 1 | Encode: 300us | Fence: 0us | Throttle: 0 | "
                "Mux: 0KB | Overload: 0x0\n"
                "[WGC CFR SMOOTHNESS SUMMARY] encoderLimitedDrops=0 maxDropTicks=0 cadenceEvents=0 "
                "phaseErrorMax=0us shortfallMax=0.0ms staleDebtDrops=0 liveRebase=0/0 tooNewRepeats=0 "
                "syncDelayHolds=0 tooNewLeadMax=0us avDelay=33.3ms startupDelay=141.6ms "
                "scheduleOffset=0us effectiveDelay=141.6ms lowSourceBypass=0 modeMismatch=0 "
                "sourceBacktrack=0 syncDelaySourceLimitedHolds=0 syncDelayPolicyHolds=0 "
                "startupReserveFrames=18 startupReserveSpan=141000us startupDelayTarget=141600us "
                "startupReserveSelected=1 startupReserveReason=selected smoothBuf=1 smoothTargetMs=250 "
                "smoothFrames=13/13/30 smoothDelay=108.3ms smoothPoolSlots=24 sourceFramePoolBuffers=8 "
                "budgetSurfaces=32 syncFrames=4 extraFrames=13 retainedCap=18 reservedFreeSlots=6 safetySlots=4 "
                "retainedCapTrim=0 ingressAccepted=120 ingressDecimated=24 ingressRetained=18/18 "
                "ingressLowWater=17 leasedMax=20 freeMin=4 poolSaturatedDrops=0 overwritePrevented=0 "
                "leaseMismatches=0 smoothVramMB=2025.0 smoothCapLimited=1 smoothReason=vram_cap_limited\n"
                "[WGC CFR SMOOTHNESS DELAY] delayReservoirLowWaterFrames=17 delayReservoirTargetFrames=18 "
                "delayReservoirLowWaterTicks=0 realizedDelayAvg=141600us realizedDelayMin=141000us "
                "realizedDelayMax=142000us delayResidualAvg=0/1000us delayResidualMax=2000us "
                "delayResidualP95=1000us delayResidualLateMax=1000us delayResidualEarlyMax=1000us "
                "rawResidualAvg=0/1000us rawResidualMax=2000us rawResidualP95=1000us "
                "rawResidualLateMax=1000us rawResidualEarlyMax=1000us predictedResidualAvg=0/1000us "
                "predictedResidualP95=1000us predictedResidualLateMax=1000us rawMinusPredictedAvg=0/0us "
                "rawMinusPredictedMax=0us\n"
                "[VideoEncoder] Final packet timeline: target=1000 us videoEnd=1000 us audioMinEnd=1000 us "
                "audioMaxEnd=1000 us maxPacketDelta=0 us audioPastTarget=0\n"
                "[VideoEncoder] Final metadata durations: target=1000 us video=1000 us audioMin=1000 "
                "us audioMax=1000 us maxDelta=0 us streams(v=1 a=2) overload(encoder=0 mux=0) "
                "backpressure=0 peakMux=0KB peakPkts=0\n"
            ),
        )
        report = classify_session_triage(wgc_ingress_decimated)
        assert "wgc_ingress_decimated" in report["verdicts"]
        assert "wgc_copy_pool_pressure" in report["verdicts"]
        assert "wgc_pool_saturated_safe_drop" not in report["verdicts"]
        assert "ce_encoder_or_mux_backpressure" not in report["verdicts"]
        assert not report["faults"]["visual_timeline"]

        wgc_094302_pool_pressure = make_session(
            "wgc_094302_pool_pressure",
            media=(
                "[WGC Perf] Input: 120 | Queued: 84 | DropFull: 0 | DropPace: 0 | DropThrottle: 0 | "
                "DropStale: 0 (DupTs=0 OOO=0) | DropCursor: 0 | DropPool: 36 | HostQ: 24 | EncQ: 1 | "
                "Dup: 65 | Late: 0 | SrcAvg: 8333us | JitAvg: 500us | JitMax: 1200us | Src->Copy: 40/80us | "
                "Deliv: 84 | MinIn250/500: 120/120 | MinDel250/500: 84/84 | FreshMiss: 0pm | BufAvg: 900pm | "
                "BufMin: 0 | NoFresh: 0 | NoReserve: 0 | SchedSelAvg: 0us SchedSelBias: 0us | "
                "WgcSelAvg: 0us WgcSelBias: 0us | CbGap: 8333/9000us CbProc: 50/90us CbDrainMax: 0 | "
                "Copy: 50us | SlotAge: 8200us FastSlot: 0 | PoolLease: max=24 freeMin=0 satDrop=36 "
                "overwritePrevented=4000 mismatch=0 sourceFramePoolBuffers=8 copyPoolSlots=24 budgetSurfaces=32 "
                "syncFrames=4 extraFrames=16 safetySlots=4 | KMFail: 0/0 | Flush: 0/0 | Dedicated: 1 | "
                "Encode: 300us | Fence: 0us | Throttle: 0 | Mux: 0KB | Overload: 0x0\n"
                "[WGC CFR SMOOTHNESS SUMMARY] encoderLimitedDrops=0 maxDropTicks=0 cadenceEvents=1 "
                "phaseErrorMax=0us shortfallMax=0.0ms staleDebtDrops=0 liveRebase=0/0 tooNewRepeats=0 "
                "syncDelayHolds=0 tooNewLeadMax=0us avDelay=31.2ms startupDelay=163.5ms "
                "scheduleOffset=102699us effectiveDelay=163.5ms lowSourceBypass=0 modeMismatch=0 "
                "sourceBacktrack=0 syncDelaySourceLimitedHolds=0 syncDelayPolicyHolds=0 "
                "startupReserveFrames=24 startupReserveSpan=163489us startupDelayTarget=163489us "
                "startupReserveSelected=1 startupReserveReason=selected smoothBuf=1 smoothTargetMs=250 "
                "smoothFrames=16/16/30 smoothDelay=132.3ms smoothPoolSlots=24 sourceFramePoolBuffers=8 "
                "budgetSurfaces=32 syncFrames=4 safetySlots=4 leasedMax=24 freeMin=0 poolSaturatedDrops=766 "
                "overwritePrevented=117761 leaseMismatches=0 smoothVramMB=2025.0 smoothCapLimited=1 "
                "smoothReason=vram_cap_limited\n"
                "[VideoEncoder] Final packet timeline: target=111108333 us videoEnd=111108333 us "
                "audioMinEnd=111108333 us audioMaxEnd=111108333 us maxPacketDelta=0 us audioPastTarget=0\n"
                "[VideoEncoder] Final metadata durations: target=111108333 us video=111108333 us "
                "audioMin=111108333 us audioMax=111108333 us maxDelta=0 us streams(v=1 a=2) "
                "overload(encoder=0 mux=0) backpressure=0 peakMux=0KB peakPkts=0\n"
                "[VideoEncoder] Post-mux audio duration mismatch maxDelta=21333\n"
                "[VideoEncoder] Post-mux audio codec priming evidence (target=111108333 audioMinEnd=111129666 "
                "audioMaxEnd=111129666 maxDelta=21333 primingTolerance=21355 roundingTolerance=21)\n"
            ),
        )
        report = classify_session_triage(wgc_094302_pool_pressure)
        assert "wgc_pool_saturated_safe_drop" in report["verdicts"]
        assert "wgc_copy_pool_pressure" in report["verdicts"]
        assert "ce_encoder_or_mux_backpressure" not in report["verdicts"]
        assert "ce_audio_timeline_fault" in report["verdicts"]
        assert not report["evidence"]["rounding_evidence"]["post_mux_one_us_or_less_is_info"]

        wgc_quality_summary = make_session(
            "wgc_quality_summary",
            media=(
                "[WGC CFR QUALITY] duplicatePct=19.4 duplicates=1399/7203 worst1sUnique=71 "
                "worst1sRepeats=49 worst1sEmit=120 limiter=wgc_pool_pressure sourceLimitedRepeats=1399 "
                "poolPressure=1 freeMin=0 poolSaturatedDrops=6 ingressHard=103 ingressSoft=500 "
                "overwritePrevented=136402 dupTsSeen=492 dupTsSkipped=489 encoderOverload=0x0 "
                "muxBackpressure=0 compactRetained=1 sourceFmt=10 retainedFmt=24 convertUs=620 "
                "finalAvSync=exported_tracks_authoritative\n"
                "[VideoEncoder] Final packet timeline: target=60000000 us videoEnd=60000000 us "
                "audioMinEnd=60000000 us audioMaxEnd=60000000 us maxPacketDelta=0 us "
                "streams(v=1 a=3) audioPastTarget=0\n"
                "[VideoEncoder] Final metadata durations: target=60000000 us video=60000000 us "
                "audioMin=60000000 us audioMax=60000000 us maxDelta=0 us streams(v=1 a=3) "
                "overload(encoder=0 mux=0) backpressure=0\n"
            ),
        )
        report = classify_session_triage(wgc_quality_summary)
        quality = report["evidence"]["wgc_quality"][0]
        assert quality["limiter"] == "wgc_pool_pressure"
        assert quality["worst_1s_unique"] == 71
        assert quality["worst_1s_repeats"] == 49
        assert quality["compact_retained"] == 1
        assert quality["source_format"] == 10
        assert quality["retained_format"] == 24
        assert quality["duplicate_timestamps_seen"] == 492
        assert quality["duplicate_timestamps_skipped"] == 489
        assert report["evidence"]["exported_av_sync_ok"]
        assert "ce_audio_timeline_fault" not in report["verdicts"]

        wgc_source_coverage_best_effort = make_session(
            "wgc_source_coverage_best_effort",
            media=(
                "[WGC CFR SOURCE COVERAGE] coverage=limited reason=delivery_holes bestEffort=1 "
                "outputFps=120 duplicates=2976/16409 sourceLimitedRepeats=2976 "
                "sourceRepeatLowerBound=2976 syncSourceRepeatLowerBound=0 deliveryRepeatLowerBound=2976 "
                "excessRepeats=0 policyAddedRepeats=0 policyNoSourceRepeats=0 cleanEncoderMux=1 "
                "cleanPool=1 cleanSelection=1 encoderOverload=0x0 muxBackpressure=0 poolPressure=0 "
                "poolFreeMin=51 finalAvSync=exported_tracks_authoritative "
                "note=surplus_source_frames_are_dropped_when_available_repeats_mean_cfr_coverage_holes\n"
                "[VideoEncoder] Final metadata durations: target=1000 us video=1000 us audioMin=1000 "
                "us audioMax=1000 us maxDelta=0 us streams(v=1 a=2) overload(encoder=0 mux=0) "
                "backpressure=0 peakMux=0KB peakPkts=0\n"
            ),
        )
        report = classify_session_triage(wgc_source_coverage_best_effort)
        coverage = report["evidence"]["wgc_source_coverage"][0]
        assert coverage["coverage"] == "limited"
        assert coverage["reason"] == "delivery_holes"
        assert coverage["best_effort"] == 1
        assert coverage["source_repeat_lower_bound"] == 2976
        assert coverage["delivery_repeat_lower_bound"] == 2976
        assert coverage["excess_repeats"] == 0
        assert "wgc_source_coverage_best_effort" in report["verdicts"]
        assert "ce_audio_timeline_fault" not in report["verdicts"]
        assert "ce_visual_timeline_fault" not in report["verdicts"]

        dxgi_variable_fps_source_limited = make_session(
            "dxgi_variable_fps_source_limited",
            capture_method="dxgi_dup",
            media=(
                "[WGC Perf] Input: 15035 | Queued: 15035 | MinIn250/500: 12/48 | "
                "MinDel250/500: 12/48 | FreshMiss: 208pm | PoolLease: max=42 freeMin=22 "
                "satDrop=0 overwritePrevented=0 mismatch=0 | Backend: DxgiDuplication DupMissed: 22 | "
                "Overload: 0x0\n"
                "[WGC CFR SUMMARY] Live=12728 Dup=85 DupPct=0.6% NoFresh=6pm NoReserve=10pm "
                "DupReason(src=85 def=0 timer=0 drain=0) SourceLimitedRepeats=85 StarvedEpisodes=281 "
                "AntiFreezeFloor=0 AntiFreezeFloorSkippedSync=14 BiasClampCount=0 "
                "longest=938ms longestDup=10 longestContiguousDup=24 (200ms) worstIn=56 worstDel=56\n"
                "[WGC CFR SMOOTHNESS SUMMARY] encoderLimitedDrops=0 maxDropTicks=0 cadenceEvents=10 "
                "phaseErrorMax=54170us shortfallMax=0.0ms staleDebtDrops=143 liveRebase=0/0 "
                "tooNewRepeats=37 syncDelayHolds=37 tooNewLeadMax=0us avDelay=28.7ms "
                "startupDelay=48.6ms scheduleOffset=19908us effectiveDelay=48.6ms "
                "lowSourceBypass=0 modeMismatch=0 sourceBacktrack=0 "
                "syncDelaySourceLimitedHolds=37 syncDelayPolicyHolds=0 startupReserveFrames=40 "
                "startupReserveSpan=270894us startupDelayTarget=328710us startupReserveSelected=0 "
                "startupReserveReason=partial_span_timeout smoothBuf=1 smoothTargetMs=300 "
                "smoothFrames=2/45/45 smoothDelay=19.9ms smoothPoolSlots=64 sourceFramePoolBuffers=0 "
                "budgetSurfaces=94 syncFrames=5 extraFrames=45 retainedCap=58 reservedFreeSlots=6 "
                "safetySlots=4 retainedCapTrim=0 ingressAccepted=15035 ingressDecimated=0 "
                "ingressPlaySoft=0 ingressPlayCredit=0 ingressRetained=6/58 ingressLowWater=6 "
                "leasedMax=42 freeNow=64 freeMin=22 poolPressureTrim=0 poolSaturatedDrops=0 "
                "overwritePrevented=0 leaseMismatches=0 smoothVramMB=2025.0 smoothCapLimited=0 "
                "smoothReason=startup_attempt\n"
                "[WGC CFR SMOOTHNESS BUFFER] smoothTargetDelay=299998us smoothActualDelay=19908us "
                "smoothDelayDeficit=280090us startupDelayTarget=328710us effectiveDelay=48619us "
                "startupDelayDeficit=280091us finalAvSync=exported_tracks_authoritative\n"
                "[WGC CFR SMOOTHNESS DELAY] delayReservoirLowWaterFrames=6 delayReservoirTargetFrames=7 "
                "delayReservoirLowWaterTicks=130 realizedDelayAvg=48601us realizedDelayMin=38874us "
                "realizedDelayMax=53330us delayResidualAvg=17/1793us delayResidualMax=9745us "
                "delayResidualP95=3000us delayResidualLateMax=9745us delayResidualEarlyMax=4711us "
                "rawResidualAvg=-2836/3799us rawResidualMax=10710us rawResidualP95=7000us "
                "rawResidualLateMax=10710us rawResidualEarlyMax=9800us predictedResidualAvg=17/1793us "
                "predictedResidualP95=3000us predictedResidualLateMax=9745us "
                "rawMinusPredictedAvg=-2854/2854us rawMinusPredictedMax=6250us\n"
                "[WGC CFR SMOOTHNESS REPEAT] delaySoftLateAccepted=0 delayRepeatSoftSafeCandidate=0 "
                "delaySyncProtectedRepeats=37 delayPostSelectionRejectedSync=0\n"
                "[WGC CFR SMOOTHNESS VERDICT] delayPostSelectionRejectedSync=0 "
                "delayPostSelectionRescuedSync=0 sourceRepeatLowerBound=85 excessRepeats=0 "
                "policyAddedRepeats=0 excessRepeatClusters=0 excessRepeatClusterMax=0 "
                "smoothnessNotMaximal=0 mixedPolicyFault=0 syncSourceRepeatLowerBound=37 "
                "deliveryRepeatLowerBound=85 policyNoSourceRepeats=0\n"
                "[WGC CFR QUALITY] duplicatePct=0.6 duplicates=85/12728 worst1sUnique=95 "
                "worst1sRepeats=25 worst1sEmit=120 limiter=source_limited sourceLimitedRepeats=85 "
                "poolPressure=0 freeMin=22 poolSaturatedDrops=0 ingressHard=0 ingressSoft=0 "
                "ingressDecimated=0 policyAddedRepeats=0 excessRepeats=0 "
                "smoothDelayDeficitUs=280090 startupDelayDeficitUs=280091 encoderOverload=0x0 "
                "muxBackpressure=0 backend=dxgi_dup finalAvSync=exported_tracks_authoritative\n"
                "[WGC CFR SOURCE COVERAGE] coverage=limited reason=source_and_delivery_holes bestEffort=1 "
                "outputFps=120 duplicates=85/12728 sourceLimitedRepeats=85 sourceRepeatLowerBound=85 "
                "syncSourceRepeatLowerBound=37 deliveryRepeatLowerBound=85 excessRepeats=0 "
                "policyAddedRepeats=0 policyNoSourceRepeats=0 cleanEncoderMux=1 cleanPool=1 "
                "cleanSelection=1 encoderOverload=0x0 muxBackpressure=0 poolPressure=0 poolFreeMin=22 "
                "finalAvSync=exported_tracks_authoritative\n"
                "[VideoEncoder] Final packet timeline: target=106075000 us videoEnd=106075000 us "
                "audioMinEnd=106074999 us audioMaxEnd=106074999 us maxPacketDelta=1 us "
                "streams(v=1 a=3) audioPastTarget=0\n"
                "[VideoEncoder] Final metadata durations: target=106075000 us video=106075000 us "
                "audioMin=106074999 us audioMax=106074999 us maxDelta=1 us streams(v=1 a=3) "
                "overload(encoder=0 mux=0) backpressure=0\n"
            ),
            hook="DetourPresent: heartbeat #1 gap=757ms presentOwner=0x0 depth=0 slFG=0 tid=0x1\n",
            perf=(
                "frame,qpc_us,total_us,capture_us,present_call_us,mux_queue_kb,overload_flags,api,"
                "source_frame_index,capture_phase,qpc_delta_us\n"
                "1,1000000,100,0,0,0,0,DX12,0,0,0\n"
                "2,1757000,100,0,0,0,0,DX12,0,0,757000\n"
                "3,1765000,100,0,0,0,0,DX12,0,1,8000\n"
                "4,1993852,100,0,0,0,0,DX12,0,2,228852\n"
                "5,2002185,100,0,0,0,0,DX12,0,2,8333\n"
            ),
        )
        report = classify_session_triage(dxgi_variable_fps_source_limited)
        assert report["evidence"]["screen_capture_backend"] == "dxgi_dup"
        assert report["evidence"]["present_gap_source"] == "perf_live_source"
        assert report["evidence"]["max_present_gap_ms"] == 228.852
        assert report["evidence"]["present_gap_filter_kind"] == "capture_phase"
        assert "dxgi_dup_source_starvation" in report["verdicts"]
        assert "dxgi_dup_source_coverage_best_effort" in report["verdicts"]
        assert not any(verdict.startswith("wgc_") for verdict in report["verdicts"])
        assert "duplication_consumer_starvation" not in report["verdicts"]
        assert "dxgi_dup_delivery_gap" in report["contexts"]
        assert "dxgi_dup_startup_reservoir_partial" in report["contexts"]
        assert "dxgi_dup_source_limited_delay_variation" in report["contexts"]
        assert "dxgi_dup_av_sync_delay_residual" not in report["verdicts"]
        assert "dxgi_dup_timestamp_domain_mismatch" not in report["verdicts"]
        assert "dxgi_dup_active_delay_realized_delay_unstable" not in report["verdicts"]
        assert "ce_visual_timeline_fault" not in report["verdicts"]
        assert not report["faults"]["visual_timeline"]
        assert report["faults"]["wgc_clean_source_limited_coverage"]
        assert report["faults"]["wgc_source_limited_delay_variation_context"]

        assert has_wgc_framepool_pressure_attribution(
            parse_media_triage("[WGC CFR ATTRIBUTION] fault_hint=wgc_framepool_pressure poolSat=0 "
                               "overwritePrevented=1 ingressDecimated=0")
        )
        legacy_media = read_text_if_exists(dxgi_variable_fps_source_limited / "media.log") + (
            "\n[CFR PhaseLock] backend=wgc state=acquired offset=10us stable=8 unstable=0 multiplier=1 "
            "transitions=1/0/0\n"
            "[CFR PHASE LOCK SUMMARY] Backend=wgc Enabled=1 Locked=1 Offset=10us Stable=8 "
            "Unstable=0 Acquire=1 Rephase=0 Release=0 Multiplier=1\n"
            "[WGC CFR ATTRIBUTION] fault_hint=wgc_framepool_pressure poolSat=0 "
            "overwritePrevented=1659 ingressDecimated=0\n"
        )
        legacy_report = classify_session_triage(make_session("legacy_phase", legacy_media, capture_method="dxgi_dup"))
        assert legacy_report["evidence"]["screen_capture_backend_history"] == ["dxgi_dup"]
        assert "screen_capture_backend_transition" not in legacy_report["contexts"]
        assert "dxgi_dup_pool_lease_contention" in legacy_report["contexts"]
        assert "dxgi_dup_framepool_pressure" not in legacy_report["verdicts"]
        assert not legacy_report["faults"]["visual_timeline"]
        assert legacy_report["evidence"]["cfr_phase_lock_summary"][0]["offset_us"] == 10

        backend_transition_media = read_text_if_exists(dxgi_variable_fps_source_limited / "media.log")
        backend_transition_media = backend_transition_media.replace(
            "\n[WGC CFR SUMMARY]",
            "\n[WGC Perf] Input: 120 | Queued: 120 | MinIn250/500: 120/120 | "
            "MinDel250/500: 120/120 | FreshMiss: 0pm | PoolLease: max=16 freeMin=48 "
            "satDrop=0 overwritePrevented=0 mismatch=0 | Backend: WGC DupMissed: 0 | "
            "Overload: 0x0\n[WGC CFR SUMMARY]",
            1,
        )
        for old, new in (
            ("SourceLimitedRepeats=85", "SourceLimitedRepeats=77"),
            ("sourceLimitedRepeats=85", "sourceLimitedRepeats=77"),
            ("sourceRepeatLowerBound=85", "sourceRepeatLowerBound=77"),
            ("Live=12728", "Live=3751"),
            ("duplicates=85/12728", "duplicates=85/3751"),
            ("excessRepeats=0", "excessRepeats=8"),
            ("bestEffort=1", "bestEffort=0"),
            ("cleanSelection=1", "cleanSelection=0"),
            ("worstIn=56 worstDel=56", "worstIn=8 worstDel=8"),
            ("realizedDelayMin=38874us realizedDelayMax=53330us", "realizedDelayMin=0us realizedDelayMax=110000us"),
            ("delayResidualMax=9745us", "delayResidualMax=118694us"),
            ("delayResidualLateMax=9745us", "delayResidualLateMax=118694us"),
            ("rawResidualMax=10710us", "rawResidualMax=118694us"),
            ("rawResidualLateMax=10710us", "rawResidualLateMax=118694us"),
            ("predictedResidualLateMax=9745us", "predictedResidualLateMax=118694us"),
            ("backend=dxgi_dup", "backend=wgc"),
        ):
            backend_transition_media = backend_transition_media.replace(old, new)
        backend_transition = make_session(
            "backend_transition_source_limited",
            media=backend_transition_media,
            capture_method="auto",
        )
        transition_report = classify_session_triage(backend_transition)
        assert transition_report["evidence"]["screen_capture_backend"] == "dxgi_dup_to_wgc"
        assert transition_report["evidence"]["screen_capture_backend_history"] == ["dxgi_dup", "wgc"]
        assert "screen_capture_backend_transition" in transition_report["contexts"]
        assert "screen_capture_source_starvation" in transition_report["verdicts"]
        assert "screen_capture_startup_reservoir_partial" in transition_report["contexts"]
        assert "screen_capture_source_limited_delay_variation" in transition_report["contexts"]
        assert "screen_capture_av_sync_delay_residual" not in transition_report["verdicts"]
        assert "screen_capture_audio_late_risk" not in transition_report["verdicts"]
        assert "screen_capture_active_delay_realized_delay_unstable" not in transition_report["verdicts"]
        assert "ce_visual_timeline_fault" not in transition_report["verdicts"]
        assert transition_report["faults"]["wgc_source_limited_playout_maximal"]
        assert transition_report["faults"]["wgc_backend_transition_source_limited_playout_maximal"]

        single_backend_uncertain_source = make_session(
            "single_backend_uncertain_source",
            media=backend_transition_media.replace(
                "Backend: DxgiDuplication", "Backend: WGC"
            ),
            capture_method="auto",
        )
        single_backend_report = classify_session_triage(single_backend_uncertain_source)
        assert single_backend_report["evidence"]["screen_capture_backend"] == "wgc"
        assert single_backend_report["evidence"]["screen_capture_backend_history"] == ["wgc"]
        assert not single_backend_report["faults"][
            "wgc_backend_transition_source_limited_playout_maximal"
        ]
        assert "wgc_av_sync_delay_residual" in single_backend_report["verdicts"]
        assert "wgc_active_delay_realized_delay_unstable" in single_backend_report["verdicts"]
        assert "ce_visual_timeline_fault" in single_backend_report["verdicts"]
