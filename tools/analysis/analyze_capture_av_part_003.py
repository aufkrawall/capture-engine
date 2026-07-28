

def analyze_inter_track_correlations(decoded_tracks, max_offset_ms=10):
    correlations = []
    for left_index in range(len(decoded_tracks)):
        for right_index in range(left_index + 1, len(decoded_tracks)):
            left = decoded_tracks[left_index]
            right = decoded_tracks[right_index]
            if not left.get("signature") or not right.get("signature"):
                continue
            signature_rate = min(left["signature_rate"], right["signature_rate"])
            if abs(left["signature_rate"] - right["signature_rate"]) > 0.01 or signature_rate <= 0:
                continue
            max_lag = max(0, int(round(max_offset_ms * signature_rate / 1000.0)))
            best = None
            for lag in range(-max_lag, max_lag + 1):
                left_start = max(0, -lag)
                right_start = max(0, lag)
                count = min(len(left["signature"]) - left_start, len(right["signature"]) - right_start)
                if count < 32:
                    continue
                left_values = left["signature"][left_start : left_start + count]
                right_values = right["signature"][right_start : right_start + count]
                left_mean = sum(left_values) / count
                right_mean = sum(right_values) / count
                numerator = 0.0
                left_energy = 0.0
                right_energy = 0.0
                for left_value, right_value in zip(left_values, right_values):
                    left_delta = left_value - left_mean
                    right_delta = right_value - right_mean
                    numerator += left_delta * right_delta
                    left_energy += left_delta * left_delta
                    right_energy += right_delta * right_delta
                denominator = math.sqrt(left_energy * right_energy)
                correlation = numerator / denominator if denominator > 0 else 0.0
                if best is None or correlation > best[0]:
                    best = (correlation, lag)
            if best is not None:
                correlations.append(
                    {
                        "left": left["audio_ordinal"],
                        "right": right["audio_ordinal"],
                        "correlation": best[0],
                        "offset_ms": best[1] * 1000.0 / signature_rate,
                    }
                )
    return correlations


def analyze_completed_capture_exact(ffprobe, ffmpeg, capture_path, threshold=1e-4):
    format_info, video_streams, audio_streams = analyze_streams(ffprobe, capture_path)
    video_stream = video_streams[0]
    fps_text = video_stream.get("avg_frame_rate") or video_stream.get("r_frame_rate")
    fps_fraction = parse_ratio_fraction(fps_text)
    nominal_fps = float(fps_fraction) if fps_fraction > 0 else 0.0
    packet_coverage = analyze_cfr_packet_coverage(ffprobe, capture_path, nominal_fps)
    frame_count = packet_coverage["packet_count"]
    target_frame_count = packet_coverage["expected_packets"]
    target_duration = Fraction(target_frame_count, 1) / fps_fraction if fps_fraction > 0 else Fraction(0, 1)
    decoded_tracks = [
        analyze_audio_tail_marker(ffmpeg, capture_path, ordinal, stream_info, threshold)
        for ordinal, stream_info in enumerate(audio_streams)
    ]
    track_reports = []
    endpoints = []
    for ordinal, (stream_info, decoded) in enumerate(zip(audio_streams, decoded_tracks)):
        sample_rate = decoded["sample_rate"]
        exact_target = target_duration * sample_rate if sample_rate > 0 else Fraction(0, 1)
        lattice_representable = exact_target.denominator == 1
        expected_samples = exact_target.numerator if lattice_representable else round_fraction(exact_target)
        decoder_clean = decoded["returncode"] == 0 and decoded["stderr"] == ""
        endpoint_exact = lattice_representable and decoded["samples"] == expected_samples
        if sample_rate > 0:
            endpoints.append(Fraction(decoded["samples"], sample_rate))
        track_reports.append(
            {
                "audio_ordinal": ordinal,
                "stream_index": parse_int(stream_info.get("index"), ordinal),
                "codec": stream_info.get("codec_name", ""),
                "sample_rate": sample_rate,
                "channels": decoded["channels"],
                "decoded_samples": decoded["samples"],
                "expected_samples": expected_samples,
                "sample_delta": decoded["samples"] - expected_samples,
                "lattice_representable": lattice_representable,
                "endpoint_exact": endpoint_exact,
                "decoder_clean": decoder_clean,
                "decoder_returncode": decoded["returncode"],
                "decoder_stderr": decoded["stderr"],
                "decoder_environment_stderr": decoded.get("ignored_environment_stderr", ""),
                "first_content_sample": decoded["first_marker_sample"],
                "last_content_sample": decoded["last_marker_sample"],
                "tail_silence_ms": decoded["tail_silence_ms"],
                "peak": decoded["peak"],
                "clipping_samples": decoded["clipping_samples"],
                "silent_frames": decoded["silent_samples"],
                "longest_silence_frames": decoded["longest_silence_samples"],
                "discontinuities": decoded["discontinuities"],
                "identical_channel_frames": decoded["identical_channel_frames"],
            }
        )
    endpoint_durations_identical = not endpoints or all(endpoint == endpoints[0] for endpoint in endpoints[1:])
    all_tracks_exact = all(track["endpoint_exact"] and track["decoder_clean"] for track in track_reports)
    correlations = analyze_inter_track_correlations(decoded_tracks)
    return {
        "analysis_mode": "exact",
        "authoritative": True,
        "capture": str(capture_path),
        "container_duration": parse_float(format_info.get("duration")),
        "video": {
            "codec": video_stream.get("codec_name", ""),
            "fps": fps_text,
            "frame_count": frame_count,
            "target_frame_count": target_frame_count,
            "target_duration_numerator": target_duration.numerator,
            "target_duration_denominator": target_duration.denominator,
            "packet_duration": packet_coverage["span"],
            "packet_coverage": packet_coverage,
        },
        "tracks": track_reports,
        "correlations": correlations,
        "endpoint_durations_identical": endpoint_durations_identical,
        "all_tracks_exact": all_tracks_exact,
        "decoder_clean": all(track["decoder_clean"] for track in track_reports),
        "cfr_packet_coverage_exact": packet_coverage["complete"],
        "passed": all_tracks_exact and endpoint_durations_identical and packet_coverage["complete"],
    }


