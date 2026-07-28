#!/usr/bin/env python3

import argparse
import array
import json
import math
import re
import statistics
import subprocess
import sys
import bisect
from pathlib import Path


FAST_MOTION_ISOLATED_DECODE_MISSING_RATIO = 0.015

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
    "audio_underrun": re.compile(r"\[PullAudio\] WARNING: Source underrun(?!.*forceDrain=1)", re.IGNORECASE),
    "audio_stop_tail_padding": re.compile(r"\[PullAudio\] WARNING: Source underrun.*forceDrain=1", re.IGNORECASE),
    "audio_strict_source_padding": re.compile(
        r"\[STOP AUDIO\] Source \d+: track=(?:1|2)\b.* pad:[1-9]\d*", re.IGNORECASE
    ),
    "audio_overflow": re.compile(r"\[PullAudio\] WARNING: Ring buffer overflow", re.IGNORECASE),
    "audio_extreme_drift": re.compile(r"\[PullAudio\] WARNING: Extreme drift detected", re.IGNORECASE),
    "audio_stop_force_drain_backlog": re.compile(
        r"\[PullAudio\] Stop force-drain backlog:", re.IGNORECASE
    ),
    "audio_late_app_source_backlog": re.compile(
        r"\[PullAudio\] Source primed .*lateStart=(?:[1-9]\d{3,}|\d{5,})ms", re.IGNORECASE
    ),
    "audio_app_source_gap_silence": re.compile(r"\[PullAudio\] App source gap silence", re.IGNORECASE),
    "audio_zero_drift_residual": re.compile(r"\[A/V ZERO DRIFT WARNING\]", re.IGNORECASE),
    "post_mux_probe_hang": re.compile(r"writer_finalize_timeout.*phase=post_mux_probe", re.IGNORECASE),
    "wgc_stop_hold": re.compile(r"WGC CFR stop drain using held pre-stop frame", re.IGNORECASE),
    "wgc_drain_duplicate": re.compile(r"\[WGC CFR SUMMARY\].*drain=[1-9]", re.IGNORECASE),
    "wgc_encoder_overload_policy": re.compile(
        r"encoder-limited mode mismatch|selected source backtrack blocked", re.IGNORECASE
    ),
}

LATE_APP_LIVE_JOIN_SRC_RE = re.compile(r"\[AudioLoop\] Late app source live join src=(\d+)", re.IGNORECASE)
LATE_APP_PRIMED_SRC_RE = re.compile(
    r"\[PullAudio\] Source primed\s+-\s+src=(\d+).*?lateStart=(\d+)ms(?:\s+app=([01]))?",
    re.IGNORECASE,
)
STOP_AUDIO_SOURCE_TYPE_RE = re.compile(
    r"\[STOP AUDIO\] Source (\d+):.*?\bprocess=([^\s]+)", re.IGNORECASE
)
APP_DRAIN_STATE_RE = re.compile(
    r"\[AppDrain\] state src=(\d+).*?\bforceDrain=([01])", re.IGNORECASE
)
AUDIO_EXTREME_DRIFT_SRC_RE = re.compile(
    r"\[PullAudio\] WARNING: Extreme drift detected \([^)]*?\bsrc=(\d+)\)"
    r"(?:.*?\bforceDrain=([01]))?",
    re.IGNORECASE,
)

STRICT_APP_PATTERNS = {
    "frame_pacing_spike": re.compile(r"AVSYNC WARNING frame pacing spike", re.IGNORECASE),
    "audio_event_timeout": re.compile(r"AVSYNC WARNING audio event timeout", re.IGNORECASE),
    "audio_renderer_failed": re.compile(r"AVSYNC WARNING audio renderer failed", re.IGNORECASE),
    "present_failed": re.compile(r"AVSYNC WARNING Present failed", re.IGNORECASE),
}

APP_FRAME_RE = re.compile(
    r"\bAVSYNC FRAME frameId=(?P<frame_id>\d+) marker=(?P<marker>\d+) "
    r"event=(?P<event>-?\d+) palette=(?P<palette>-?\d+) "
    r"stimulusSeconds=(?P<stimulus>-?\d+(?:\.\d+)?)"
)
APP_STALL_BEGIN_RE = re.compile(
    r"\bAVSYNC SOURCE_STALL_BEGIN .*stimulusSeconds=(?P<stimulus>-?\d+(?:\.\d+)?) "
    r"frameId=(?P<frame_id>\d+)"
)
APP_STALL_END_RE = re.compile(
    r"\bAVSYNC SOURCE_STALL_END .*stimulusSeconds=(?P<stimulus>-?\d+(?:\.\d+)?) "
    r".*frameId=(?P<frame_id>\d+)"
)

