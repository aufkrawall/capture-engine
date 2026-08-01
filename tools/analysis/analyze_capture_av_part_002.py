

def safe_pstdev(values):
    return statistics.pstdev(values) if len(values) > 1 else 0.0


def format_seconds(value):
    return f"{value:.6f}s"


def format_metric(value):
    if isinstance(value, int):
        return str(value)
    if isinstance(value, float):
        return f"{value:.6f}" if abs(value) < 1.0 else f"{value:.3f}"
    return str(value)


def parse_named_int_thresholds(entries, valid_names, option_name):
    thresholds = {}
    for entry in entries:
        if "=" not in entry:
            fail(f"{option_name} expects NAME=VALUE entries, got: {entry}")
        name, value_text = entry.split("=", 1)
        name = name.strip()
        value_text = value_text.strip()
        if name not in valid_names:
            fail(
                f"unknown {option_name} name '{name}'. Valid names: {', '.join(sorted(valid_names))}"
            )
        try:
            value = int(value_text)
        except ValueError:
            fail(f"invalid integer value for {option_name} {name}: {value_text}")
        thresholds[name] = value
    return thresholds


def make_upper_bound_check(name, actual, limit, unit="", tolerance=0.0):
    suffix = f" {unit}" if unit else ""
    return {
        "name": name,
        "passed": actual <= limit + tolerance,
        "actual": f"{format_metric(actual)}{suffix}",
        "expected": f"<= {format_metric(limit)}{suffix}",
    }


def make_lower_bound_check(name, actual, limit, unit=""):
    suffix = f" {unit}" if unit else ""
    return {
        "name": name,
        "passed": actual >= limit,
        "actual": f"{format_metric(actual)}{suffix}",
        "expected": f">= {format_metric(limit)}{suffix}",
    }


def print_checks(checks):
    print("checks:")
    if not checks:
        print("  none")
        return
    for check in checks:
        status = "PASS" if check["passed"] else "FAIL"
        print(
            "  {status} {name}: actual={actual} expected={expected}".format(
                status=status,
                name=check["name"],
                actual=check["actual"],
                expected=check["expected"],
            )
        )