def analyze_completed_capture_metadata(ffprobe, capture_path):
    format_info, video_streams, audio_streams = analyze_streams(ffprobe, capture_path)
    video_stream = video_streams[0]
    format_duration = parse_float(format_info.get("duration"))
    video_timing = analyze_video_stream_metadata(video_stream, format_duration)
    fps_text = video_stream.get("avg_frame_rate") or video_stream.get("r_frame_rate") or "0/0"
    tracks = []
    for ordinal, stream_info in enumerate(audio_streams):
        metadata = analyze_audio_stream_metadata(ordinal, stream_info, format_duration)
        tracks.append(
            {
                "audio_ordinal": ordinal,
                "stream_index": metadata["stream_index"],
                "codec": metadata["codec"],
                "sample_rate": metadata["sample_rate"],
                "channels": metadata["channels"],
                "metadata_samples": metadata["sample_total"],
                "metadata_duration": metadata["decoded_duration"],
                "frame_start": metadata["frame_start"],
                "frame_end": metadata["frame_end"],
            }
        )
    return {
        "analysis_mode": "metadata",
        "authoritative": False,
        "capture": str(capture_path),
        "container_duration": format_duration,
        "video": {
            "codec": video_stream.get("codec_name", ""),
            "fps": fps_text,
            "frame_count": video_timing["frame_count"],
            "metadata_duration": video_timing["duration"],
            "frame_start": video_timing["first_pts"],
            "frame_end": video_timing["frame_end"],
        },
        "tracks": tracks,
        "correlations": [],
        "endpoint_durations_identical": None,
        "all_tracks_exact": None,
        "decoder_clean": None,
        "cfr_packet_coverage_exact": None,
        "probe_succeeded": True,
        "passed": None,
    }


def analyze_completed_capture(ffprobe, ffmpeg, capture_path, full_scan, threshold=1e-4):
    if full_scan:
        return analyze_completed_capture_exact(ffprobe, ffmpeg, capture_path, threshold)
    return analyze_completed_capture_metadata(ffprobe, capture_path)


