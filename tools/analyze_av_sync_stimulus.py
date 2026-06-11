#!/usr/bin/env python3

import argparse
import array
import json
import math
import os
import re
import statistics
import subprocess
import sys
from pathlib import Path


STRICT_CE_PATTERNS = {
    "audio_trim": re.compile(
        r"\[PullAudio\](?:"
        r" WARNING: CFR audio headroom exhausted|"
        r" Audio latency cap:|"
        r" WGC overload sync trim:|"
        r" WGC CFR lead cap trim:|"
        r" Ring buffer overflow protection|"
        r" Track \d+ bootstrap complete .* trimmed=[1-9]"
        r")",
        re.IGNORECASE,
    ),
    "audio_underrun": re.compile(r"\[PullAudio\] WARNING: Source underrun", re.IGNORECASE),
    "audio_overflow": re.compile(r"\[PullAudio\] WARNING: Ring buffer overflow", re.IGNORECASE),
    "audio_extreme_drift": re.compile(r"\[PullAudio\] WARNING: Extreme drift detected", re.IGNORECASE),
    "wgc_stop_hold": re.compile(r"WGC CFR stop drain using held pre-stop frame", re.IGNORECASE),
    "wgc_drain_duplicate": re.compile(r"\[WGC CFR SUMMARY\].*drain=[1-9]", re.IGNORECASE),
}


def fail(message):
    print(f"error: {message}", file=sys.stderr)
    raise SystemExit(1)


def run_command(command, text=True):
    result = subprocess.run(
        [str(part) for part in command],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=text,
        encoding="utf-8" if text else None,
        errors="replace" if text else None,
        check=False,
    )
    if result.returncode != 0:
        stderr = result.stderr.strip() if text else result.stderr.decode("utf-8", errors="replace")
        fail(f"command failed ({result.returncode}): {' '.join(str(part) for part in command)}\n{stderr}")
    return result


def run_ffprobe_json(ffprobe, args):
    result = run_command([ffprobe, "-v", "error", *args, "-of", "json"])
    try:
        return json.loads(result.stdout)
    except json.JSONDecodeError as exc:
        fail(f"ffprobe returned invalid JSON: {exc}")


def load_manifest(path):
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except OSError as exc:
        fail(f"failed to read manifest {path}: {exc}")
    except json.JSONDecodeError as exc:
        fail(f"manifest is not valid JSON: {exc}")
    if data.get("schema") != "ce-avsync-stimulus-v1":
        fail(f"unsupported manifest schema: {data.get('schema')}")
    return data


def analyze_streams(ffprobe, capture):
    data = run_ffprobe_json(ffprobe, ["-show_streams", "-show_format", str(capture)])
    streams = data.get("streams", [])
    video = [stream for stream in streams if stream.get("codec_type") == "video"]
    audio = [stream for stream in streams if stream.get("codec_type") == "audio"]
    if not video:
        fail("capture has no video stream")
    return data.get("format", {}), video[0], audio


def parse_float(value, default=0.0):
    if value in (None, "", "N/A"):
        return default
    try:
        return float(value)
    except (TypeError, ValueError):
        return default


def parse_int(value, default=0):
    if value in (None, "", "N/A"):
        return default
    try:
        return int(value)
    except (TypeError, ValueError):
        return default


def parse_ratio(text):
    if not text or text == "0/0":
        return 0.0
    if "/" in text:
        num, den = text.split("/", 1)
        den_value = parse_float(den)
        return parse_float(num) / den_value if den_value else 0.0
    return parse_float(text)


def parse_ordinal_set(text):
    values = set()
    if not text:
        return values
    for item in str(text).split(","):
        item = item.strip()
        if not item:
            continue
        try:
            values.add(int(item))
        except ValueError:
            fail(f"invalid audio ordinal: {item}")
    return values


