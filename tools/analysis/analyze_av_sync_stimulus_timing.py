

def infer_capture_timing(frames, manifest):
    if not frames:
        return {
            "capture_to_stimulus_offset_seconds": 0.0,
            "anchor_error_seconds": 0.0,
            "anchor_transition_matches": 0,
        }
    _, transitions = compress_states(
        [(frame["pts"], frame["palette"]) for frame in frames],
        min_duration=0.12,
        transition_time_mode="first_new_sample",
    )
    if not transitions:
        return {
            "capture_to_stimulus_offset_seconds": 0.0,
            "anchor_error_seconds": 0.0,
            "anchor_transition_matches": 0,
        }

    capture_start = min(frame["pts"] for frame in frames)
    capture_end = max(frame["pts"] for frame in frames)
    capture_span = max(0.0, capture_end - capture_start)
    duration_seconds = parse_float(manifest.get("duration_seconds"), frames[-1]["pts"] + 2.0)
    period = parse_float(manifest.get("event_period_seconds"), 1.0)
    if period <= 0.0:
        period = 1.0
    min_offset = -period
    max_offset = max(min_offset, duration_seconds - capture_span + period)
    expected = expected_transition_events(
        manifest, duration_seconds + max((item["time"] for item in transitions), default=0.0) + 2.0
    )
    by_pair = {}
    by_to = {}
    for item in expected:
        by_pair.setdefault((item["from"], item["to"]), []).append(item["time"])
        by_to.setdefault(item["to"], []).append(item["time"])

    candidates = []
    for transition in transitions:
        choices = by_pair.get((transition["from"], transition["to"])) or by_to.get(transition["to"], [])
        for expected_time in choices:
            offset = expected_time - transition["time"]
            if min_offset <= offset <= max_offset:
                candidates.append(offset)
    if not candidates:
        return {
            "capture_to_stimulus_offset_seconds": 0.0,
            "anchor_error_seconds": 0.0,
            "anchor_transition_matches": 0,
        }

    def score_candidate(offset):
        if offset < min_offset or offset > max_offset:
            return (999999.0 + abs(offset), 999999.0, 0)
        if capture_end + offset > duration_seconds + period:
            return (999999.0 + capture_end + offset - duration_seconds, 999999.0, 0)
        residuals = []
        missing = 0
        for transition in transitions:
            stimulus_time = transition["time"] + offset
            choices = by_pair.get((transition["from"], transition["to"])) or by_to.get(transition["to"], [])
            if not choices:
                missing += 1
                continue
            residuals.append(min(abs(expected_time - stimulus_time) for expected_time in choices))
        if not residuals:
            return (999999.0, 999999.0, 0)
        median = statistics.median(residuals)
        worst = max(residuals)
        return (median + missing * 0.25 + worst * 0.01, median, len(residuals))

    best_offset = min(candidates, key=lambda value: (*score_candidate(value), value))
    _, median_error, matches = score_candidate(best_offset)
    return {
        "capture_to_stimulus_offset_seconds": best_offset,
        "anchor_error_seconds": median_error,
        "anchor_transition_matches": matches,
    }


def load_source_stalls(manifest, capture_to_stimulus_offset=0.0):
    stalls = []
    for item in manifest.get("source_stalls", []):
        if not isinstance(item, dict):
            continue
        start = parse_float(item.get("actual_start_seconds"), -1.0)
        end = parse_float(item.get("actual_end_seconds"), -1.0)
        if start < 0.0 or end <= start:
            start = parse_float(item.get("requested_start_seconds"), -1.0)
            end = parse_float(item.get("requested_end_seconds"), -1.0)
        if start < 0.0 or end <= start:
            continue
        stimulus_start = start
        stimulus_end = end
        start -= capture_to_stimulus_offset
        end -= capture_to_stimulus_offset
        tolerance = parse_float(item.get("tolerance_seconds"), 0.05)
        expected_span = parse_float(
            item.get("expected_repeat_span_seconds"),
            max(0.0, parse_float(item.get("requested_duration_ms"), 0.0) / 1000.0),
        )
        stalls.append(
            {
                "index": parse_int(item.get("index"), len(stalls)),
                "start": start,
                "end": end,
                "stimulus_start": stimulus_start,
                "stimulus_end": stimulus_end,
                "tolerance": tolerance,
                "expected_repeat_span_seconds": expected_span,
                "suppressed_present_count": parse_int(item.get("suppressed_present_count"), 0),
            }
        )
    return stalls