def attach_completed_capture_report(report, completed_capture):
    report["completed_capture"] = completed_capture
    if not completed_capture.get("authoritative", True):
        return
    if not completed_capture["all_tracks_exact"] or not completed_capture["endpoint_durations_identical"]:
        if "ce_audio_timeline_fault" not in report["verdicts"]:
            report["verdicts"].append("ce_audio_timeline_fault")
        report["faults"]["audio_timeline"] = True
    if not completed_capture["cfr_packet_coverage_exact"]:
        if "ce_visual_timeline_fault" not in report["verdicts"]:
            report["verdicts"].append("ce_visual_timeline_fault")
        report["faults"]["visual_timeline"] = True
    if len(report["verdicts"]) > 1 and "unknown" in report["verdicts"]:
        report["verdicts"].remove("unknown")


def count_unjoined_late_app_source_backlog(text):
    live_join_sources = {match.group(1) for match in LATE_APP_LIVE_JOIN_SRC_RE.finditer(text)}

    typed_stop_sources = {}
    for match in STOP_AUDIO_SOURCE_RE.finditer(text):
        process = match.group(10) or ""
        typed_stop_sources[match.group(1)] = bool(process and process != "-")

    count = 0
    matched_structured_line = False
    for match in LATE_APP_PRIMED_SRC_RE.finditer(text):
        matched_structured_line = True
        source = match.group(1)
        explicit_app = match.group(3)
        if explicit_app == "0":
            continue
        if explicit_app is None:
            if source in typed_stop_sources and not typed_stop_sources[source]:
                continue
        if parse_int(match.group(2), 0) >= 1000 and source not in live_join_sources:
            count += 1
    if matched_structured_line:
        return count
    return len(LOG_PATTERNS["audio_late_app_source_backlog"].findall(text))


def count_audio_extreme_drift_events(text):
    force_drain_by_source = {}
    live_count = 0
    stop_force_drain_count = 0
    for line in text.splitlines():
        drain_match = APP_DRAIN_STATE_RE.search(line)
        if drain_match:
            force_drain_by_source[drain_match.group(1)] = drain_match.group(2) == "1"

        drift_match = AUDIO_EXTREME_DRIFT_SRC_RE.search(line)
        if not drift_match:
            continue
        source = drift_match.group(1)
        explicit_force_drain = drift_match.group(2)
        is_force_drain = (
            explicit_force_drain == "1"
            if explicit_force_drain is not None
            else force_drain_by_source.get(source, False)
        )
        if is_force_drain:
            stop_force_drain_count += 1
        else:
            live_count += 1
    return live_count, stop_force_drain_count


def analyze_log(log_path):
    if not log_path:
        return None
    text = log_path.read_text(encoding="utf-8", errors="replace")
    return analyze_log_text(text)