def evaluate_thresholds(args, nominal_fps, video_timing, duplicate_runs, audio_duration_spread, video_audio_max_delta,
                        log_summary):
    checks = []

    mean_frame_delta_error_us = None
    if video_timing["frame_count"] > 1 and nominal_fps > 0.0:
        expected_delta = 1.0 / nominal_fps
        mean_frame_delta_error_us = abs(video_timing["delta_mean"] - expected_delta) * 1_000_000.0

    if args.max_mean_frame_delta_error_us is not None:
        if mean_frame_delta_error_us is None:
            fail("--max-mean-frame-delta-error-us requires valid video timing with at least 2 frames and nominal FPS")
        checks.append(
            make_upper_bound_check(
                "mean_frame_delta_error", mean_frame_delta_error_us, args.max_mean_frame_delta_error_us, "us"
            )
        )

    if args.max_audio_spread_ms is not None:
        checks.append(
            make_upper_bound_check("audio_duration_spread", audio_duration_spread * 1000.0, args.max_audio_spread_ms,
                                   "ms", tolerance=0.0005)
        )

    if args.max_video_audio_delta_ms is not None:
        checks.append(
            make_upper_bound_check(
                "max_video_audio_duration_delta", video_audio_max_delta * 1000.0, args.max_video_audio_delta_ms,
                "ms", tolerance=0.0005
            )
        )

    if args.max_longest_duplicate_run is not None:
        if duplicate_runs is None:
            fail("--max-longest-duplicate-run requires --framehash")
        checks.append(
            make_upper_bound_check(
                "longest_duplicate_run", duplicate_runs["longest_run"], args.max_longest_duplicate_run, "frames"
            )
        )

    if args.max_repeated_frames is not None:
        if duplicate_runs is None:
            fail("--max-repeated-frames requires --framehash")
        checks.append(
            make_upper_bound_check(
                "repeated_frame_count", duplicate_runs["repeated_frame_count"], args.max_repeated_frames, "frames"
            )
        )

    log_event_thresholds = parse_named_int_thresholds(args.max_log_event, LOG_PATTERNS.keys(), "--max-log-event")
    if args.strict_sync_events:
        for name in STRICT_SYNC_LOG_EVENTS:
            log_event_thresholds.setdefault(name, 0)
    if log_event_thresholds and log_summary is None:
        fail("--max-log-event requires --log")
    for name, limit in sorted(log_event_thresholds.items()):
        checks.append(make_upper_bound_check(f"log.{name}", log_summary["counts"].get(name, 0), limit, "count"))

    cadence_metric_values = {
        "age_max_us": 0 if log_summary is None else log_summary["max_age_max_us"],
        "sel_miss": 0 if log_summary is None else log_summary["max_sel_miss"],
        "stale_unique": 0 if log_summary is None else log_summary["max_stale_unique"],
        "ancient": 0 if log_summary is None else log_summary["max_ancient"],
        "rep_no_fresh": 0 if log_summary is None else log_summary["max_rep_no_fresh"],
        "wgc_sel_bias_abs_us": 0 if log_summary is None else log_summary["max_wgc_sel_bias_abs_us"],
        "wgc_shortfall_ms": 0 if log_summary is None else log_summary["max_wgc_shortfall_ms"],
        "wgc_lead_excess_ms": 0 if log_summary is None else log_summary["max_wgc_lead_excess_ms"],
        "wgc_oldest_ms": 0 if log_summary is None else log_summary["max_wgc_oldest_ms"],
        "wgc_buffered_frames": 0 if log_summary is None else log_summary["max_wgc_buffered_frames"],
        "wgc_live_rebase_max_ticks": 0 if log_summary is None else log_summary["max_wgc_live_rebase_ticks"],
        "wgc_startup_frame_age_us": 0 if log_summary is None else log_summary["max_wgc_startup_frame_age_us"],
        "wgc_encoder_limited_drops": 0 if log_summary is None else log_summary["max_wgc_encoder_limited_drops"],
        "wgc_phase_error_us": 0 if log_summary is None else log_summary["max_wgc_phase_error_us"],
    }
    cadence_metric_thresholds = parse_named_int_thresholds(
        args.max_cadence_metric, cadence_metric_values.keys(), "--max-cadence-metric"
    )
    if cadence_metric_thresholds and log_summary is None:
        fail("--max-cadence-metric requires --log")
    for name, limit in sorted(cadence_metric_thresholds.items()):
        checks.append(make_upper_bound_check(f"cadence.{name}", cadence_metric_values[name], limit, "count"))

    return checks, mean_frame_delta_error_us


def analyze_streams(ffprobe, capture_path):
    data = run_ffprobe_json(
        ffprobe,
        [
            "-show_streams",
            "-show_format",
            str(capture_path),
        ],
    )
    if not isinstance(data, dict):
        fail("ffprobe stream output was not a JSON object")
    streams = data.get("streams", [])
    if not isinstance(streams, list):
        streams = []
    format_info = data.get("format", {})
    if not isinstance(format_info, dict):
        format_info = {}
    typed_streams = [stream for stream in streams if isinstance(stream, dict)]
    video_streams = [stream for stream in typed_streams if stream.get("codec_type") == "video"]
    audio_streams = [stream for stream in typed_streams if stream.get("codec_type") == "audio"]
    if not video_streams:
        fail("capture has no video stream")
    return format_info, video_streams, audio_streams


def summarize_cfr_packet_coverage(packet_pts, packet_durations, nominal_fps):
    packet_count = len(packet_pts)
    if packet_count == 0 or nominal_fps <= 0.0:
        return {
            "packet_count": packet_count,
            "expected_packets": 0,
            "missing_packets": 0,
            "max_gap_ticks": 0.0,
            "first_pts": 0.0,
            "last_pts": 0.0,
            "packet_end": 0.0,
            "span": 0.0,
            "complete": False,
        }
    # ffprobe reports packets in decode/mux order; codecs with B-frame
    # reordering can therefore have non-monotonic PTS in that order.
    ordered_pts = sorted(packet_pts)
    frame_duration = 1.0 / nominal_fps
    last_duration = packet_durations[-1] if packet_durations else 0.0
    if last_duration <= 0.0:
        last_duration = frame_duration
    span = max(0.0, ordered_pts[-1] + last_duration - ordered_pts[0])
    expected_packets = max(1, int(round(span * nominal_fps)))
    gaps = [ordered_pts[index] - ordered_pts[index - 1] for index in range(1, packet_count)]
    max_gap_ticks = (max(gaps) * nominal_fps) if gaps else 1.0
    missing_packets = max(0, expected_packets - packet_count)
    complete = missing_packets == 0 and max_gap_ticks <= 1.01
    return {
        "packet_count": packet_count,
        "expected_packets": expected_packets,
        "missing_packets": missing_packets,
        "max_gap_ticks": max_gap_ticks,
        "first_pts": ordered_pts[0],
        "last_pts": ordered_pts[-1],
        "packet_end": ordered_pts[-1] + last_duration,
        "span": span,
        "complete": complete,
    }


