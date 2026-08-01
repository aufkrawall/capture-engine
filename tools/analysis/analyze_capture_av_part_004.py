

def parse_wgc_perf_line(line):
    def find_int(pattern, default=0):
        match = re.search(pattern, line)
        return parse_int(match.group(1), default) if match else default

    cb_gap_match = re.search(r"CbGap:\s*(-?\d+)/(-?\d+)us", line)
    min_in_match = re.search(r"MinIn250/500:\s*(\d+)/(\d+)", line)
    min_del_match = re.search(r"MinDel250/500:\s*(\d+)/(\d+)", line)
    km_fail_match = re.search(r"KMFail:\s*(\d+)/(\d+)", line)
    flush_match = re.search(r"Flush:\s*(\d+)/(\d+)", line)
    return {
        "pool_lease_evidence": bool(re.search(r"PoolLease:", line)),
        "input": find_int(r"Input:\s*(\d+)"),
        "queued": find_int(r"Queued:\s*(\d+)"),
        "drop_ingress": find_int(r"DropIngress:\s*(\d+)"),
        "duplicate_timestamps_seen": find_int(r"SrcDupTs:\s*seen=(\d+)"),
        "duplicate_timestamps_skipped": find_int(r"SrcDupTs:\s*seen=\d+\s*skip=(\d+)"),
        "duplicate": find_int(r"Dup:\s*(\d+)"),
        "late": find_int(r"Late:\s*(\d+)"),
        "min_in_250": parse_int(min_in_match.group(1)) if min_in_match else 0,
        "min_in_500": parse_int(min_in_match.group(2)) if min_in_match else 0,
        "min_del_250": parse_int(min_del_match.group(1)) if min_del_match else 0,
        "min_del_500": parse_int(min_del_match.group(2)) if min_del_match else 0,
        "fresh_miss_pm": find_int(r"FreshMiss:\s*(\d+)pm"),
        "buf_min": find_int(r"BufMin:\s*(\d+)"),
        "no_fresh": find_int(r"NoFresh:\s*(\d+)"),
        "cb_gap_avg_us": parse_int(cb_gap_match.group(1)) if cb_gap_match else 0,
        "cb_gap_max_us": parse_int(cb_gap_match.group(2)) if cb_gap_match else 0,
        "copy_us": find_int(r"Copy:\s*(-?\d+)us"),
        "fence_us": find_int(r"Fence:\s*(-?\d+)us"),
        "mux_kb": find_int(r"Mux:\s*(\d+)KB"),
        "overload_flags": int(re.search(r"Overload:\s*0x([0-9A-Fa-f]+)", line).group(1), 16)
        if re.search(r"Overload:\s*0x([0-9A-Fa-f]+)", line)
        else 0,
        "keyed_acquire_fail": parse_int(km_fail_match.group(1)) if km_fail_match else 0,
        "keyed_release_fail": parse_int(km_fail_match.group(2)) if km_fail_match else 0,
        "flush_count": parse_int(flush_match.group(1)) if flush_match else 0,
        "flush_skipped": parse_int(flush_match.group(2)) if flush_match else 0,
        "pool_lease_max": find_int(r"PoolLease:\s*max=(\d+)"),
        "pool_free_min": find_int(r"freeMin=(\d+)"),
        "pool_saturated_drops": find_int(r"satDrop=(\d+)"),
        "pool_overwrite_prevented": find_int(r"overwritePrevented=(\d+)"),
        "pool_lease_mismatch": find_int(r"mismatch=(\d+)"),
        "source_frame_pool_buffers": find_int(r"sourceFramePoolBuffers=(\d+)"),
        "copy_pool_slots": find_int(r"copyPoolSlots=(\d+)"),
        "budget_surfaces": find_int(r"budgetSurfaces=(\d+)"),
        "sync_frames": find_int(r"syncFrames=(\d+)"),
        "extra_frames": find_int(r"extraFrames=(\d+)"),
        "retained_cap": find_int(r"retainedCap=(\d+)"),
        "reserved_free_slots": find_int(r"reservedFree=(\d+)"),
        "safety_slots": find_int(r"safetySlots=(\d+)"),
        "source_format": find_int(r"sourceFmt=(\d+)"),
        "copy_format": find_int(r"copyFmt=(\d+)"),
        "compact_retained": find_int(r"compactRetained=(\d+)"),
        "source_budget_mb": parse_named_float_field(line, "sourceBudgetMB", 0.0),
        "copy_budget_mb": parse_named_float_field(line, "copyBudgetMB", 0.0),
        "source_surface_mb": parse_named_float_field(line, "sourceSurfaceMB", 0.0),
        "copy_surface_mb": parse_named_float_field(line, "copySurfaceMB", 0.0),
        "convert_us": parse_named_int_field(line, "convertUs", 0),
        "ingress_accepted": find_int(r"Ingress:\s*accepted=(\d+)"),
        "ingress_decimated": find_int(r"decimated=(\d+)"),
        "ingress_retained": find_int(r"retained=(\d+)/\d+"),
        "ingress_retained_cap": find_int(r"retained=\d+/(\d+)"),
        "ingress_low_water": find_int(r"lowWater=(\d+)"),
        "ingress_accepted_low_water": find_int(r"accLow=(\d+)"),
        "ingress_accepted_recovery": find_int(r"accRec=(\d+)"),
        "ingress_accepted_source_below": find_int(r"accSrcBelow=(\d+)"),
        "ingress_accepted_healthy": find_int(r"accHealthy=(\d+)"),
        "ingress_accepted_playout_soft": find_int(r"accPlaySoft=(\d+)"),
        "ingress_accepted_playout_credit": find_int(r"accPlayCredit=(\d+)"),
        "ingress_decimated_soft_reserve": find_int(r"decSoft=(\d+)"),
        "ingress_decimated_hard_reserve": find_int(r"decHard=(\d+)"),
        "ingress_decimated_credit": find_int(r"decCredit=(\d+)"),
        "ingress_soft_reserve_pressure": find_int(r"softPress=(\d+)"),
        "ingress_hard_reserve_pressure": find_int(r"hardPress=(\d+)"),
        "ingress_reason": (re.search(r"reason=([A-Za-z0-9_-]+)", line).group(1)
                           if re.search(r"reason=([A-Za-z0-9_-]+)", line)
                           else ""),
        "backend": (re.search(r"Backend:\s*([A-Za-z0-9_-]+)", line).group(1)
                    if re.search(r"Backend:\s*([A-Za-z0-9_-]+)", line)
                    else ""),
        "dup_missed": find_int(r"DupMissed:\s*(\d+)"),
    }


