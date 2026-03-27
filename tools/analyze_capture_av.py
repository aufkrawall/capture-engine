#!/usr/bin/env python3

import argparse
import collections
import json
import math
import re
import statistics
import subprocess
import sys
from pathlib import Path
from typing import NoReturn


LOG_PATTERNS = {
    "audio_latency_cap": re.compile(r"\[PullAudio\] Audio latency cap:"),
    "audio_retain_trim": re.compile(r"\[PullAudio\] WARNING: WGC CFR audio headroom exhausted"),
    "audio_coverage_trim": re.compile(r"\[PullAudio\] WGC overload sync trim:"),
    "wgc_cfr_lead_warning": re.compile(r"\[PullAudio\] WGC CFR lead warning:"),
    "wgc_coverage_mode_active": re.compile(r"CovMode=1"),
    "audio_large_gap": re.compile(r"\[PullAudio\] Large A/V gap"),
    "audio_underrun": re.compile(r"\[PullAudio\] WARNING: Source underrun"),
    "audio_overflow": re.compile(r"\[PullAudio\] WARNING: Ring buffer overflow"),
    "audio_silence_fill": re.compile(r"\[PullAudio\] Track \d+ silent - generating"),
    "wgc_output_limited": re.compile(r"\[WGC CFR\] (?:Output limited|Encoder cannot sustain target)"),
    "wgc_stop_drain_aborted": re.compile(r"\[EncoderThread\] WGC stop drain aborted"),
}

CADENCE_SELMISS_RE = re.compile(r"SelMiss=(\d+)")
CADENCE_STALEUNI_RE = re.compile(r"StaleUni=(\d+)")
CADENCE_ANCIENT_RE = re.compile(r"Ancient=(\d+)")
CADENCE_REPNOFRESH_RE = re.compile(r"RepFreshMiss=(\d+)")
CADENCE_OVER_RE = re.compile(r"Over=0x([0-9A-Fa-f]+)")


def fail(message) -> NoReturn:
    print(f"error: {message}", file=sys.stderr)
    raise SystemExit(1)


def run_command(command, text=True):
    result = subprocess.run(
        command,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=text,
        encoding="utf-8" if text else None,
        errors="replace" if text else None,
        check=False,
    )
    if result.returncode != 0:
        fail(
            "command failed ({code}): {cmd}\n{stderr}".format(
                code=result.returncode, cmd=" ".join(str(part) for part in command), stderr=result.stderr.strip()
            )
        )
    return result


def run_ffprobe_json(ffprobe, args):
    result = run_command([str(ffprobe), "-v", "error", *args, "-of", "json"])
    try:
        return json.loads(result.stdout)
    except json.JSONDecodeError as exc:
        fail(f"ffprobe returned invalid JSON: {exc}")


def build_read_interval(start_time, duration):
    if duration <= 0.0:
        return None
    safe_start = max(0.0, start_time)
    return f"{safe_start:.6f}%+{duration:.6f}"


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
        numerator, denominator = text.split("/", 1)
        denominator_value = parse_float(denominator, 0.0)
        if denominator_value == 0.0:
            return 0.0
        return parse_float(numerator, 0.0) / denominator_value
    return parse_float(text, 0.0)


def safe_mean(values):
    return statistics.mean(values) if values else 0.0


def safe_pstdev(values):
    return statistics.pstdev(values) if len(values) > 1 else 0.0


def format_seconds(value):
    return f"{value:.6f}s"


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


def analyze_video_timing(ffprobe, capture_path, read_interval=None):
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

    durations = [parse_float(frame.get("pkt_duration_time")) for frame in typed_frames]
    deltas = [round(pts[i] - pts[i - 1], 6) for i in range(1, len(pts))]
    last_duration = durations[-1] if durations else 0.0
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