def cluster_overlaps_stall(cluster, stall):
    return (
        cluster["end_pts"] >= stall["start"] - stall["tolerance"]
        and cluster["start_pts"] <= stall["end"] + stall["tolerance"]
    )


def classify_repeat_clusters(clusters, manifest, stalls=None, nominal_output_fps=0.0):
    if stalls is None:
        stalls = load_source_stalls(manifest)
    source_repeat_run = expected_source_repeat_run(manifest, nominal_output_fps)
    planned = []
    source_fps_limited = []
    unplanned = []
    matched_stalls = set()
    for cluster in clusters:
        matches = [stall for stall in stalls if cluster_overlaps_stall(cluster, stall)]
        if matches:
            cluster_mid = (cluster["start_pts"] + cluster["end_pts"]) / 2.0
            stall = min(matches, key=lambda item: abs(cluster_mid - ((item["start"] + item["end"]) / 2.0)))
            duration = max(0.0, cluster["end_pts"] - cluster["start_pts"])
            expected = max(0.0, stall["expected_repeat_span_seconds"])
            within_duration = expected <= 0.0 or abs(duration - expected) <= stall["tolerance"] + (1.0 / 15.0)
            item = dict(cluster)
            item["source_stall_index"] = stall["index"]
            item["expected_span_seconds"] = expected
            item["duration_within_tolerance"] = within_duration
            if within_duration:
                matched_stalls.add(stall["index"])
                item["classification"] = "planned_source_stall"
                planned.append(item)
            elif cluster["frames"] <= 2 or duration <= 0.025:
                item["classification"] = "planned_source_stall_boundary_repeat"
                if cluster["frames"] <= 2:
                    item["expected_max_frames"] = 2
                item["expected_max_duration_seconds"] = 0.025
                source_fps_limited.append(item)
            elif source_repeat_run > 1 and cluster["frames"] <= source_repeat_run:
                item["classification"] = "source_fps_limited_repeat"
                item["expected_max_frames"] = source_repeat_run
                source_fps_limited.append(item)
            else:
                item["classification"] = "unplanned_repeat_cluster"
                if source_repeat_run > 1:
                    item["expected_max_frames"] = source_repeat_run
                unplanned.append(item)
            continue
        item = dict(cluster)
        if source_repeat_run > 1 and cluster["frames"] <= source_repeat_run:
            item["classification"] = "source_fps_limited_repeat"
            item["expected_max_frames"] = source_repeat_run
            source_fps_limited.append(item)
        else:
            item["classification"] = "unplanned_repeat_cluster"
            if source_repeat_run > 1:
                item["expected_max_frames"] = source_repeat_run
            unplanned.append(item)
    missing = [
        stall
        for stall in stalls
        if stall["suppressed_present_count"] > 0 and stall["index"] not in matched_stalls
    ]
    return planned, source_fps_limited, unplanned, missing


def time_in_source_stall(time_value, stalls):
    for stall in stalls:
        if stall["start"] - stall["tolerance"] <= time_value <= stall["end"] + stall["tolerance"]:
            return True
    return False


def stimulus_time_in_source_stall(stimulus_time, stalls):
    for stall in stalls:
        if stall["stimulus_start"] - stall["tolerance"] <= stimulus_time <= stall["stimulus_end"] + stall["tolerance"]:
            return True
    return False


def filter_transitions_outside_source_stalls(transitions, stalls):
    if not stalls:
        return transitions
    return [item for item in transitions if not time_in_source_stall(item["time"], stalls)]