def analyze_cfr_packet_coverage(ffprobe, capture_path, nominal_fps):
    data = run_ffprobe_json(
        ffprobe,
        [
            "-select_streams",
            "v:0",
            "-show_packets",
            "-show_entries",
            "packet=pts_time,duration_time",
            str(capture_path),
        ],
    )
    packets = data.get("packets", []) if isinstance(data, dict) else []
    typed_packets = [packet for packet in packets if isinstance(packet, dict) and "pts_time" in packet]
    pts = [parse_float(packet.get("pts_time")) for packet in typed_packets]
    durations = [parse_float(packet.get("duration_time")) for packet in typed_packets]
    return summarize_cfr_packet_coverage(pts, durations, nominal_fps)


def analyze_video_timing(ffprobe, capture_path, read_interval=None, nominal_fps=0.0):
    args = [
        "-select_streams",
        "v:0",
        "-show_frames",
        "-show_entries",
        "frame=best_effort_timestamp_time,pkt_duration_time",
    ]
    if read_interval:
        args.extend(["-read_intervals", read_interval])
    args.append(str(capture_path))
    frame_data = run_ffprobe_json(ffprobe, args)
    if not isinstance(frame_data, dict):
        fail("ffprobe video frame output was not a JSON object")
    frames = frame_data.get("frames", [])
    if not isinstance(frames, list):
        frames = []
    typed_frames = [frame for frame in frames if isinstance(frame, dict)]
    pts = [
        parse_float(frame.get("best_effort_timestamp_time"))
        for frame in typed_frames
        if "best_effort_timestamp_time" in frame
    ]
    if not pts:
        return {
            "frame_count": 0,
            "first_pts": 0.0,
            "last_pts": 0.0,
            "frame_end": 0.0,
            "duration": 0.0,
            "delta_histogram": collections.Counter(),
            "delta_mean": 0.0,
            "delta_min": 0.0,
            "delta_max": 0.0,
            "delta_stdev": 0.0,
        }

    deltas = [round(pts[i] - pts[i - 1], 6) for i in range(1, len(pts))]
    durations = [parse_float(frame.get("pkt_duration_time")) for frame in typed_frames]
    last_duration = durations[-1] if durations else 0.0
    if last_duration <= 0.0 and nominal_fps > 0.0:
        last_duration = 1.0 / nominal_fps
    elif last_duration <= 0.0 and deltas:
        last_duration = deltas[-1]
    frame_end = pts[-1] + last_duration
    return {
        "source": "full-scan",
        "frame_count": len(pts),
        "first_pts": pts[0],
        "last_pts": pts[-1],
        "frame_end": frame_end,
        "duration": max(frame_end - pts[0], 0.0),
        "delta_histogram": collections.Counter(deltas),
        "delta_mean": safe_mean(deltas),
        "delta_min": min(deltas) if deltas else 0.0,
        "delta_max": max(deltas) if deltas else 0.0,
        "delta_stdev": safe_pstdev(deltas),
    }


def analyze_video_stream_metadata(stream_info, format_duration):
    nominal_fps = parse_ratio(stream_info.get("avg_frame_rate") or stream_info.get("r_frame_rate"))
    start_time = parse_float(stream_info.get("start_time"))
    duration = parse_float(stream_info.get("duration"))
    if duration <= 0.0:
        duration = max(format_duration - start_time, 0.0)
    frame_count = parse_int(stream_info.get("nb_frames"))
    if frame_count <= 0 and nominal_fps > 0.0 and duration > 0.0:
        frame_count = max(int(round(duration * nominal_fps)), 0)
    delta = (1.0 / nominal_fps) if nominal_fps > 0.0 else 0.0
    delta_histogram = (
        collections.Counter({round(delta, 6): max(frame_count - 1, 0)})
        if delta > 0.0
        else collections.Counter()
    )
    last_pts = max(start_time + duration - delta, start_time) if frame_count > 0 else start_time
    return {
        "source": "stream-metadata",
        "frame_count": frame_count,
        "first_pts": start_time,
        "last_pts": last_pts,
        "frame_end": start_time + duration,
        "duration": duration,
        "delta_histogram": delta_histogram,
        "delta_mean": delta,
        "delta_min": delta,
        "delta_max": delta,
        "delta_stdev": 0.0,
    }