def parse_wgc_quality_line(line):
    payload_match = WGC_QUALITY_RE.search(line)
    payload = payload_match.group(1) if payload_match else line
    values = parse_attribution_payload(payload)
    duplicate_counts = values.get("duplicates", "0/0").split("/", 1)
    return {
        "duplicate_pct": parse_float(values.get("duplicatePct"), 0.0),
        "duplicates": parse_int(duplicate_counts[0] if duplicate_counts else 0),
        "live": parse_int(duplicate_counts[1] if len(duplicate_counts) > 1 else 0),
        "worst_1s_unique": parse_int(values.get("worst1sUnique"), 0),
        "worst_1s_repeats": parse_int(values.get("worst1sRepeats"), 0),
        "worst_1s_emit": parse_int(values.get("worst1sEmit"), 0),
        "limiter": values.get("limiter", ""),
        "source_limited_repeats": parse_int(values.get("sourceLimitedRepeats"), 0),
        "pool_pressure": parse_int(values.get("poolPressure"), 0),
        "pool_free_evidence": "freeMin" in values,
        "free_min": parse_int(values.get("freeMin"), 0),
        "pool_saturated_drops": parse_int(values.get("poolSaturatedDrops"), 0),
        "ingress_hard": parse_int(values.get("ingressHard"), 0),
        "ingress_soft": parse_int(values.get("ingressSoft"), 0),
        "ingress_decimated": parse_int(values.get("ingressDecimated"), 0),
        "pool_pressure_trim": parse_int(values.get("poolPressureTrim"), 0),
        "ingress_accepted_playout_soft": parse_int(values.get("ingressPlaySoft"), 0),
        "ingress_accepted_playout_credit": parse_int(values.get("ingressPlayCredit"), 0),
        "overwrite_prevented": parse_int(values.get("overwritePrevented"), 0),
        "sync_protected_repeats": parse_int(values.get("syncProtectedRepeats"), 0),
        "policy_added_repeats": parse_int(values.get("policyAddedRepeats"), 0),
        "excess_repeats": parse_int(values.get("excessRepeats"), 0),
        "smooth_delay_deficit_us": parse_int(values.get("smoothDelayDeficitUs"), 0),
        "startup_delay_deficit_us": parse_int(values.get("startupDelayDeficitUs"), 0),
        "duplicate_timestamps_seen": parse_int(values.get("dupTsSeen"), 0),
        "duplicate_timestamps_skipped": parse_int(values.get("dupTsSkipped"), 0),
        "encoder_overload": values.get("encoderOverload", "0x0"),
        "mux_backpressure": parse_int(values.get("muxBackpressure"), 0),
        "compact_retained": parse_int(values.get("compactRetained"), 0),
        "source_format": parse_int(values.get("sourceFmt"), 0),
        "retained_format": parse_int(values.get("retainedFmt"), 0),
        "convert_us": parse_int(values.get("convertUs"), 0),
        "backend": values.get("backend", ""),
        "final_av_sync": values.get("finalAvSync", ""),
        "line": line,
    }