def read_video_pts(ffprobe, capture):
    data = run_ffprobe_json(
        ffprobe,
        [
            "-select_streams",
            "v:0",
            "-show_frames",
            "-show_entries",
            "frame=best_effort_timestamp_time,pkt_duration_time",
            str(capture),
        ],
    )
    frames = [frame for frame in data.get("frames", []) if isinstance(frame, dict)]
    pts = [parse_float(frame.get("best_effort_timestamp_time")) for frame in frames]
    durations = [parse_float(frame.get("pkt_duration_time")) for frame in frames]
    return pts, durations


def pixel(frame, width, height, x, y):
    sx = max(0, min(width - 1, int(round(x))))
    sy = max(0, min(height - 1, int(round(y))))
    offset = (sy * width + sx) * 3
    return frame[offset], frame[offset + 1], frame[offset + 2]


def luminance(rgb):
    return 0.2126 * rgb[0] + 0.7152 * rgb[1] + 0.0722 * rgb[2]


def nearest_palette_index(rgb, palette):
    best_index = 0
    best_dist = 1 << 60
    for index, color in enumerate(palette):
        dr = rgb[0] - color[0]
        dg = rgb[1] - color[1]
        db = rgb[2] - color[2]
        dist = dr * dr + dg * dg + db * db
        if dist < best_dist:
            best_dist = dist
            best_index = index
    return best_index, math.sqrt(best_dist)


def decode_marker(frame, scaled_w, scaled_h, manifest, inverse=False):
    src_w = max(1, int(manifest["width"]))
    src_h = max(1, int(manifest["height"]))
    tile = manifest["marker_tile"]
    margin = manifest["marker_margin"]
    gap = manifest["marker_gap"]
    bits = []
    bad = 0
    top = margin + (tile + gap if inverse else 0)
    for bit in range(int(manifest["frame_marker_bits"])):
        src_x = margin + bit * (tile + gap) + tile / 2.0
        src_y = top + tile / 2.0
        rgb = pixel(frame, scaled_w, scaled_h, src_x / src_w * scaled_w, src_y / src_h * scaled_h)
        lum = luminance(rgb)
        if 80.0 < lum < 176.0:
            bad += 1
        bits.append(lum >= 128.0)
    marker = 0
    for bit, value in enumerate(bits):
        if value ^ inverse:
            marker |= 1 << bit
    return marker, bad


def decode_checksum(frame, scaled_w, scaled_h, manifest, marker, palette_index):
    src_w = max(1, int(manifest["width"]))
    src_h = max(1, int(manifest["height"]))
    size = 28
    start_x = src_w - manifest["marker_margin"] - 8 * (size + 4)
    start_y = src_h - manifest["marker_margin"] - size
    observed = 0
    bad = 0
    for bit in range(8):
        src_x = start_x + bit * (size + 4) + size / 2.0
        src_y = start_y + size / 2.0
        lum = luminance(pixel(frame, scaled_w, scaled_h, src_x / src_w * scaled_w, src_y / src_h * scaled_h))
        if 80.0 < lum < 176.0:
            bad += 1
        if lum >= 128.0:
            observed |= 1 << bit
    expected = (marker & 0xFF) ^ ((marker >> 8) & 0xFF) ^ (palette_index if palette_index >= 0 else 0)
    return observed == expected, bad, observed, expected


def decode_motion(frame, scaled_w, scaled_h, manifest):
    src_w = max(1, int(manifest["width"]))
    src_h = max(1, int(manifest["height"]))
    y = int(round(0.72 * src_h + 21.0))
    scaled_y = max(0, min(scaled_h - 1, int(round(y / src_h * scaled_h))))
    xs = []
    for x in range(int(0.02 * scaled_w), int(0.98 * scaled_w)):
        if luminance(pixel(frame, scaled_w, scaled_h, x, scaled_y)) > 210.0:
            xs.append(x)
    if not xs:
        return None
    return statistics.mean(xs) / float(scaled_w)