def analyze_log_text(text):
    lines = text.splitlines()
    counts = {name: len(pattern.findall(text)) for name, pattern in LOG_PATTERNS.items()}
    counts["audio_late_app_source_backlog"] = count_unjoined_late_app_source_backlog(text)
    raw_extreme_drift = counts["audio_extreme_drift"]
    live_extreme_drift, historical_stop_force_drain = count_audio_extreme_drift_events(text)
    classified_extreme_drift = live_extreme_drift + historical_stop_force_drain
    counts["audio_extreme_drift"] = live_extreme_drift + max(
        0, raw_extreme_drift - classified_extreme_drift
    )
    counts["audio_stop_force_drain_backlog"] += historical_stop_force_drain
    cadence_window_count = 0

    cadence_metrics = {
        "age_max_us": [],
        "sel_miss": [],
        "stale_unique": [],
        "ancient": [],
        "rep_no_fresh": [],
        "overload_flags": [],
        "wgc_sel_bias_abs_us": [],
        "wgc_shortfall_ms": [],
        "wgc_lead_excess_ms": [],
        "wgc_oldest_ms": [],
        "wgc_buffered_frames": [],
        "wgc_live_rebase_max_ticks": [],
        "wgc_startup_frame_age_us": [],
        "wgc_encoder_limited_drops": [],
        "wgc_phase_error_us": [],
        "wgc_sync_delay_holds": [],
        "wgc_too_new_lead_us": [],
        "wgc_av_delay_ms": [],
        "wgc_sync_delay_source_limited_holds": [],
        "wgc_sync_delay_policy_holds": [],
        "wgc_low_source_bypass": [],
        "wgc_mode_mismatch": [],
        "wgc_source_backtrack": [],
        "wgc_delay_reservoir_low_ticks": [],
        "wgc_delay_residual_avg_abs_us": [],
        "wgc_delay_residual_max_us": [],
        "wgc_delay_residual_p95_us": [],
    }
    for line in lines:
        smoothness_match = WGC_SMOOTHNESS_SUMMARY_RE.search(line)
        if smoothness_match:
            cadence_metrics["wgc_encoder_limited_drops"].append(parse_int(smoothness_match.group(1)))
            cadence_metrics["wgc_phase_error_us"].append(parse_int(smoothness_match.group(4)))
            cadence_metrics["wgc_sync_delay_holds"].append(parse_int(smoothness_match.group(10)))
            cadence_metrics["wgc_too_new_lead_us"].append(parse_int(smoothness_match.group(11)))
            cadence_metrics["wgc_av_delay_ms"].append(parse_float(smoothness_match.group(12)))
            smoothness_extra = WGC_SMOOTHNESS_EXTRA_RE.search(line)
            if smoothness_extra:
                cadence_metrics["wgc_sync_delay_source_limited_holds"].append(parse_int(smoothness_extra.group(1)))
                cadence_metrics["wgc_sync_delay_policy_holds"].append(parse_int(smoothness_extra.group(2)))
            delay_realization = WGC_DELAY_REALIZATION_RE.search(line)
            if delay_realization:
                cadence_metrics["wgc_delay_reservoir_low_ticks"].append(parse_int(delay_realization.group(3)))
                cadence_metrics["wgc_delay_residual_avg_abs_us"].append(parse_int(delay_realization.group(8)))
                cadence_metrics["wgc_delay_residual_max_us"].append(parse_int(delay_realization.group(9)))
                cadence_metrics["wgc_delay_residual_p95_us"].append(parse_int(delay_realization.group(10)))
            cadence_metrics["wgc_low_source_bypass"].append(parse_int(smoothness_match.group(16)))
            cadence_metrics["wgc_mode_mismatch"].append(parse_int(smoothness_match.group(17)))
            cadence_metrics["wgc_source_backtrack"].append(parse_int(smoothness_match.group(18)))
        elif "[WGC CFR SMOOTHNESS " in line:
            smoothness_extra = WGC_SMOOTHNESS_EXTRA_RE.search(line)
            if smoothness_extra:
                cadence_metrics["wgc_sync_delay_source_limited_holds"].append(parse_int(smoothness_extra.group(1)))
                cadence_metrics["wgc_sync_delay_policy_holds"].append(parse_int(smoothness_extra.group(2)))
            delay_realization = WGC_DELAY_REALIZATION_RE.search(line)
            if delay_realization:
                cadence_metrics["wgc_delay_reservoir_low_ticks"].append(parse_int(delay_realization.group(3)))
                cadence_metrics["wgc_delay_residual_avg_abs_us"].append(parse_int(delay_realization.group(8)))
                cadence_metrics["wgc_delay_residual_max_us"].append(parse_int(delay_realization.group(9)))
                cadence_metrics["wgc_delay_residual_p95_us"].append(parse_int(delay_realization.group(10)))
        startup_frame_age_match = WGC_STARTUP_FRAME_AGE_RE.search(line)
        if startup_frame_age_match:
            cadence_metrics["wgc_startup_frame_age_us"].append(parse_int(startup_frame_age_match.group(1)))
        summary_live_rebase_match = WGC_SUMMARY_LIVE_REBASE_RE.search(line)
        if summary_live_rebase_match:
            cadence_metrics["wgc_live_rebase_max_ticks"].append(parse_int(summary_live_rebase_match.group(1)))
        if "[Cadence Health]" not in line:
            continue
        cadence_window_count += 1
        age_max_match = CADENCE_AGEMAX_RE.search(line)
        if age_max_match:
            cadence_metrics["age_max_us"].append(parse_int(age_max_match.group(1)))
        sel_miss_match = CADENCE_SELMISS_RE.search(line)
        if sel_miss_match:
            cadence_metrics["sel_miss"].append(parse_int(sel_miss_match.group(1)))
        stale_unique_match = CADENCE_STALEUNI_RE.search(line)
        if stale_unique_match:
            cadence_metrics["stale_unique"].append(parse_int(stale_unique_match.group(1)))
        ancient_match = CADENCE_ANCIENT_RE.search(line)
        if ancient_match:
            cadence_metrics["ancient"].append(parse_int(ancient_match.group(1)))
        rep_no_fresh_match = CADENCE_REPNOFRESH_RE.search(line)
        if rep_no_fresh_match:
            cadence_metrics["rep_no_fresh"].append(parse_int(rep_no_fresh_match.group(1)))
        overload_match = CADENCE_OVER_RE.search(line)
        if overload_match:
            cadence_metrics["overload_flags"].append(int(overload_match.group(1), 16))
        wgc_sel_bias_match = CADENCE_WGC_SEL_BIAS_RE.search(line)
        if wgc_sel_bias_match:
            cadence_metrics["wgc_sel_bias_abs_us"].append(abs(parse_int(wgc_sel_bias_match.group(1))))
        shortfall_match = CADENCE_SHORTFALL_RE.search(line)
        if shortfall_match:
            cadence_metrics["wgc_shortfall_ms"].append(int(round(parse_float(shortfall_match.group(1)))))
        lead_excess_match = CADENCE_LEAD_EXCESS_RE.search(line)
        if lead_excess_match:
            cadence_metrics["wgc_lead_excess_ms"].append(int(round(parse_float(lead_excess_match.group(1)))))
        oldest_match = CADENCE_OLDEST_RE.search(line)
        if oldest_match:
            cadence_metrics["wgc_oldest_ms"].append(int(round(parse_float(oldest_match.group(1)))))
        buf_now_match = CADENCE_BUFNOW_RE.search(line)
        if buf_now_match:
            cadence_metrics["wgc_buffered_frames"].append(parse_int(buf_now_match.group(1)))
        live_rebase_match = CADENCE_WGC_LIVE_REBASE_RE.search(line)
        if live_rebase_match:
            cadence_metrics["wgc_live_rebase_max_ticks"].append(parse_int(live_rebase_match.group(1)))
        low_source_bypass_match = CADENCE_ENC_LOW_BYPASS_RE.search(line)
        if low_source_bypass_match:
            cadence_metrics["wgc_low_source_bypass"].append(parse_int(low_source_bypass_match.group(2)))
        mode_mismatch_match = CADENCE_MODE_MISMATCH_RE.search(line)
        if mode_mismatch_match:
            cadence_metrics["wgc_mode_mismatch"].append(parse_int(mode_mismatch_match.group(2)))
        source_backtrack_match = CADENCE_SOURCE_BACKTRACK_RE.search(line)
        if source_backtrack_match:
            cadence_metrics["wgc_source_backtrack"].append(parse_int(source_backtrack_match.group(2)))

    return {
        "counts": counts,
        "cadence_windows": cadence_window_count,
        "max_age_max_us": max(cadence_metrics["age_max_us"]) if cadence_metrics["age_max_us"] else 0,
        "max_sel_miss": max(cadence_metrics["sel_miss"]) if cadence_metrics["sel_miss"] else 0,
        "max_stale_unique": max(cadence_metrics["stale_unique"]) if cadence_metrics["stale_unique"] else 0,
        "max_ancient": max(cadence_metrics["ancient"]) if cadence_metrics["ancient"] else 0,
        "max_rep_no_fresh": max(cadence_metrics["rep_no_fresh"]) if cadence_metrics["rep_no_fresh"] else 0,
        "max_wgc_sel_bias_abs_us": max(cadence_metrics["wgc_sel_bias_abs_us"])
        if cadence_metrics["wgc_sel_bias_abs_us"]
        else 0,
        "max_wgc_shortfall_ms": max(cadence_metrics["wgc_shortfall_ms"])
        if cadence_metrics["wgc_shortfall_ms"]
        else 0,
        "max_wgc_lead_excess_ms": max(cadence_metrics["wgc_lead_excess_ms"])
        if cadence_metrics["wgc_lead_excess_ms"]
        else 0,
        "max_wgc_oldest_ms": max(cadence_metrics["wgc_oldest_ms"]) if cadence_metrics["wgc_oldest_ms"] else 0,
        "max_wgc_buffered_frames": max(cadence_metrics["wgc_buffered_frames"])
        if cadence_metrics["wgc_buffered_frames"]
        else 0,
        "max_wgc_live_rebase_ticks": max(cadence_metrics["wgc_live_rebase_max_ticks"])
        if cadence_metrics["wgc_live_rebase_max_ticks"]
        else 0,
        "max_wgc_startup_frame_age_us": max(cadence_metrics["wgc_startup_frame_age_us"])
        if cadence_metrics["wgc_startup_frame_age_us"]
        else 0,
        "max_wgc_encoder_limited_drops": max(cadence_metrics["wgc_encoder_limited_drops"])
        if cadence_metrics["wgc_encoder_limited_drops"]
        else 0,
        "max_wgc_phase_error_us": max(cadence_metrics["wgc_phase_error_us"])
        if cadence_metrics["wgc_phase_error_us"]
        else 0,
        "max_wgc_low_source_bypass": max(cadence_metrics["wgc_low_source_bypass"])
        if cadence_metrics["wgc_low_source_bypass"]
        else 0,
        "max_wgc_mode_mismatch": max(cadence_metrics["wgc_mode_mismatch"])
        if cadence_metrics["wgc_mode_mismatch"]
        else 0,
        "max_wgc_source_backtrack": max(cadence_metrics["wgc_source_backtrack"])
        if cadence_metrics["wgc_source_backtrack"]
        else 0,
        "max_wgc_sync_delay_source_limited_holds": max(cadence_metrics["wgc_sync_delay_source_limited_holds"])
        if cadence_metrics["wgc_sync_delay_source_limited_holds"]
        else 0,
        "max_wgc_sync_delay_policy_holds": max(cadence_metrics["wgc_sync_delay_policy_holds"])
        if cadence_metrics["wgc_sync_delay_policy_holds"]
        else 0,
        "max_wgc_delay_reservoir_low_ticks": max(cadence_metrics["wgc_delay_reservoir_low_ticks"])
        if cadence_metrics["wgc_delay_reservoir_low_ticks"]
        else 0,
        "max_wgc_delay_residual_avg_abs_us": max(cadence_metrics["wgc_delay_residual_avg_abs_us"])
        if cadence_metrics["wgc_delay_residual_avg_abs_us"]
        else 0,
        "max_wgc_delay_residual_us": max(cadence_metrics["wgc_delay_residual_max_us"])
        if cadence_metrics["wgc_delay_residual_max_us"]
        else 0,
        "max_wgc_delay_residual_p95_us": max(cadence_metrics["wgc_delay_residual_p95_us"])
        if cadence_metrics["wgc_delay_residual_p95_us"]
        else 0,
        "saw_encoder_overload": any(flags & 0x1 for flags in cadence_metrics["overload_flags"]),
        "saw_mux_overload": any(flags & 0x2 for flags in cadence_metrics["overload_flags"]),
    }