def parse_wgc_source_coverage_line(line):
    payload_match = WGC_SOURCE_COVERAGE_RE.search(line)
    payload = payload_match.group(1) if payload_match else line
    values = parse_attribution_payload(payload)
    duplicate_counts = values.get("duplicates", "0/0").split("/", 1)
    return {
        "coverage": values.get("coverage", ""),
        "reason": values.get("reason", ""),
        "best_effort": parse_int(values.get("bestEffort"), 0),
        "output_fps": parse_int(values.get("outputFps"), 0),
        "duplicates": parse_int(duplicate_counts[0] if duplicate_counts else 0),
        "live": parse_int(duplicate_counts[1] if len(duplicate_counts) > 1 else 0),
        "source_limited_repeats": parse_int(values.get("sourceLimitedRepeats"), 0),
        "source_repeat_lower_bound": parse_int(values.get("sourceRepeatLowerBound"), 0),
        "sync_source_repeat_lower_bound": parse_int(values.get("syncSourceRepeatLowerBound"), 0),
        "delivery_repeat_lower_bound": parse_int(values.get("deliveryRepeatLowerBound"), 0),
        "excess_repeats": parse_int(values.get("excessRepeats"), 0),
        "policy_added_repeats": parse_int(values.get("policyAddedRepeats"), 0),
        "policy_no_source_repeats": parse_int(values.get("policyNoSourceRepeats"), 0),
        "clean_encoder_mux": parse_int(values.get("cleanEncoderMux"), 0),
        "clean_pool": parse_int(values.get("cleanPool"), 0),
        "clean_selection": parse_int(values.get("cleanSelection"), 0),
        "encoder_overload": values.get("encoderOverload", "0x0"),
        "mux_backpressure": parse_int(values.get("muxBackpressure"), 0),
        "pool_pressure": parse_int(values.get("poolPressure"), 0),
        "pool_free_min": parse_int(values.get("poolFreeMin"), 0),
        "final_av_sync": values.get("finalAvSync", ""),
        "note": values.get("note", ""),
        "line": line,
    }


def parse_recording_health_line(line):
    payload_match = RECORDING_HEALTH_RE.search(line)
    payload = payload_match.group(1) if payload_match else line
    values = parse_attribution_payload(payload)
    return {
        "status": values.get("status", "unknown"),
        "cause": values.get("cause", "none"),
        "flags": parse_base0_int(values.get("flags"), 0),
        "current_debt_ms": parse_int(values.get("currentDebtMs"), 0),
        "peak_debt_ms": parse_int(values.get("peakDebtMs"), 0),
        "capacity_attributed_debt_ms": parse_int(values.get("capacityDebtMs"), -1),
        "cfr": parse_int(values.get("cfr"), 0),
        "settings_changed": parse_int(values.get("settingsChanged"), 0),
        "line": line,
    }