def analyze_video_duplicate_runs(ffmpeg, capture_path, scale_width):
    result = run_command(
        [
            str(ffmpeg),
            "-v",
            "error",
            "-i",
            str(capture_path),
            "-map",
            "0:v:0",
            "-an",
            "-sn",
            "-dn",
            "-vf",
            f"scale={scale_width}:-2:flags=fast_bilinear,format=gray",
            "-f",
            "framemd5",
            "-",
        ]
    )
    hashes = []
    for line in result.stdout.splitlines():
        if not line or line.startswith("#"):
            continue
        parts = [part.strip() for part in line.split(",")]
        if len(parts) < 6:
            continue
        hashes.append(parts[-1])

    run_lengths = []
    if hashes:
        current_run = 1
        for index in range(1, len(hashes)):
            if hashes[index] == hashes[index - 1]:
                current_run += 1
            else:
                run_lengths.append(current_run)
                current_run = 1
        run_lengths.append(current_run)

    repeated_runs = [run for run in run_lengths if run > 1]
    return {
        "framehash_count": len(hashes),
        "run_count": len(run_lengths),
        "repeated_run_count": len(repeated_runs),
        "repeated_frame_count": sum(run - 1 for run in repeated_runs),
        "longest_run": max(run_lengths) if run_lengths else 0,
        "repeated_histogram": collections.Counter(repeated_runs),
    }


def analyze_audio_stream(ffprobe, capture_path, audio_ordinal, stream_info, read_interval=None):
    args = [
        "-select_streams",
        f"a:{audio_ordinal}",
        "-show_frames",
        "-show_entries",
        "frame=nb_samples,best_effort_timestamp_time,pkt_duration_time",
    ]
    if read_interval:
        args.extend(["-read_intervals", read_interval])
    args.append(str(capture_path))
    frame_data = run_ffprobe_json(ffprobe, args)
    if not isinstance(frame_data, dict):
        fail("ffprobe audio frame output was not a JSON object")
    frames = frame_data.get("frames", [])
    if not isinstance(frames, list):
        frames = []
    typed_frames = [frame for frame in frames if isinstance(frame, dict)]
    sample_rate = parse_int(stream_info.get("sample_rate"))
    total_samples = 0
    first_pts = None
    last_pts = 0.0
    last_duration = 0.0
    for frame in typed_frames:
        total_samples += parse_int(frame.get("nb_samples"))
        if first_pts is None and "best_effort_timestamp_time" in frame:
            first_pts = parse_float(frame.get("best_effort_timestamp_time"))
        if "best_effort_timestamp_time" in frame:
            last_pts = parse_float(frame.get("best_effort_timestamp_time"))
        if "pkt_duration_time" in frame:
            last_duration = parse_float(frame.get("pkt_duration_time"))
    decoded_duration = (total_samples / sample_rate) if sample_rate > 0 else 0.0
    frame_end = last_pts + last_duration if typed_frames else 0.0
    return {
        "source": "full-scan",
        "audio_ordinal": audio_ordinal,
        "stream_index": parse_int(stream_info.get("index")),
        "codec": stream_info.get("codec_name", ""),
        "sample_rate": sample_rate,
        "channels": parse_int(stream_info.get("channels")),
        "sample_total": total_samples,
        "decoded_duration": decoded_duration,
        "frame_start": first_pts if first_pts is not None else 0.0,
        "frame_end": frame_end,
    }


def analyze_audio_stream_metadata(audio_ordinal, stream_info, format_duration):
    sample_rate = parse_int(stream_info.get("sample_rate"))
    duration = parse_float(stream_info.get("duration"))
    start_time = parse_float(stream_info.get("start_time"))
    if duration <= 0.0:
        duration = max(format_duration - start_time, 0.0)
    duration_ts = parse_int(stream_info.get("duration_ts"))
    time_base = parse_ratio(stream_info.get("time_base"))
    sample_total = 0
    if sample_rate > 0:
        if duration_ts > 0 and time_base > 0.0:
            sample_total = int(round(duration_ts * time_base * sample_rate))
        elif duration > 0.0:
            sample_total = int(round(duration * sample_rate))
    return {
        "source": "stream-metadata",
        "audio_ordinal": audio_ordinal,
        "stream_index": parse_int(stream_info.get("index")),
        "codec": stream_info.get("codec_name", ""),
        "sample_rate": sample_rate,
        "channels": parse_int(stream_info.get("channels")),
        "sample_total": sample_total,
        "decoded_duration": duration,
        "frame_start": start_time,
        "frame_end": start_time + duration,
    }