def read_text_if_exists(path):
    try:
        return path.read_text(encoding="utf-8", errors="replace") if path and path.exists() else ""
    except OSError:
        return ""


def parse_key_value_manifest(path):
    manifest = {}
    for line in read_text_if_exists(path).splitlines():
        if "=" not in line:
            continue
        key, value = line.split("=", 1)
        manifest[key.strip()] = value.strip()
    return manifest


def parse_session_manifest(session_dir):
    return parse_key_value_manifest(session_dir / "session_manifest.txt")


MEDIA_LOG_RE = re.compile(r"^media_(?P<recording_id>[A-Za-z0-9_-]+)_(?P<pid>[0-9]+)\.log$", re.IGNORECASE)


def is_media_log_path(path):
    lower_name = path.name.lower()
    return lower_name == "media.log" or MEDIA_LOG_RE.match(path.name) is not None


def discover_recording_evidence(session_dir):
    manifests_by_log = {}
    for path in sorted(session_dir.glob("recording_*.manifest")):
        manifest = parse_key_value_manifest(path)
        media_name = manifest.get("media_log", "")
        if media_name:
            manifests_by_log[media_name.lower()] = (path, manifest)

    recordings = []
    for path in sorted(session_dir.glob("*.log")):
        if not is_media_log_path(path):
            continue
        match = MEDIA_LOG_RE.match(path.name)
        recording_id = match.group("recording_id") if match else "legacy"
        media_pid = int(match.group("pid")) if match else None
        manifest_path = None
        recording_manifest = {}
        manifest_entry = manifests_by_log.get(path.name.lower())
        if manifest_entry:
            manifest_path, recording_manifest = manifest_entry
            recording_id = recording_manifest.get("recording_id", recording_id)
            try:
                media_pid = int(recording_manifest.get("media_pid", media_pid))
            except (TypeError, ValueError):
                pass
        recordings.append(
            {
                "recording_id": recording_id,
                "media_pid": media_pid,
                "media_log": path,
                "manifest_path": manifest_path,
                "manifest": recording_manifest,
            }
        )
    return recordings