def parse_inject_perf_line(line):
    def find_int(pattern, default=0):
        match = re.search(pattern, line)
        return parse_int(match.group(1), default) if match else default

    return {
        "input": find_int(r"Input:\s*(\d+)"),
        "queued": find_int(r"Queued:\s*(\d+)"),
        "drop_full": find_int(r"DropFull:\s*(\d+)"),
        "drop_pace": find_int(r"DropPace:\s*(\d+)"),
        "publication_fps": find_int(r"PubFps:\s*(\d+)"),
        "host_queue": find_int(r"HostQ:\s*(\d+)"),
        "encoder_queue": find_int(r"EncQ:\s*(\d+)"),
        "duplicate": find_int(r"Dup:\s*(\d+)"),
        "late": find_int(r"Late:\s*(\d+)"),
        "trim": find_int(r"Trim:\s*(\d+)"),
        "selection_drop": find_int(r"SelDrop:\s*(\d+)"),
        "deferred": find_int(r"Def:\s*(\d+)"),
        "encode_us": find_int(r"Encode:\s*(-?\d+)us"),
        "fence_us": find_int(r"Fence:\s*(-?\d+)us"),
        "mux_kb": find_int(r"Mux:\s*(\d+)KB"),
        "overload_flags": int(re.search(r"Overload:\s*0x([0-9A-Fa-f]+)", line).group(1), 16)
        if re.search(r"Overload:\s*0x([0-9A-Fa-f]+)", line)
        else 0,
        "line": line,
    }


def parse_attribution_payload(payload):
    values = {}
    for key, value in re.findall(r"([A-Za-z_][A-Za-z0-9_]*)=([^|\s]+)", payload):
        values[key] = value.rstrip(",")
    return values


def parse_recording_window_spec(spec):
    if not spec:
        return None
    match = re.fullmatch(r"\s*([0-9]+(?:\.[0-9]+)?)\s*:\s*([0-9]+(?:\.[0-9]+)?)\s*", spec)
    if not match:
        raise ValueError("recording window must use START:END seconds, for example 25:45")
    start_s = parse_float(match.group(1))
    end_s = parse_float(match.group(2))
    if end_s <= start_s:
        raise ValueError("recording window END must be greater than START")
    return start_s, end_s


def parse_live_start_qpc(media_text):
    match = WGC_LIVE_START_QPC_RE.search(media_text)
    return parse_int(match.group(1), 0) if match else 0


def parse_log_timestamp_us(line):
    match = LOG_LINE_TIMESTAMP_RE.match(line)
    if not match:
        return -1
    timestamp = match.group(1)
    try:
        parsed = datetime.datetime.strptime(timestamp, "%Y-%m-%d %H:%M:%S.%f")
    except ValueError:
        return -1
    epoch = datetime.datetime(parsed.year, parsed.month, parsed.day)
    return int(round((parsed - epoch).total_seconds() * 1000000.0))


def parse_live_start_wall_us(media_text):
    for line in media_text.splitlines():
        if "[A/V START] Shared startup anchor selected" in line or "[EncoderThread] Recording live" in line:
            timestamp_us = parse_log_timestamp_us(line)
            if timestamp_us >= 0:
                return timestamp_us
    return -1


def parse_live_start_qpc_wall_us(media_text):
    for line in media_text.splitlines():
        if "liveStartQpc=" in line:
            timestamp_us = parse_log_timestamp_us(line)
            if timestamp_us >= 0:
                return timestamp_us
    return parse_live_start_wall_us(media_text)


def parse_stop_start_wall_us(media_text):
    for line in media_text.splitlines():
        if "[Media] Stopping recording" in line:
            timestamp_us = parse_log_timestamp_us(line)
            if timestamp_us >= 0:
                return timestamp_us
    return -1


def choose_perf_qpc_us_from_live_start(live_start_qpc, perf_summaries):
    if live_start_qpc <= 0:
        return 0
    perf_min = min((item.get("min_qpc_us", 0) for item in perf_summaries if item.get("min_qpc_us", 0) > 0), default=0)
    perf_max = max((item.get("max_qpc_us", 0) for item in perf_summaries), default=0)
    if perf_min <= 0 or perf_max <= 0:
        return live_start_qpc

    best_value = live_start_qpc
    best_distance = None
    for divisor in (1, 10, 1000, 1000000):
        candidate = live_start_qpc // divisor
        if perf_min <= candidate <= perf_max:
            return candidate
        distance = min(abs(candidate - perf_min), abs(candidate - perf_max))
        if best_distance is None or distance < best_distance:
            best_distance = distance
            best_value = candidate
    return best_value