MAX_AUDIO_REFINEMENT_DELTA_SECONDS = 0.012


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


def load_app_frame_anchors(path):
    if not path:
        return []
    try:
        text = Path(path).read_text(encoding="utf-8", errors="replace")
    except OSError:
        return []

    anchors = []
    for line in text.splitlines():
        match = APP_FRAME_RE.search(line)
        if match:
            anchors.append(
                {
                    "frame_id": parse_int(match.group("frame_id")),
                    "stimulus_seconds": parse_float(match.group("stimulus")),
                    "kind": "frame",
                    "order": 0,
                }
            )
            continue
        match = APP_STALL_BEGIN_RE.search(line)
        if match:
            anchors.append(
                {
                    "frame_id": parse_int(match.group("frame_id")),
                    "stimulus_seconds": parse_float(match.group("stimulus")),
                    "kind": "source_stall_begin",
                    "order": 1,
                }
            )
            continue
        match = APP_STALL_END_RE.search(line)
        if match:
            anchors.append(
                {
                    "frame_id": parse_int(match.group("frame_id")),
                    "stimulus_seconds": parse_float(match.group("stimulus")),
                    "kind": "source_stall_end",
                    "order": 2,
                }
            )

    anchors = [item for item in anchors if item["frame_id"] >= 0 and math.isfinite(item["stimulus_seconds"])]
    anchors.sort(key=lambda item: (item["frame_id"], item["order"], item["stimulus_seconds"]))
    deduped = []
    seen = set()
    for item in anchors:
        key = (item["frame_id"], item["kind"], round(item["stimulus_seconds"], 6))
        if key in seen:
            continue
        seen.add(key)
        deduped.append(item)
    return deduped


def effective_marker_fps_for_extrapolation(manifest):
    target_fps = parse_float(manifest.get("target_fps"), 0.0)
    if target_fps <= 0.0:
        target_fps = 240.0

    frame_pacing = manifest.get("frame_pacing")
    if isinstance(frame_pacing, dict):
        avg_delta_ms = parse_float(frame_pacing.get("average_present_delta_ms"), 0.0)
        present_count = parse_int(frame_pacing.get("present_delta_count"), 0)
        planned_gaps = parse_int(frame_pacing.get("planned_source_gap_count"), 0)
        if avg_delta_ms > 0.0 and present_count > 10 and planned_gaps == 0:
            measured_fps = 1000.0 / avg_delta_ms
            if math.isfinite(measured_fps) and measured_fps > 1.0:
                return measured_fps

    return target_fps


def annotate_video_frames_from_app_anchors(frames, anchors, manifest):
    if not frames or not anchors:
        return False

    previous_marker = None
    unwrapped = 0
    for frame in frames:
        marker = parse_int(frame.get("marker"), 0) & 0xFFFF
        if previous_marker is None:
            unwrapped = marker
        else:
            delta = (marker - previous_marker) & 0xFFFF
            if delta > 0x8000:
                delta -= 0x10000
            unwrapped += delta
        previous_marker = marker
        frame["source_frame_id"] = unwrapped

    anchor_ids = [item["frame_id"] for item in anchors]
    target_fps = effective_marker_fps_for_extrapolation(manifest)

    for frame in frames:
        marker_id = frame.get("source_frame_id")
        pos = bisect.bisect_right(anchor_ids, marker_id)
        if pos <= 0:
            anchor = anchors[0]
            stimulus = anchor["stimulus_seconds"] + (marker_id - anchor["frame_id"]) / target_fps
            mode = "extrapolated_before"
        elif pos >= len(anchors):
            anchor = anchors[-1]
            stimulus = anchor["stimulus_seconds"] + (marker_id - anchor["frame_id"]) / target_fps
            mode = "extrapolated_after"
        else:
            prev_anchor = anchors[pos - 1]
            next_anchor = anchors[pos]
            span_frames = next_anchor["frame_id"] - prev_anchor["frame_id"]
            if span_frames <= 0:
                stimulus = prev_anchor["stimulus_seconds"]
                mode = "duplicate_anchor"
            else:
                ratio = (marker_id - prev_anchor["frame_id"]) / span_frames
                stimulus = prev_anchor["stimulus_seconds"] + ratio * (
                    next_anchor["stimulus_seconds"] - prev_anchor["stimulus_seconds"]
                )
                mode = "interpolated"
        frame["stimulus_seconds_marker"] = stimulus
        frame["stimulus_seconds_marker_mode"] = mode
    return True


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