def analyze_audio_decode(ffmpeg, capture_path, audio_ordinal):
    result = subprocess.run(
        [
            str(ffmpeg),
            "-nostdin",
            "-v",
            "error",
            "-i",
            str(capture_path),
            "-map",
            f"0:a:{audio_ordinal}",
            "-f",
            "null",
            os.devnull,
        ],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        encoding="utf-8",
        errors="replace",
        check=False,
    )
    actionable_stderr, ignored_stderr = split_decoder_stderr(result.stderr)
    return {
        "audio_ordinal": audio_ordinal,
        "returncode": result.returncode,
        "stderr": actionable_stderr,
        "ignored_environment_stderr": ignored_stderr,
    }


def compute_audio_signature_stride(
    sample_rate, stream_info, fallback_duration=0.0, target_rate=250.0, sample_budget=500000
):
    if sample_rate <= 0:
        return 1

    duration = max(parse_float(stream_info.get("duration")), max(0.0, fallback_duration))
    duration_ts = parse_int(stream_info.get("duration_ts"))
    time_base = parse_ratio(stream_info.get("time_base"))
    if duration_ts > 0 and time_base > 0.0:
        duration = max(duration, duration_ts * time_base)

    rate_stride = max(1, int(math.ceil(sample_rate / max(1.0, target_rate))))
    estimated_samples = max(0, int(math.ceil(duration * sample_rate)))
    budget_stride = (
        max(1, int(math.ceil(estimated_samples / max(1, sample_budget)))) if estimated_samples > 0 else 1
    )
    return max(rate_stride, budget_stride)