def resolve_recording_evidence(session_dir, recording_id=None, media_log=None):
    recordings = discover_recording_evidence(session_dir)
    if media_log is not None:
        selected_path = Path(media_log)
        if not selected_path.is_absolute():
            selected_path = session_dir / selected_path
        if not selected_path.exists():
            raise ValueError(f"media log not found: {selected_path}")
        for recording in recordings:
            if recording["media_log"].resolve() == selected_path.resolve():
                return recording, recordings
        return {
            "recording_id": recording_id or "explicit",
            "media_pid": None,
            "media_log": selected_path,
            "manifest_path": None,
            "manifest": {},
        }, recordings

    if recording_id:
        matching = [item for item in recordings if item["recording_id"].lower() == recording_id.lower()]
        if len(matching) != 1:
            detail = ", ".join(
                f"{item['recording_id']}:{item['media_log'].name}" for item in recordings
            ) or "none"
            raise ValueError(
                f"recording id {recording_id!r} matched {len(matching)} media logs; available: {detail}"
            )
        return matching[0], recordings

    if len(recordings) > 1:
        detail = ", ".join(f"{item['recording_id']}:{item['media_log'].name}" for item in recordings)
        raise ValueError(
            "multiple recordings exist in this controller session; select one with --recording-id or "
            f"--media-log, or use --all-recordings. Available: {detail}"
        )
    if recordings:
        return recordings[0], recordings
    return {
        "recording_id": None,
        "media_pid": None,
        "media_log": session_dir / "media.log",
        "manifest_path": None,
        "manifest": {},
    }, recordings


def normalize_screen_capture_backend(value):
    normalized = str(value or "").strip().lower().replace("-", "_")
    if normalized in ("dxgiduplication", "dxgi_duplication", "desktop_dup", "duplication"):
        return "dxgi_dup"
    if normalized in ("dxgi_dup", "wgc"):
        return normalized
    return ""


def resolve_screen_capture_backend_history(manifest, media_evidence):
    history = []

    def append_backend(value):
        backend = normalize_screen_capture_backend(value)
        if backend and (not history or history[-1] != backend):
            history.append(backend)

    for event in media_evidence.get("screen_capture_backend_events", []):
        append_backend(event.get("backend", ""))
    if not history:
        for perf in media_evidence.get("wgc_perf", []):
            append_backend(perf.get("backend", ""))
        for quality in media_evidence.get("wgc_quality", []):
            append_backend(quality.get("backend", ""))
    if not history:
        append_backend(manifest.get("capture_method", ""))
    return history


def resolve_screen_capture_backend(manifest, media_evidence):
    history = resolve_screen_capture_backend_history(manifest, media_evidence)
    if not history:
        return "screen_capture"
    if len(history) == 1:
        return history[0]
    return "_to_".join(history)