def infer_pts_delta(pts):
    deltas = [
        pts[index] - pts[index - 1]
        for index in range(1, len(pts))
        if pts[index] > pts[index - 1]
    ]
    if deltas:
        return statistics.median(deltas)
    return 1.0 / 60.0


def parse_frame_rate_ratio(value):
    text = str(value or "").strip()
    if not text:
        return 0.0
    if "/" in text:
        numerator, denominator = text.split("/", 1)
        den = parse_float(denominator, 0.0)
        if den <= 0.0:
            return 0.0
        return parse_float(numerator, 0.0) / den
    return parse_float(text, 0.0)


def declared_video_frame_delta(video_stream):
    for key in ("avg_frame_rate", "r_frame_rate"):
        fps = parse_frame_rate_ratio(video_stream.get(key) if isinstance(video_stream, dict) else "")
        if math.isfinite(fps) and fps > 1.0:
            return 1.0 / fps
    return 0.0


def frame_pts_for_index(pts, frame_index, fallback_delta):
    if frame_index < len(pts):
        return pts[frame_index]
    if pts:
        return pts[-1] + fallback_delta * float(frame_index - len(pts) + 1)
    return fallback_delta * float(frame_index)


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


def decode_parity(frame, scaled_w, scaled_h, manifest, marker, palette_index):
    src_w = max(1, int(manifest["width"]))
    src_h = max(1, int(manifest["height"]))
    size = 28
    start_x = src_w - manifest["marker_margin"] - 8 * (size + 4)
    start_y = src_h - manifest["marker_margin"] - size
    src_x = start_x - size - 12 + size / 2.0
    src_y = start_y + size / 2.0
    lum = luminance(pixel(frame, scaled_w, scaled_h, src_x / src_w * scaled_w, src_y / src_h * scaled_h))
    bad = 1 if 80.0 < lum < 176.0 else 0
    observed = lum >= 128.0
    checksum = (marker & 0xFF) ^ ((marker >> 8) & 0xFF) ^ (palette_index if palette_index >= 0 else 0)
    value = marker ^ checksum
    expected = (bin(value & 0xFFFFFF).count("1") % 2) != 0
    return observed == expected, bad, int(observed), int(expected)


def decode_motion_at(frame, scaled_w, scaled_h, manifest, top_ratio, bar_width):
    src_h = max(1, int(manifest["height"]))
    lane_height = parse_float(manifest.get("motion_lane_height"), 42.0)
    y = int(round(float(top_ratio) * src_h + lane_height / 2.0))
    scaled_y = max(0, min(scaled_h - 1, int(round(y / src_h * scaled_h))))
    xs = []
    for x in range(int(0.02 * scaled_w), int(0.98 * scaled_w)):
        if luminance(pixel(frame, scaled_w, scaled_h, x, scaled_y)) > 210.0:
            xs.append(x)
    if not xs:
        return None
    return statistics.mean(xs) / float(scaled_w)


def decode_motion(frame, scaled_w, scaled_h, manifest):
    top_ratio = parse_float(manifest.get("motion_lane_top_ratio"), 0.72)
    bar_width = parse_float(manifest.get("motion_lane_bar_width"), 96.0)
    return decode_motion_at(frame, scaled_w, scaled_h, manifest, top_ratio, bar_width)