def analyze_audio_tail_marker(
    ffmpeg, capture_path, audio_ordinal, stream_info, threshold, fallback_duration=0.0
):
    sample_rate = parse_int(stream_info.get("sample_rate"))
    channels = max(1, parse_int(stream_info.get("channels"), 1))
    if sample_rate <= 0:
        return {
            "audio_ordinal": audio_ordinal,
            "sample_rate": sample_rate,
            "channels": channels,
            "samples": 0,
            "last_marker_sample": None,
            "last_marker_time": None,
            "tail_silence_ms": None,
            "stderr": "invalid sample rate",
            "returncode": 1,
        }

    command = [
        str(ffmpeg),
        "-nostdin",
        "-v",
        "error",
        "-i",
        str(capture_path),
        "-map",
        f"0:a:{audio_ordinal}",
        "-acodec",
        "pcm_f32le",
        "-f",
        "f32le",
        "-",
    ]
    process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    frame_bytes = channels * 4
    total_samples = 0
    first_marker_sample = None
    last_marker_sample = None
    peak_sample = 0.0
    clipping_samples = 0
    silent_samples = 0
    longest_silence_samples = 0
    current_silence_samples = 0
    discontinuities = 0
    previous_frame = None
    identical_channel_frames = 0
    # Preserve a bounded, uniformly sampled RMS-envelope signature across the whole stream.
    # The envelope survives codec and mix differences far better than picking one aliased raw
    # sample per interval. The old 1 kHz/120k-point prefix also silently stopped at 120 seconds,
    # so a later source join/restart could be badly out of sync while only the healthy opening
    # was correlated. array('f') keeps the full-duration evidence small.
    signature = array.array("f")
    signature_stride = compute_audio_signature_stride(sample_rate, stream_info, fallback_duration)
    signature_limit = 500000
    signature_truncated = False
    signature_energy = 0.0
    signature_energy_frames = 0
    pending = b""
    assert process.stdout is not None
    while True:
        chunk = process.stdout.read(1 << 18)
        if not chunk:
            break
        pending += chunk
        usable = (len(pending) // frame_bytes) * frame_bytes
        if usable <= 0:
            continue
        payload = pending[:usable]
        pending = pending[usable:]
        values = array.array("f")
        values.frombytes(payload)
        if sys.byteorder != "little":
            values.byteswap()
        frames = usable // frame_bytes
        for frame in range(frames):
            base = frame * channels
            channel_values = values[base : base + channels]
            peak = max(abs(value) for value in channel_values)
            peak_sample = max(peak_sample, peak)
            clipping_samples += sum(1 for value in channel_values if abs(value) >= 1.0)
            if peak > threshold:
                if first_marker_sample is None:
                    first_marker_sample = total_samples + frame
                last_marker_sample = total_samples + frame
                current_silence_samples = 0
            else:
                silent_samples += 1
                current_silence_samples += 1
                longest_silence_samples = max(longest_silence_samples, current_silence_samples)
            if previous_frame is not None and any(
                abs(channel_values[channel] - previous_frame[channel]) > 1.5 for channel in range(channels)
            ):
                discontinuities += 1
            previous_frame = channel_values
            if channels > 1 and all(
                abs(channel_values[channel] - channel_values[0]) <= 1e-7 for channel in range(1, channels)
            ):
                identical_channel_frames += 1
            signature_energy += sum(value * value for value in channel_values) / channels
            signature_energy_frames += 1
            if signature_energy_frames >= signature_stride:
                if len(signature) < signature_limit:
                    signature.append(math.sqrt(signature_energy / signature_energy_frames))
                else:
                    signature_truncated = True
                signature_energy = 0.0
                signature_energy_frames = 0
        total_samples += frames

    if signature_energy_frames > 0:
        if len(signature) < signature_limit:
            signature.append(math.sqrt(signature_energy / signature_energy_frames))
        else:
            signature_truncated = True

    stderr = b""
    if process.stderr is not None:
        stderr = process.stderr.read()
    returncode = process.wait()
    if total_samples > 0 and last_marker_sample is not None:
        tail_silence_ms = (total_samples - 1 - last_marker_sample) * 1000.0 / sample_rate
        last_marker_time = (last_marker_sample + 1) / sample_rate
    else:
        tail_silence_ms = None
        last_marker_time = None
    actionable_stderr, ignored_stderr = split_decoder_stderr(stderr.decode("utf-8", errors="replace"))
    return {
        "audio_ordinal": audio_ordinal,
        "sample_rate": sample_rate,
        "channels": channels,
        "samples": total_samples,
        "first_marker_sample": first_marker_sample,
        "last_marker_sample": last_marker_sample,
        "last_marker_time": last_marker_time,
        "tail_silence_ms": tail_silence_ms,
        "stderr": actionable_stderr,
        "ignored_environment_stderr": ignored_stderr,
        "returncode": returncode,
        "peak": peak_sample,
        "clipping_samples": clipping_samples,
        "silent_samples": silent_samples,
        "longest_silence_samples": longest_silence_samples,
        "discontinuities": discontinuities,
        "identical_channel_frames": identical_channel_frames,
        "signature_rate": sample_rate / signature_stride,
        "signature_stride": signature_stride,
        "signature_complete": not signature_truncated,
        "signature_coverage_seconds": min(total_samples, len(signature) * signature_stride) / sample_rate,
        "signature": signature,
    }


def signature_window_variance(signature, start, end):
    count = max(0, end - start)
    if count < 2:
        return 0.0
    total = 0.0
    total_squares = 0.0
    for index in range(start, end):
        value = signature[index]
        total += value
        total_squares += value * value
    return max(0.0, (total_squares - (total * total / count)) / count)


def normalized_signature_correlation(left, right, start, end, lag, step=1):
    if lag >= 0:
        left_start = start
        right_start = start + lag
    else:
        left_start = start - lag
        right_start = start
    available = min(end - left_start, end - right_start)
    if available <= 0:
        return None

    step = max(1, step)
    count = (available + step - 1) // step
    if count < 32:
        return None
    left_sum = 0.0
    right_sum = 0.0
    left_squares = 0.0
    right_squares = 0.0
    products = 0.0
    for offset in range(0, available, step):
        left_value = left[left_start + offset]
        right_value = right[right_start + offset]
        left_sum += left_value
        right_sum += right_value
        left_squares += left_value * left_value
        right_squares += right_value * right_value
        products += left_value * right_value

    numerator = products - (left_sum * right_sum / count)
    left_energy = left_squares - (left_sum * left_sum / count)
    right_energy = right_squares - (right_sum * right_sum / count)
    denominator = math.sqrt(max(0.0, left_energy) * max(0.0, right_energy))
    return numerator / denominator if denominator > 0.0 else None