def filter_transitions_for_analysis(transitions, video_summary, stimulus_time_adjust=0.0):
    timing = video_summary.get("timing", {})
    capture_to_stimulus_offset = parse_float(timing.get("capture_to_stimulus_offset_seconds"), 0.0)
    analysis_start = parse_float(video_summary.get("analysis_start_seconds"), 0.0)
    stalls = video_summary.get("source_stalls", [])
    filtered = []
    for item in transitions:
        stimulus_time = item["time"] + capture_to_stimulus_offset + stimulus_time_adjust
        if stimulus_time + 1e-6 < analysis_start:
            continue
        if stimulus_time_in_source_stall(stimulus_time, stalls):
            continue
        filtered.append(item)
    return filtered


def dedupe_transitions_by_target_state(transitions):
    deduped = []
    seen = set()
    for item in transitions:
        target = item.get("to")
        if target in seen:
            continue
        seen.add(target)
        deduped.append(item)
    return deduped


def use_video_transition_uncertainty_intervals(transitions):
    interval_transitions = []
    for item in transitions:
        updated = dict(item)
        display_time = parse_float(item.get("display_time"), float("nan"))
        if math.isfinite(display_time):
            source_time = parse_float(item.get("time"), display_time)
            updated["time_min"] = min(source_time, display_time)
            updated["time_max"] = max(source_time, display_time)
            updated["time_mode_for_audio"] = "source_to_visible_interval"
        interval_transitions.append(updated)
    return interval_transitions


def compress_states(samples, min_duration=0.08, transition_time_mode="midpoint"):
    segments = []
    active = None
    for time_value, state in samples:
        if state is None or state < 0:
            continue
        if active is None:
            active = {"state": state, "start": time_value, "end": time_value, "count": 1}
        elif active["state"] == state:
            active["end"] = time_value
            active["count"] += 1
        else:
            if active["end"] - active["start"] >= min_duration:
                segments.append(active)
            active = {"state": state, "start": time_value, "end": time_value, "count": 1}
    if active and active["end"] - active["start"] >= min_duration:
        segments.append(active)

    transitions = []
    for prev, cur in zip(segments, segments[1:]):
        transition_time = (
            cur["start"] if transition_time_mode == "first_new_sample" else (prev["end"] + cur["start"]) / 2.0
        )
        transitions.append(
            {
                "from": prev["state"],
                "to": cur["state"],
                "time": transition_time,
            }
        )
    return segments, transitions


def build_video_transition_segments(frames, min_duration=0.08):
    segments = []
    active = None
    for frame in frames:
        state = frame.get("palette")
        if state is None or state < 0:
            continue
        if active is None:
            active = {
                "state": state,
                "start": frame["pts"],
                "end": frame["pts"],
                "count": 1,
                "first_frame": frame,
                "last_frame": frame,
                "last_distinct_frame": frame,
            }
        elif active["state"] == state:
            if frame.get("marker") != active["last_distinct_frame"].get("marker"):
                active["last_distinct_frame"] = frame
            active["end"] = frame["pts"]
            active["count"] += 1
            active["last_frame"] = frame
        else:
            if active["end"] - active["start"] >= min_duration:
                segments.append(active)
            active = {
                "state": state,
                "start": frame["pts"],
                "end": frame["pts"],
                "count": 1,
                "first_frame": frame,
                "last_frame": frame,
                "last_distinct_frame": frame,
            }
    if active and active["end"] - active["start"] >= min_duration:
        segments.append(active)
    return segments