def decode_fast_motion(frame, scaled_w, scaled_h, manifest):
    if parse_int(manifest.get("motion_lane_count"), 1) < 2:
        return None
    top_ratio = parse_float(manifest.get("fast_motion_lane_top_ratio"), 0.82)
    bar_width = parse_float(manifest.get("fast_motion_lane_bar_width"), 48.0)
    return decode_motion_at(frame, scaled_w, scaled_h, manifest, top_ratio, bar_width)


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
    stream_start = parse_float(video_stream.get("start_time"), 0.0)
    declared_delta = declared_video_frame_delta(video_stream)
    fallback_pts_delta = declared_delta if declared_delta > 0.0 else infer_pts_delta(pts)
    use_declared_cfr_timeline = declared_delta > 0.0
    while True:
        payload = process.stdout.read(frame_size)
        if not payload:
            break
        if len(payload) != frame_size:
            break
        event_rgb = pixel(payload, scaled_w, scaled_h, scaled_w * 0.50, scaled_h * 0.45)
        palette_index, color_dist = nearest_palette_index(event_rgb, palette)
        event_color_confidence = max(0.0, min(1.0, 1.0 - (color_dist / 180.0)))
        marker, marker_bad = decode_marker(payload, scaled_w, scaled_h, manifest, inverse=False)
        inverse_marker, inverse_bad = decode_marker(payload, scaled_w, scaled_h, manifest, inverse=True)
        checksum_ok, checksum_bad, checksum_observed, checksum_expected = decode_checksum(
            payload, scaled_w, scaled_h, manifest, marker, palette_index
        )
        if parse_int(manifest.get("visual_marker_version"), 1) >= 2:
            parity_ok, parity_bad, parity_observed, parity_expected = decode_parity(
                payload, scaled_w, scaled_h, manifest, marker, palette_index
            )
        else:
            parity_ok, parity_bad, parity_observed, parity_expected = True, 0, 0, 0
        motion = decode_motion(payload, scaled_w, scaled_h, manifest)
        fast_motion = decode_fast_motion(payload, scaled_w, scaled_h, manifest)
        frames.append(
            {
                "index": frame_index,
                "pts": stream_start + frame_index * fallback_pts_delta
                if use_declared_cfr_timeline
                else frame_pts_for_index(pts, frame_index, fallback_pts_delta),
                "palette": palette_index,
                "color_dist": color_dist,
                "event_color_confidence": event_color_confidence,
                "marker": marker,
                "inverse_marker": inverse_marker,
                "marker_bad_tiles": marker_bad + inverse_bad + checksum_bad + parity_bad,
                "marker_inverse_ok": marker == inverse_marker,
                "checksum_ok": checksum_ok,
                "checksum_observed": checksum_observed,
                "checksum_expected": checksum_expected,
                "parity_ok": parity_ok,
                "parity_observed": parity_observed,
                "parity_expected": parity_expected,
                "motion": motion,
                "fast_motion": fast_motion,
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


def circular_distance(a, b):
    if a is None or b is None:
        return 0.0
    delta = abs(a - b)
    return min(delta, 1.0 - delta)


def expected_motion_from_anchor(anchor_motion, anchor_pts, pts, speed=0.25):
    if anchor_motion is None:
        return None
    return (anchor_motion + (pts - anchor_pts) * speed) % 1.0


def expected_motion_from_stimulus(stimulus_seconds, manifest):
    speed = parse_float(manifest.get("motion_lane_speed_cycles_per_second"), 0.25)
    pos = (stimulus_seconds * speed) % 1.0
    src_w = max(1, int(manifest["width"]))
    margin = parse_float(manifest.get("motion_lane_margin", manifest.get("marker_margin", 24)))
    bar_width = parse_float(manifest.get("motion_lane_bar_width", 96))
    usable = max(1.0, float(src_w) - 2.0 * margin - bar_width)
    return (margin + pos * usable + bar_width / 2.0) / float(src_w)


def expected_fast_motion_from_stimulus(stimulus_seconds, manifest):
    speed = parse_float(manifest.get("fast_motion_lane_speed_cycles_per_second"), 1.0)
    pos = (stimulus_seconds * speed) % 1.0
    src_w = max(1, int(manifest["width"]))
    margin = parse_float(manifest.get("motion_lane_margin", manifest.get("marker_margin", 24)))
    bar_width = parse_float(manifest.get("fast_motion_lane_bar_width"), 48)
    usable = max(1.0, float(src_w) - 2.0 * margin - bar_width)
    return (margin + pos * usable + bar_width / 2.0) / float(src_w)


def infer_nominal_video_fps(frames):
    deltas = [
        frames[index]["pts"] - frames[index - 1]["pts"]
        for index in range(1, len(frames))
        if frames[index]["pts"] > frames[index - 1]["pts"]
    ]
    if not deltas:
        return 0.0
    median_delta = statistics.median(deltas)
    return 1.0 / median_delta if median_delta > 0.0 else 0.0


def expected_source_repeat_run(manifest, nominal_output_fps):
    source_fps = parse_float(manifest.get("target_fps"), 0.0)
    if source_fps <= 0.0 or nominal_output_fps <= 0.0 or source_fps >= nominal_output_fps * 0.98:
        return 1
    return max(2, int(math.ceil(nominal_output_fps / source_fps)) + 1)


def expected_transition_events(manifest, max_stimulus_seconds):
    period = parse_float(manifest.get("event_period_seconds"), 1.0)
    events = manifest.get("events", [])
    event_count = max(1, len(events))
    if period <= 0.0:
        period = 1.0
    max_index = max(1, int(math.ceil(max_stimulus_seconds / period)) + event_count + 2)
    transitions = []
    for index in range(1, max_index + 1):
        transitions.append({"from": (index - 1) % event_count, "to": index % event_count, "time": index * period})
    return transitions