def build_recording_window_info(media_text, recording_window_spec, perf_summaries):
    parsed = parse_recording_window_spec(recording_window_spec)
    if not parsed:
        return None
    live_start_qpc = parse_live_start_qpc(media_text)
    live_start_qpc_us = choose_perf_qpc_us_from_live_start(live_start_qpc, perf_summaries)
    live_start_wall_us = parse_live_start_wall_us(media_text)
    start_s, end_s = parsed
    start_offset_us = int(round(start_s * 1000000.0))
    end_offset_us = int(round(end_s * 1000000.0))
    if live_start_qpc_us <= 0:
        return {
            "spec": recording_window_spec,
            "start_s": start_s,
            "end_s": end_s,
            "live_start_qpc": live_start_qpc,
            "live_start_qpc_us": 0,
            "start_qpc_us": 0,
            "end_qpc_us": 0,
            "live_start_wall_us": live_start_wall_us,
            "start_wall_us": live_start_wall_us + start_offset_us if live_start_wall_us >= 0 else -1,
            "end_wall_us": live_start_wall_us + end_offset_us if live_start_wall_us >= 0 else -1,
            "active": False,
            "reason": "missing_live_start_qpc",
        }
    return {
        "spec": recording_window_spec,
        "start_s": start_s,
        "end_s": end_s,
        "live_start_qpc": live_start_qpc,
        "live_start_qpc_us": live_start_qpc_us,
        "start_qpc_us": live_start_qpc_us + start_offset_us,
        "end_qpc_us": live_start_qpc_us + end_offset_us,
        "live_start_wall_us": live_start_wall_us,
        "start_wall_us": live_start_wall_us + start_offset_us if live_start_wall_us >= 0 else -1,
        "end_wall_us": live_start_wall_us + end_offset_us if live_start_wall_us >= 0 else -1,
        "active": True,
        "reason": "ok",
    }


def build_full_recording_perf_window_info(media_text, perf_summaries):
    live_start_qpc = parse_live_start_qpc(media_text)
    live_start_qpc_us = choose_perf_qpc_us_from_live_start(live_start_qpc, perf_summaries)
    live_start_wall_us = parse_live_start_qpc_wall_us(media_text)
    stop_wall_us = parse_stop_start_wall_us(media_text)
    if live_start_qpc_us <= 0 or live_start_wall_us < 0 or stop_wall_us <= live_start_wall_us:
        return None
    duration_us = stop_wall_us - live_start_wall_us
    return {
        "spec": "full-recording",
        "start_s": 0.0,
        "end_s": duration_us / 1000000.0,
        "live_start_qpc": live_start_qpc,
        "live_start_qpc_us": live_start_qpc_us,
        "start_qpc_us": live_start_qpc_us,
        "end_qpc_us": live_start_qpc_us + duration_us,
        "live_start_wall_us": live_start_wall_us,
        "start_wall_us": live_start_wall_us,
        "end_wall_us": stop_wall_us,
        "active": True,
        "reason": "derived_selected_recording_bounds",
        "automatic": True,
    }


def filter_media_text_for_recording_window(media_text, recording_window_info):
    if not recording_window_info or not recording_window_info.get("active"):
        return media_text
    start_wall_us = recording_window_info.get("start_wall_us", 0)
    end_wall_us = recording_window_info.get("end_wall_us", 0)
    if start_wall_us < 0 or end_wall_us <= start_wall_us:
        return media_text

    windowed_lines = []
    for line in media_text.splitlines():
        timestamp_us = parse_log_timestamp_us(line)
        if timestamp_us >= 0 and start_wall_us <= timestamp_us < end_wall_us:
            windowed_lines.append(line)
    return "\n".join(windowed_lines)


def merge_window_media_evidence(window_evidence, full_evidence):
    merged = dict(window_evidence)
    for key in (
        "screen_capture_backend_events",
        "wgc_summary",
        "wgc_quality",
        "recording_health",
        "wgc_smoothness_summary",
        "inject_summary",
        "inject_source_summary",
        "inject_quality_summary",
        "cfr_phase_lock_summary",
        "inject_contention",
        "final_packet_timelines",
        "final_metadata",
        "post_mux_audio_mismatch_delta_us",
        "post_mux_audio_priming",
        "audio_codec_contracts",
        "audio_finalizations",
        "stop_audio_tracks",
        "stop_audio_sources",
        "stop_audio_ingest",
        "stop_audio_overlap",
        "stop_app_audio_latency",
    ):
        merged[key] = full_evidence.get(key, [])
    return merged