def interpolate_source_transition_time(prev_frame, cur_frame, from_state, to_state, manifest):
    prev_stimulus = prev_frame.get("stimulus_seconds_marker")
    cur_stimulus = cur_frame.get("stimulus_seconds_marker")
    if prev_stimulus is None or cur_stimulus is None:
        return None
    if not math.isfinite(prev_stimulus) or not math.isfinite(cur_stimulus) or cur_stimulus <= prev_stimulus:
        return None

    expected = expected_transition_events(manifest, cur_stimulus + 2.0)
    tolerance = max(0.002, (cur_stimulus - prev_stimulus) * 0.25)
    choices = [
        item
        for item in expected
        if item["from"] == from_state
        and item["to"] == to_state
        and prev_stimulus - tolerance <= item["time"] <= cur_stimulus + tolerance
    ]
    if not choices:
        return None

    event = min(choices, key=lambda item: abs(item["time"] - ((prev_stimulus + cur_stimulus) * 0.5)))
    span = cur_stimulus - prev_stimulus
    ratio = min(1.0, max(0.0, (event["time"] - prev_stimulus) / span))
    display_span = cur_frame["pts"] - prev_frame["pts"]
    if display_span <= 0.0:
        return None
    return {
        "time": prev_frame["pts"] + ratio * display_span,
        "stimulus_time": event["time"],
        "mode": "source_marker_interpolated",
        "previous_stimulus_time": prev_stimulus,
        "current_stimulus_time": cur_stimulus,
        "previous_marker": prev_frame.get("marker"),
        "current_marker": cur_frame.get("marker"),
    }


def build_video_transitions(frames, manifest):
    segments = build_video_transition_segments(frames, min_duration=0.12)
    transitions = []
    for prev, cur in zip(segments, segments[1:]):
        prev_frame = prev.get("last_distinct_frame", prev["last_frame"])
        cur_frame = cur["first_frame"]
        display_time = cur_frame["pts"]
        refined = interpolate_source_transition_time(prev_frame, cur_frame, prev["state"], cur["state"], manifest)
        transition = {
            "from": prev["state"],
            "to": cur["state"],
            "time": refined["time"] if refined else display_time,
            "display_time": display_time,
            "time_mode": refined["mode"] if refined else "first_new_sample",
        }
        if refined:
            transition.update(
                {
                    "stimulus_time": refined["stimulus_time"],
                    "previous_stimulus_time": refined["previous_stimulus_time"],
                    "current_stimulus_time": refined["current_stimulus_time"],
                    "previous_marker": refined["previous_marker"],
                    "current_marker": refined["current_marker"],
                }
            )
        transitions.append(transition)
    return transitions