def analyze_log(log_path):
    if not log_path:
        return None
    text = log_path.read_text(encoding="utf-8", errors="replace")
    lines = text.splitlines()
    counts = {name: len(pattern.findall(text)) for name, pattern in LOG_PATTERNS.items()}
    cadence_window_count = 0

    cadence_metrics = {
        "sel_miss": [],
        "stale_unique": [],
        "ancient": [],
        "rep_no_fresh": [],
        "overload_flags": [],
    }
    for line in lines:
        if "[Cadence Health]" not in line:
            continue
        cadence_window_count += 1
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

    return {
        "counts": counts,
        "cadence_windows": cadence_window_count,
        "max_sel_miss": max(cadence_metrics["sel_miss"]) if cadence_metrics["sel_miss"] else 0,
        "max_stale_unique": max(cadence_metrics["stale_unique"]) if cadence_metrics["stale_unique"] else 0,
        "max_ancient": max(cadence_metrics["ancient"]) if cadence_metrics["ancient"] else 0,
        "max_rep_no_fresh": max(cadence_metrics["rep_no_fresh"]) if cadence_metrics["rep_no_fresh"] else 0,
        "saw_encoder_overload": any(flags & 0x1 for flags in cadence_metrics["overload_flags"]),
        "saw_mux_overload": any(flags & 0x2 for flags in cadence_metrics["overload_flags"]),
    }


def print_top_histogram(name, histogram, limit=6):
    print(f"{name}:")
    if not histogram:
        print("  none")
        return
    for key, count in histogram.most_common(limit):
        print(f"  {key}: {count}")


def analyze_window(ffprobe, ffmpeg, capture_path, audio_streams, start_time, duration, framehash, framehash_width):
    read_interval = build_read_interval(start_time, duration)
    video_timing = analyze_video_timing(ffprobe, capture_path, read_interval=read_interval)
    audio_tracks = [
        analyze_audio_stream(ffprobe, capture_path, audio_ordinal, stream_info, read_interval=read_interval)
        for audio_ordinal, stream_info in enumerate(audio_streams)
    ]
    duplicate_runs = None
    if framehash and duration > 0.0:
        duplicate_runs = analyze_video_duplicate_runs_segment(
            ffmpeg, capture_path, framehash_width, start_time, duration
        )

    video_duration = video_timing["duration"]
    audio_lengths = [track["decoded_duration"] for track in audio_tracks]
    audio_duration_spread = (max(audio_lengths) - min(audio_lengths)) if audio_lengths else 0.0
    video_audio_max_delta = (
        max(abs(track["decoded_duration"] - video_duration) for track in audio_tracks) if audio_tracks else 0.0
    )
    return {
        "start_time": start_time,
        "duration": duration,
        "video": video_timing,
        "audio": audio_tracks,
        "duplicate_runs": duplicate_runs,
        "audio_duration_spread": audio_duration_spread,
        "video_audio_max_delta": video_audio_max_delta,
    }