def decode_video(ffmpeg, capture, manifest, video_stream, pts, scale_width):
    stream_w = parse_int(video_stream.get("width"))
    stream_h = parse_int(video_stream.get("height"))
    if stream_w <= 0 or stream_h <= 0:
        fail("video stream has invalid dimensions")
    if scale_width <= 0:
        manifest_w = max(1, int(manifest["width"]))
        marker_tile = max(1, int(manifest["marker_tile"]))
        min_marker_pixels = 8
        min_scale_for_markers = math.ceil(manifest_w * min_marker_pixels / marker_tile)
        scaled_w = min(stream_w, max(480, min_scale_for_markers))
    else:
        scaled_w = scale_width
    scaled_h = max(2, int(round(scaled_w * stream_h / stream_w)))
    if scaled_h % 2:
        scaled_h += 1

    command = [
        ffmpeg,
        "-nostdin",
        "-v",
        "error",
        "-i",
        str(capture),
        "-map",
        "0:v:0",
        "-an",
        "-sn",
        "-dn",
        "-vf",
        f"scale={scaled_w}:{scaled_h}:flags=fast_bilinear",
        "-pix_fmt",
        "rgb24",
        "-f",
        "rawvideo",
        "-",
    ]
    process = subprocess.Popen([str(part) for part in command], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    assert process.stdout is not None
    palette = [event["rgb"] for event in manifest["events"]]
    frame_size = scaled_w * scaled_h * 3
    frames = []
    frame_index = 0
    while True:
        payload = process.stdout.read(frame_size)
        if not payload:
            break
        if len(payload) != frame_size:
            break
        event_rgb = pixel(payload, scaled_w, scaled_h, scaled_w * 0.50, scaled_h * 0.45)
        palette_index, color_dist = nearest_palette_index(event_rgb, palette)
        marker, marker_bad = decode_marker(payload, scaled_w, scaled_h, manifest, inverse=False)
        inverse_marker, inverse_bad = decode_marker(payload, scaled_w, scaled_h, manifest, inverse=True)
        checksum_ok, checksum_bad, checksum_observed, checksum_expected = decode_checksum(
            payload, scaled_w, scaled_h, manifest, marker, palette_index
        )
        motion = decode_motion(payload, scaled_w, scaled_h, manifest)
        frames.append(
            {
                "index": frame_index,
                "pts": pts[frame_index] if frame_index < len(pts) else frame_index,
                "palette": palette_index,
                "color_dist": color_dist,
                "marker": marker,
                "inverse_marker": inverse_marker,
                "marker_bad_tiles": marker_bad + inverse_bad + checksum_bad,
                "marker_inverse_ok": marker == inverse_marker,
                "checksum_ok": checksum_ok,
                "checksum_observed": checksum_observed,
                "checksum_expected": checksum_expected,
                "motion": motion,
            }
        )
        frame_index += 1
    stderr = b""
    if process.stderr is not None:
        stderr = process.stderr.read()
    returncode = process.wait()
    if returncode != 0:
        fail(f"ffmpeg video decode failed: {stderr.decode('utf-8', errors='replace')}")
    return frames


def compress_states(samples, min_duration=0.08):
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
        transitions.append(
            {
                "from": prev["state"],
                "to": cur["state"],
                "time": (prev["end"] + cur["start"]) / 2.0,
            }
        )
    return segments, transitions


def summarize_video(frames):
    if not frames:
        return {"error": "no decoded video frames"}
    repeated = 0
    longest_repeat = 1
    current_repeat = 1
    out_of_order = 0
    corrupt = 0
    motion_missing = 0
    motion_stalls = 0
    motion_stall_run = 1
    longest_motion_stall = 1
    previous = None
    previous_motion = None
    for frame in frames:
        if (
            frame["marker_bad_tiles"] > 0
            or not frame["marker_inverse_ok"]
            or not frame["checksum_ok"]
            or frame["color_dist"] > 90.0
        ):
            corrupt += 1
        if frame["motion"] is None:
            motion_missing += 1
        if previous is not None:
            if frame["marker"] == previous["marker"]:
                repeated += 1
                current_repeat += 1
            else:
                longest_repeat = max(longest_repeat, current_repeat)
                current_repeat = 1
                delta = (frame["marker"] - previous["marker"]) & 0xFFFF
                if delta == 0 or delta > 0x8000:
                    out_of_order += 1
            if frame["motion"] is not None and previous_motion is not None:
                if abs(frame["motion"] - previous_motion) < 0.0005:
                    motion_stalls += 1
                    motion_stall_run += 1
                else:
                    longest_motion_stall = max(longest_motion_stall, motion_stall_run)
                    motion_stall_run = 1
        previous = frame
        if frame["motion"] is not None:
            previous_motion = frame["motion"]
    longest_repeat = max(longest_repeat, current_repeat)
    longest_motion_stall = max(longest_motion_stall, motion_stall_run)
    _, transitions = compress_states([(frame["pts"], frame["palette"]) for frame in frames], min_duration=0.12)
    return {
        "frames": len(frames),
        "corrupt_frames": corrupt,
        "repeated_marker_frames": repeated,
        "longest_marker_repeat": longest_repeat,
        "out_of_order_markers": out_of_order,
        "motion_missing_frames": motion_missing,
        "motion_stall_frames": motion_stalls,
        "longest_motion_stall": longest_motion_stall,
        "transitions": transitions,
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


def detect_audio_states(samples, sample_rate, frequencies):
    if not samples or sample_rate <= 0:
        return [], []
    window = max(1024, int(sample_rate * 0.045))
    step = max(256, int(sample_rate * 0.0125))
    states = []
    for start in range(0, max(0, len(samples) - window), step):
        chunk = samples[start : start + window]
        rms = math.sqrt(sum(float(value) * float(value) for value in chunk) / max(1, len(chunk)))
        time_value = (start + window / 2.0) / sample_rate
        if rms < 0.015:
            states.append((time_value, None))
            continue
        powers = [goertzel_power(samples, start, window, sample_rate, freq) for freq in frequencies]
        states.append((time_value, int(max(range(len(powers)), key=lambda index: powers[index]))))
    segments, transitions = compress_states(states, min_duration=0.10)
    return segments, refine_audio_transitions(samples, sample_rate, frequencies, transitions)


def refine_audio_transitions(samples, sample_rate, frequencies, transitions):
    refined = []
    if sample_rate <= 0 or not samples:
        return transitions
    window = max(512, int(sample_rate * 0.024))
    step = max(1, int(sample_rate * 0.001))
    for transition in transitions:
        from_index = transition["from"]
        to_index = transition["to"]
        coarse_time = transition["time"]
        if (
            from_index < 0
            or to_index < 0
            or from_index >= len(frequencies)
            or to_index >= len(frequencies)
            or from_index == to_index
        ):
            refined.append(transition)
            continue

        search_start = max(0, int((coarse_time - 0.16) * sample_rate))
        search_end = min(max(0, len(samples) - window), int((coarse_time + 0.06) * sample_rate))
        if search_end <= search_start:
            refined.append(transition)
            continue

        points = []
        for start in range(search_start, search_end + 1, step):
            from_power = goertzel_power(samples, start, window, sample_rate, frequencies[from_index])
            to_power = goertzel_power(samples, start, window, sample_rate, frequencies[to_index])
            center_time = (start + window / 2.0) / sample_rate
            points.append((center_time, to_power - from_power))
        if not points:
            refined.append(transition)
            continue

        crossing = None
        previous = points[0]
        for current in points[1:]:
            if previous[1] <= 0.0 <= current[1]:
                denom = current[1] - previous[1]
                fraction = 0.0 if denom == 0.0 else (-previous[1] / denom)
                crossing = previous[0] + fraction * (current[0] - previous[0])
                break
            previous = current
        if crossing is None:
            crossing = min(points, key=lambda item: abs(item[1]))[0]

        updated = dict(transition)
        updated["coarse_time"] = coarse_time
        updated["time"] = crossing
        refined.append(updated)
    return refined


def match_transition_offsets(reference, candidate, max_window):
    offsets = []
    missing = 0
    used = set()
    for item in candidate:
        choices = [
            (abs(item["time"] - ref["time"]), index, ref)
            for index, ref in enumerate(reference)
            if index not in used and ref["to"] == item["to"] and abs(item["time"] - ref["time"]) <= max_window
        ]
        if not choices:
            missing += 1
            continue
        _, index, ref = min(choices, key=lambda value: value[0])
        used.add(index)
        offsets.append(item["time"] - ref["time"])
    return offsets, missing


def analyze_ce_log_text(text):
    return {name: len(pattern.findall(text)) for name, pattern in STRICT_CE_PATTERNS.items()}


def analyze_ce_log(path):
    if not path:
        return {}
    return analyze_ce_log_text(path.read_text(encoding="utf-8", errors="replace"))


def make_check(name, passed, actual, expected, failure_class):
    return {
        "name": name,
        "passed": bool(passed),
        "actual": actual,
        "expected": expected,
        "failure_class": failure_class,
    }


def evaluate(args, video_summary, audio_results, ce_counts):
    checks = []
    non_strict_audio = getattr(args, "non_strict_audio_ordinals_set", set())
    checks.append(
        make_check(
            "video.corrupt_frames",
            video_summary["corrupt_frames"] <= args.max_corrupt_frames,
            video_summary["corrupt_frames"],
            f"<= {args.max_corrupt_frames}",
            "corrupted_video_frame",
        )
    )
    checks.append(
        make_check(
            "video.longest_marker_repeat",
            video_summary["longest_marker_repeat"] <= args.max_longest_repeat,
            video_summary["longest_marker_repeat"],
            f"<= {args.max_longest_repeat}",
            "repeat_cluster",
        )
    )
    checks.append(
        make_check(
            "video.longest_motion_stall",
            video_summary["longest_motion_stall"] <= args.max_motion_stall,
            video_summary["longest_motion_stall"],
            f"<= {args.max_motion_stall}",
            "visual_judder",
        )
    )
    checks.append(
        make_check(
            "video.out_of_order_markers",
            video_summary["out_of_order_markers"] == 0,
            video_summary["out_of_order_markers"],
            "0",
            "video_content_drift",
        )
    )
    checks.append(
        make_check(
            "video.event_transitions",
            len(video_summary["transitions"]) >= args.min_video_transitions,
            len(video_summary["transitions"]),
            f">= {args.min_video_transitions}",
            "missing_marker",
        )
    )

    video_transitions = video_summary["transitions"]
    audio_transition_sets = []
    for result in audio_results:
        is_strict_audio = result["ordinal"] not in non_strict_audio
        if result.get("decode_error"):
            checks.append(
                make_check(
                    f"audio.a:{result['ordinal']}.decode"
                    if is_strict_audio
                    else f"audio.a:{result['ordinal']}.opportunistic_decode",
                    not is_strict_audio,
                    result["decode_error"],
                    "no decode errors",
                    "decode_error" if is_strict_audio else "opportunistic_audio_decode",
                )
            )
            continue
        transitions = result["transitions"]
        if is_strict_audio:
            audio_transition_sets.append((result["ordinal"], transitions))
        checks.append(
            make_check(
                f"audio.a:{result['ordinal']}.markers"
                if is_strict_audio
                else f"audio.a:{result['ordinal']}.opportunistic_markers",
                (len(transitions) >= args.min_audio_transitions) if is_strict_audio else True,
                len(transitions),
                f">= {args.min_audio_transitions}",
                "audio_marker_missing" if is_strict_audio else "opportunistic_audio_marker",
            )
        )
        offsets, missing = match_transition_offsets(video_transitions, transitions, args.transition_match_window_ms / 1000.0)
        max_offset_ms = max((abs(offset) * 1000.0 for offset in offsets), default=999999.0 if transitions else 0.0)
        checks.append(
            make_check(
                f"audio.a:{result['ordinal']}.av_offset_ms"
                if is_strict_audio
                else f"audio.a:{result['ordinal']}.opportunistic_av_offset_ms",
                (offsets and missing <= args.max_missing_transition_matches and max_offset_ms <= args.max_av_offset_ms)
                if is_strict_audio
                else True,
                round(max_offset_ms, 3),
                f"<= {args.max_av_offset_ms} ms",
                "audio_video_event_offset" if is_strict_audio else "opportunistic_audio_video_event_offset",
            )
        )

    if len(audio_transition_sets) >= 2:
        reference_ordinal, reference = audio_transition_sets[0]
        max_spread_ms = 0.0
        missing_total = 0
        for ordinal, transitions in audio_transition_sets[1:]:
            offsets, missing = match_transition_offsets(reference, transitions, args.transition_match_window_ms / 1000.0)
            missing_total += missing
            max_spread_ms = max(max_spread_ms, max((abs(offset) * 1000.0 for offset in offsets), default=0.0))
        checks.append(
            make_check(
                "audio.inter_track_spread_ms",
                missing_total == 0 and max_spread_ms <= args.max_track_spread_ms,
                round(max_spread_ms, 3),
                f"<= {args.max_track_spread_ms} ms",
                "inter_track_spread",
            )
        )

    for name, count in sorted(ce_counts.items()):
        checks.append(make_check(f"ce_log.{name}", count == 0, count, "0", "ce_strict_log_event"))

    return checks


def print_report(report):
    print("avsync_stimulus:")
    print(f"  capture={report['capture']}")
    print(f"  manifest={report['manifest']}")
    print(
        "  video frames={frames} corrupt={corrupt} repeats={repeat} longest_repeat={longest} "
        "motion_stall={motion}".format(
            frames=report["video"]["frames"],
            corrupt=report["video"]["corrupt_frames"],
            repeat=report["video"]["repeated_marker_frames"],
            longest=report["video"]["longest_marker_repeat"],
            motion=report["video"]["longest_motion_stall"],
        )
    )
    for audio in report["audio"]:
        print(
            "  audio a:{ordinal} codec={codec} rate={rate} transitions={transitions} decode_error={decode_error}".format(
                ordinal=audio["ordinal"],
                codec=audio["codec"],
                rate=audio["sample_rate"],
                transitions=len(audio.get("transitions", [])),
                decode_error="yes" if audio.get("decode_error") else "no",
            )
        )
    print("checks:")
    for check in report["checks"]:
        status = "PASS" if check["passed"] else "FAIL"
        print(
            f"  {status} {check['name']}: actual={check['actual']} expected={check['expected']} "
            f"class={check['failure_class']}"
        )
    print(f"summary: passed={report['passed']}")


def self_test():
    reference = [{"to": 1, "time": 1.0}, {"to": 2, "time": 2.0}, {"to": 3, "time": 3.0}]
    candidate = [{"to": 1, "time": 1.012}, {"to": 2, "time": 2.015}, {"to": 3, "time": 2.990}]
    offsets, missing = match_transition_offsets(reference, candidate, 0.1)
    assert missing == 0
    assert round(max(abs(offset) for offset in offsets), 3) == 0.015
    segments, transitions = compress_states([(0.0, 1), (0.1, 1), (0.2, 2), (0.3, 2)], min_duration=0.05)
    assert len(segments) == 2
    assert transitions[0]["to"] == 2
    audio = array.array("f")
    phase = 0.0
    rate = 48000
    freqs = [440.0, 660.0]
    for sample_index in range(rate * 2):
        freq = freqs[0] if sample_index < rate else freqs[1]
        audio.append(0.25 * math.sin(phase))
        phase += 2.0 * math.pi * freq / rate
        if phase > 2.0 * math.pi:
            phase -= 2.0 * math.pi
    _, audio_transitions = detect_audio_states(audio, rate, freqs)
    assert audio_transitions and abs(audio_transitions[0]["time"] - 1.0) < 0.01
    assert "coarse_time" in audio_transitions[0]
    clean_counts = analyze_ce_log_text(
        "[PullAudio] Track 1 bootstrap complete - target=3ms samples=160 forced=0 trimmed=0 protected=1029\n"
        "[A/V SYNC CHECK] Track 1: RetainTrim=0, CoverageTrim=0, Tier2Trim=0, BootstrapTrim=0\n"
    )
    assert clean_counts["audio_trim"] == 0
    bad_counts = analyze_ce_log_text(
        "[PullAudio] Track 1 bootstrap complete - target=3ms samples=160 forced=0 trimmed=4 protected=1029\n"
        "[PullAudio] Audio latency cap: src 0 ahead by 1200 samples - trimming 240\n"
    )
    assert bad_counts["audio_trim"] == 2
    print("self-test: PASS")


def main():
    parser = argparse.ArgumentParser(description="Analyze dx12_av_sync_test captures against the stimulus manifest.")
    parser.add_argument("capture", nargs="?", type=Path)
    parser.add_argument("--manifest", type=Path)
    parser.add_argument("--ffmpeg", type=Path, default=Path("ffmpeg"))
    parser.add_argument("--ffprobe", type=Path, default=Path("ffprobe"))
    parser.add_argument("--ce-log", type=Path)
    parser.add_argument("--json-out", type=Path)
    parser.add_argument(
        "--video-scale-width",
        type=int,
        default=0,
        help="Decoded video width for marker analysis; 0 auto-scales so marker tiles remain readable.",
    )
    parser.add_argument("--max-av-offset-ms", type=float, default=80.0)
    parser.add_argument("--max-track-spread-ms", type=float, default=30.0)
    parser.add_argument("--transition-match-window-ms", type=float, default=300.0)
    parser.add_argument("--min-video-transitions", type=int, default=4)
    parser.add_argument("--min-audio-transitions", type=int, default=4)
    parser.add_argument("--max-missing-transition-matches", type=int, default=1)
    parser.add_argument("--max-corrupt-frames", type=int, default=0)
    parser.add_argument("--max-longest-repeat", type=int, default=2)
    parser.add_argument("--max-motion-stall", type=int, default=3)
    parser.add_argument(
        "--non-strict-audio-ordinals",
        default="",
        help="Comma-separated audio stream ordinals to report without making them pass/fail gates.",
    )
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()

    if args.self_test:
        self_test()
        return
    if not args.capture or not args.manifest:
        fail("capture and --manifest are required unless --self-test is used")
    args.non_strict_audio_ordinals_set = parse_ordinal_set(args.non_strict_audio_ordinals)

    manifest = load_manifest(args.manifest)
    _, video_stream, audio_streams = analyze_streams(args.ffprobe, args.capture)
    pts, _ = read_video_pts(args.ffprobe, args.capture)
    frames = decode_video(args.ffmpeg, args.capture, manifest, video_stream, pts, args.video_scale_width)
    video_summary = summarize_video(frames)
    if "error" in video_summary:
        fail(video_summary["error"])

    frequencies = [float(event["frequency_hz"]) for event in manifest["events"]]
    audio_results = []
    for ordinal, stream in enumerate(audio_streams):
        samples, decode_error = decode_audio_track(args.ffmpeg, args.capture, ordinal)
        sample_rate = parse_int(stream.get("sample_rate"))
        transitions = []
        if samples is not None:
            _, transitions = detect_audio_states(samples, sample_rate, frequencies)
        audio_results.append(
            {
                "ordinal": ordinal,
                "stream_index": parse_int(stream.get("index")),
                "codec": stream.get("codec_name", ""),
                "sample_rate": sample_rate,
                "transitions": transitions,
                "decode_error": decode_error,
                "strict": ordinal not in args.non_strict_audio_ordinals_set,
            }
        )

    ce_counts = analyze_ce_log(args.ce_log) if args.ce_log else {}
    checks = evaluate(args, video_summary, audio_results, ce_counts)
    report = {
        "capture": str(args.capture),
        "manifest": str(args.manifest),
        "video": video_summary,
        "audio": audio_results,
        "ce_log_counts": ce_counts,
        "checks": checks,
        "passed": all(check["passed"] for check in checks),
    }
    print_report(report)
    if args.json_out:
        args.json_out.write_text(json.dumps(report, indent=2), encoding="utf-8")
    if not report["passed"]:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