def summarize_video(frames, manifest, timing, app_frame_anchors=None):
    if not frames:
        return {"error": "no decoded video frames"}
    marker_timing_available = annotate_video_frames_from_app_anchors(frames, app_frame_anchors or [], manifest)
    capture_to_stimulus_offset = parse_float(timing.get("capture_to_stimulus_offset_seconds"), 0.0)
    analysis_start = parse_float(manifest.get("analysis_start_seconds"), 0.0)
    source_stalls = load_source_stalls(manifest, capture_to_stimulus_offset)
    nominal_output_fps = infer_nominal_video_fps(frames)
    source_repeat_run = expected_source_repeat_run(manifest, nominal_output_fps)
    repeated = 0
    longest_repeat = 1
    current_repeat = 1
    repeat_clusters = []
    current_repeat_start = None
    out_of_order = 0
    out_of_order_details = []
    corrupt = 0
    motion_missing = 0
    motion_stalls = 0
    motion_stall_run = 1
    longest_motion_stall = 1
    motion_error_frames = 0
    motion_error_max = 0.0
    fast_motion_available = parse_int(manifest.get("motion_lane_count"), 1) >= 2
    fast_motion_missing = 0
    fast_motion_stalls = 0
    fast_motion_stall_run = 1
    longest_fast_motion_stall = 1
    fast_motion_error_frames = 0
    fast_motion_error_max = 0.0
    previous = None
    previous_motion = None
    previous_fast_motion = None
    for frame in frames:
        stimulus_time = frame["pts"] + capture_to_stimulus_offset
        if stimulus_time + 1e-6 < analysis_start:
            previous = None
            previous_motion = None
            previous_fast_motion = None
            current_repeat = 1
            current_repeat_start = None
            motion_stall_run = 1
            fast_motion_stall_run = 1
            continue
        expected_source_duplicate = (
            previous is not None
            and source_repeat_run > 1
            and frame["marker"] == previous["marker"]
            and current_repeat < source_repeat_run
        )
        if (
            frame["marker_bad_tiles"] > 0
            or not frame["marker_inverse_ok"]
            or not frame["checksum_ok"]
            or not frame["parity_ok"]
            or frame["event_color_confidence"] < 0.50
        ):
            corrupt += 1
        in_planned_stall = time_in_source_stall(frame["pts"], source_stalls)
        if frame["motion"] is None:
            motion_missing += 1
        else:
            expected_motion = expected_motion_from_stimulus(frame["pts"] + capture_to_stimulus_offset, manifest)
            frame["motion_expected"] = expected_motion
            error = circular_distance(frame["motion"], expected_motion)
            frame["motion_error"] = error
            motion_error_max = max(motion_error_max, error)
            if error > 0.035 and not in_planned_stall and not expected_source_duplicate:
                motion_error_frames += 1
        fast_motion = frame.get("fast_motion")
        if fast_motion_available:
            if fast_motion is None:
                fast_motion_missing += 1
            else:
                expected_fast_motion = expected_fast_motion_from_stimulus(
                    frame["pts"] + capture_to_stimulus_offset, manifest
                )
                frame["fast_motion_expected"] = expected_fast_motion
                fast_error = circular_distance(fast_motion, expected_fast_motion)
                frame["fast_motion_error"] = fast_error
                fast_motion_error_max = max(fast_motion_error_max, fast_error)
                if fast_error > 0.045 and not in_planned_stall and not expected_source_duplicate:
                    fast_motion_error_frames += 1
        if previous is not None:
            if frame["marker"] == previous["marker"]:
                repeated += 1
                current_repeat += 1
                if current_repeat_start is None:
                    current_repeat_start = previous
            else:
                if current_repeat > 1 and current_repeat_start is not None:
                    repeat_clusters.append(
                        {
                            "start_index": current_repeat_start["index"],
                            "end_index": previous["index"],
                            "start_pts": current_repeat_start["pts"],
                            "end_pts": previous["pts"],
                            "frames": current_repeat,
                            "repeated_frames": current_repeat - 1,
                            "marker": previous["marker"],
                        }
                    )
                longest_repeat = max(longest_repeat, current_repeat)
                current_repeat = 1
                current_repeat_start = None
                delta = (frame["marker"] - previous["marker"]) & 0xFFFF
                if delta == 0 or delta > 0x8000:
                    out_of_order += 1
                    if len(out_of_order_details) < 16:
                        out_of_order_details.append(
                            {
                                "previous_index": previous["index"],
                                "index": frame["index"],
                                "previous_pts": previous["pts"],
                                "pts": frame["pts"],
                                "previous_marker": previous["marker"],
                                "marker": frame["marker"],
                                "delta": delta,
                                "previous_bad_tiles": previous.get("marker_bad_tiles", 0),
                                "bad_tiles": frame.get("marker_bad_tiles", 0),
                            }
                        )
            previous_in_planned_stall = time_in_source_stall(previous["pts"], source_stalls)
            if frame["motion"] is not None and previous_motion is not None:
                if abs(frame["motion"] - previous_motion) < 0.0005:
                    if in_planned_stall or previous_in_planned_stall or expected_source_duplicate:
                        longest_motion_stall = max(longest_motion_stall, motion_stall_run)
                        motion_stall_run = 1
                    else:
                        motion_stalls += 1
                        motion_stall_run += 1
                else:
                    longest_motion_stall = max(longest_motion_stall, motion_stall_run)
                    motion_stall_run = 1
            if fast_motion_available and fast_motion is not None and previous_fast_motion is not None:
                if abs(fast_motion - previous_fast_motion) < 0.0005:
                    if in_planned_stall or previous_in_planned_stall or expected_source_duplicate:
                        longest_fast_motion_stall = max(longest_fast_motion_stall, fast_motion_stall_run)
                        fast_motion_stall_run = 1
                    else:
                        fast_motion_stalls += 1
                        fast_motion_stall_run += 1
                else:
                    longest_fast_motion_stall = max(longest_fast_motion_stall, fast_motion_stall_run)
                    fast_motion_stall_run = 1
        previous = frame
        if frame["motion"] is not None:
            previous_motion = frame["motion"]
        if fast_motion is not None:
            previous_fast_motion = fast_motion
    longest_repeat = max(longest_repeat, current_repeat)
    if current_repeat > 1 and current_repeat_start is not None:
        repeat_clusters.append(
            {
                "start_index": current_repeat_start["index"],
                "end_index": frames[-1]["index"],
                "start_pts": current_repeat_start["pts"],
                "end_pts": frames[-1]["pts"],
                "frames": current_repeat,
                "repeated_frames": current_repeat - 1,
                "marker": frames[-1]["marker"],
            }
        )
    longest_motion_stall = max(longest_motion_stall, motion_stall_run)
    longest_fast_motion_stall = max(longest_fast_motion_stall, fast_motion_stall_run)
    (
        planned_clusters,
        source_fps_limited_clusters,
        unplanned_clusters,
        missing_planned_stalls,
    ) = classify_repeat_clusters(repeat_clusters, manifest, source_stalls, nominal_output_fps)
    transitions = build_video_transitions(frames, manifest)
    return {
        "timing": timing,
        "marker_timing_available": marker_timing_available,
        "frames": len(frames),
        "corrupt_frames": corrupt,
        "repeated_marker_frames": repeated,
        "longest_marker_repeat": longest_repeat,
        "repeat_clusters": repeat_clusters,
        "planned_source_stall_clusters": planned_clusters,
        "source_fps_limited_repeat_clusters": source_fps_limited_clusters,
        "unplanned_repeat_clusters": unplanned_clusters,
        "missing_planned_source_stalls": missing_planned_stalls,
        "longest_unplanned_marker_repeat": max((item["frames"] for item in unplanned_clusters), default=1),
        "unplanned_repeated_marker_frames": sum(item["repeated_frames"] for item in unplanned_clusters),
        "out_of_order_markers": out_of_order,
        "out_of_order_marker_details": out_of_order_details,
        "motion_missing_frames": motion_missing,
        "motion_stall_frames": motion_stalls,
        "longest_motion_stall": longest_motion_stall,
        "motion_error_frames": motion_error_frames,
        "motion_error_max": motion_error_max,
        "fast_motion_available": fast_motion_available,
        "fast_motion_missing_frames": fast_motion_missing,
        "fast_motion_stall_frames": fast_motion_stalls,
        "longest_fast_motion_stall": longest_fast_motion_stall if fast_motion_available else 1,
        "fast_motion_error_frames": fast_motion_error_frames,
        "fast_motion_error_max": fast_motion_error_max,
        "transitions": transitions,
        "source_stalls": source_stalls,
        "analysis_start_seconds": analysis_start,
        "audio_stimulus_lead_seconds": parse_float(manifest.get("audio_stimulus_lead_ms"), 0.0) / 1000.0,
        "nominal_output_fps": nominal_output_fps,
        "expected_source_repeat_run": source_repeat_run,
    }


def decode_audio_track(ffmpeg, capture, audio_ordinal):
    command = [
        ffmpeg,
        "-nostdin",
        "-v",
        "error",
        "-i",
        str(capture),
        "-map",
        f"0:a:{audio_ordinal}",
        "-ac",
        "1",
        "-acodec",
        "pcm_f32le",
        "-f",
        "f32le",
        "-",
    ]
    process = subprocess.Popen([str(part) for part in command], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    assert process.stdout is not None
    payload = process.stdout.read()
    stderr = b""
    if process.stderr is not None:
        stderr = process.stderr.read()
    returncode = process.wait()
    if returncode != 0:
        return None, stderr.decode("utf-8", errors="replace")
    samples = array.array("f")
    samples.frombytes(payload)
    return samples, ""


def goertzel_power(samples, start, length, sample_rate, frequency):
    coeff = 2.0 * math.cos(2.0 * math.pi * frequency / sample_rate)
    s0 = 0.0
    s1 = 0.0
    s2 = 0.0
    end = min(start + length, len(samples))
    for i in range(start, end):
        s0 = samples[i] + coeff * s1 - s2
        s2 = s1
        s1 = s0
    return s1 * s1 + s2 * s2 - coeff * s1 * s2