def analyze_video_duplicate_runs_segment(ffmpeg, capture_path, scale_width, start_time, duration):
    result = run_command(
        [
            str(ffmpeg),
            "-v",
            "error",
            "-ss",
            f"{max(0.0, start_time):.6f}",
            "-t",
            f"{duration:.6f}",
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


def print_window_summary(name, window):
    print(f"window_{name}:")
    print(
        "  start={start:.6f} duration={duration:.6f} video_frames={frames} video_duration={video_duration:.6f}".format(
            start=window["start_time"],
            duration=window["duration"],
            frames=window["video"]["frame_count"],
            video_duration=window["video"]["duration"],
        )
    )
    print(
        "  delta_mean={mean:.6f} delta_min={delta_min:.6f} delta_max={delta_max:.6f} delta_stdev={stdev:.6f}".format(
            mean=window["video"]["delta_mean"],
            delta_min=window["video"]["delta_min"],
            delta_max=window["video"]["delta_max"],
            stdev=window["video"]["delta_stdev"],
        )
    )
    print(
        f"  audio_duration_spread={window['audio_duration_spread']:.6f} max_video_audio_duration_delta={window['video_audio_max_delta']:.6f}"
    )
    if window["duplicate_runs"] is None:
        print("  duplicate_runs=skipped")
    else:
        print(
            "  duplicate_runs framehash_frames={framehash_count} repeated_runs={repeated_runs} repeated_frames={repeated_frames} longest_run={longest}".format(
                framehash_count=window["duplicate_runs"]["framehash_count"],
                repeated_runs=window["duplicate_runs"]["repeated_run_count"],
                repeated_frames=window["duplicate_runs"]["repeated_frame_count"],
                longest=window["duplicate_runs"]["longest_run"],
            )
        )
    for track in window["audio"]:
        print(
            "  a:{ordinal} samples={samples} duration={duration:.6f} start={start:.6f} end={end:.6f}".format(
                ordinal=track["audio_ordinal"],
                samples=track["sample_total"],
                duration=track["decoded_duration"],
                start=track["frame_start"],
                end=track["frame_end"],
            )
        )


def main():
    parser = argparse.ArgumentParser(
        description="Analyze a capture file for CFR timing, duplicate-frame runs, and exact audio track alignment."
    )
    parser.add_argument("capture", type=Path, help="Capture file to analyze")
    parser.add_argument("--log", type=Path, help="Optional media.log to summarize alongside the capture")
    parser.add_argument("--ffprobe", type=Path, default=Path("ffprobe"), help="Path to ffprobe executable")
    parser.add_argument("--ffmpeg", type=Path, default=Path("ffmpeg"), help="Path to ffmpeg executable")
    parser.add_argument(
        "--framehash",
        action="store_true",
        help="Run an additional full-length ffmpeg framemd5 pass to quantify repeated visual frames",
    )
    parser.add_argument(
        "--full-scan",
        action="store_true",
        help="Use ffprobe frame-by-frame scans for exact frame deltas and decoded audio sample totals",
    )
    parser.add_argument(
        "--framehash-width",
        type=int,
        default=320,
        help="Downscale width for duplicate-run frame hashing when --framehash is enabled (default: 320)",
    )
    parser.add_argument(
        "--window-seconds",
        type=float,
        default=10.0,
        help="Analyze first/middle/last windows of this many seconds using ffprobe frame scans (default: 10)",
    )
    args = parser.parse_args()

    if not args.capture.exists():
        fail(f"capture file not found: {args.capture}")
    if args.log and not args.log.exists():
        fail(f"log file not found: {args.log}")

    format_info, video_streams, audio_streams = analyze_streams(args.ffprobe, args.capture)
    video_stream = video_streams[0]
    format_duration = parse_float(format_info.get("duration"))
    video_timing = (
        analyze_video_timing(args.ffprobe, args.capture)
        if args.full_scan
        else analyze_video_stream_metadata(video_stream, format_duration)
    )
    duplicate_runs = None
    if args.framehash:
        duplicate_runs = analyze_video_duplicate_runs(args.ffmpeg, args.capture, args.framehash_width)
    audio_tracks = []
    for audio_ordinal, stream_info in enumerate(audio_streams):
        if args.full_scan:
            audio_tracks.append(analyze_audio_stream(args.ffprobe, args.capture, audio_ordinal, stream_info))
        else:
            audio_tracks.append(analyze_audio_stream_metadata(audio_ordinal, stream_info, format_duration))
    log_summary = analyze_log(args.log)

    nominal_fps = parse_ratio(video_stream.get("avg_frame_rate") or video_stream.get("r_frame_rate"))
    video_duration = video_timing["duration"]
    audio_lengths = [track["decoded_duration"] for track in audio_tracks]
    audio_duration_spread = (max(audio_lengths) - min(audio_lengths)) if audio_lengths else 0.0
    video_audio_max_delta = (
        max(abs(track["decoded_duration"] - video_duration) for track in audio_tracks) if audio_tracks else 0.0
    )
    window_duration = max(0.0, min(args.window_seconds, format_duration if format_duration > 0.0 else args.window_seconds))
    windows = {}
    if window_duration > 0.0 and format_duration > 0.0:
        middle_start = max(0.0, (format_duration / 2.0) - (window_duration / 2.0))
        last_start = max(0.0, format_duration - window_duration)
        window_specs = {
            "first": 0.0,
            "middle": middle_start,
            "last": last_start,
        }
        for name, start_time in window_specs.items():
            windows[name] = analyze_window(
                args.ffprobe,
                args.ffmpeg,
                args.capture,
                audio_streams,
                start_time,
                window_duration,
                args.framehash,
                args.framehash_width,
            )

    print(f"capture: {args.capture}")
    print(f"container_duration: {format_seconds(format_duration)}")
    print()

    print("video:")
    print(f"  timing_source={video_timing['source']}")
    print(
        "  codec={codec} resolution={width}x{height} nominal_fps={fps:.6f}".format(
            codec=video_stream.get("codec_name", ""),
            width=parse_int(video_stream.get("width")),
            height=parse_int(video_stream.get("height")),
            fps=nominal_fps,
        )
    )
    print(
        "  frames={frames} first_pts={first_pts:.6f} last_pts={last_pts:.6f} duration={duration:.6f}".format(
            frames=video_timing["frame_count"],
            first_pts=video_timing["first_pts"],
            last_pts=video_timing["last_pts"],
            duration=video_duration,
        )
    )
    print(
        "  delta_mean={mean:.6f} delta_min={delta_min:.6f} delta_max={delta_max:.6f} delta_stdev={stdev:.6f}".format(
            mean=video_timing["delta_mean"],
            delta_min=video_timing["delta_min"],
            delta_max=video_timing["delta_max"],
            stdev=video_timing["delta_stdev"],
        )
    )
    if args.full_scan:
        print_top_histogram("  frame_delta_histogram", video_timing["delta_histogram"])
    else:
        print("  frame_delta_histogram: skipped (pass --full-scan for per-frame timing)")
    print(
        "  duplicate_runs={status}".format(
            status=(
                "skipped (pass --framehash for full visual duplicate scan)"
                if duplicate_runs is None
                else "enabled"
            )
        )
    )
    if duplicate_runs is not None:
        print(
            (
                "  duplicate_runs framehash_frames={framehash_count} repeated_runs={repeated_runs} "
                "repeated_frames={repeated_frames} longest_run={longest}"
            ).format(
                framehash_count=duplicate_runs["framehash_count"],
                repeated_runs=duplicate_runs["repeated_run_count"],
                repeated_frames=duplicate_runs["repeated_frame_count"],
                longest=duplicate_runs["longest_run"],
            ),
        )
        print_top_histogram("  duplicate_run_histogram", duplicate_runs["repeated_histogram"])
    print()

    print("audio:")
    if not audio_tracks:
        print("  no audio streams")
    for track in audio_tracks:
        print(
            (
                "  a:{ordinal} stream={stream_index} codec={codec} rate={rate}Hz ch={channels} "
                "samples={samples} duration={duration:.6f} start={start:.6f} "
                "end={end:.6f} source={source}"
            ).format(
                ordinal=track["audio_ordinal"],
                stream_index=track["stream_index"],
                codec=track["codec"],
                rate=track["sample_rate"],
                channels=track["channels"],
                samples=track["sample_total"],
                duration=track["decoded_duration"],
                start=track["frame_start"],
                end=track["frame_end"],
                source=track["source"],
            ),
        )
    if audio_tracks:
        print(f"  audio_duration_spread={audio_duration_spread:.6f}")
        print(f"  max_video_audio_duration_delta={video_audio_max_delta:.6f}")
    print()

    if log_summary:
        print("log_summary:")
        for name, count in sorted(log_summary["counts"].items()):
            print(f"  {name}={count}")
        print(
            (
                "  cadence_windows={windows} max_sel_miss={sel_miss} "
                "max_stale_unique={stale_unique} max_ancient={ancient} "
                "max_rep_no_fresh={rep_no_fresh}"
            ).format(
                windows=log_summary["cadence_windows"],
                sel_miss=log_summary["max_sel_miss"],
                stale_unique=log_summary["max_stale_unique"],
                ancient=log_summary["max_ancient"],
                rep_no_fresh=log_summary["max_rep_no_fresh"],
            ),
        )
        print(
            "  saw_encoder_overload={enc} saw_mux_overload={mux}".format(
                enc=int(log_summary["saw_encoder_overload"]),
                mux=int(log_summary["saw_mux_overload"]),
            )
        )
        print()

    if windows:
        print("windows:")
        for name in ("first", "middle", "last"):
            print_window_summary(name, windows[name])
        print()

    print("summary:")
    if video_timing["frame_count"] > 1 and nominal_fps > 0.0:
        expected_delta = 1.0 / nominal_fps
        delta_error_us = abs(video_timing["delta_mean"] - expected_delta) * 1_000_000.0
        print(f"  mean_frame_delta_error_us={delta_error_us:.3f}")
    print(f"  exact_audio_length_match={'yes' if math.isclose(audio_duration_spread, 0.0, abs_tol=1e-6) else 'no'}")
    print(
        "  all_audio_tracks_match_video_length={value}".format(
            value="yes" if math.isclose(video_audio_max_delta, 0.0, abs_tol=1e-3) else "no"
        )
    )


if __name__ == "__main__":
    main()
