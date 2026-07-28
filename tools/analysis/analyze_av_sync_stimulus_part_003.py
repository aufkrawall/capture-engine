

def detect_audio_states(samples, sample_rate, frequencies):
    if not samples or sample_rate <= 0:
        return [], []
    window = max(1024, int(sample_rate * 0.045))
    step = max(128, int(sample_rate * 0.005))
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
        if audio_refinement_is_plausible(coarse_time, crossing):
            updated["time"] = crossing
        else:
            updated["time"] = coarse_time
            updated["rejected_refined_time"] = crossing
        refined.append(updated)
    return refined


def audio_refinement_is_plausible(coarse_time, refined_time):
    return math.isfinite(refined_time) and abs(refined_time - coarse_time) <= MAX_AUDIO_REFINEMENT_DELTA_SECONDS


def synthesize_local_frequency_transition(sample_rate, from_frequency, to_frequency, boundary_seconds):
    duration_seconds = boundary_seconds + 0.20
    total_samples = int(math.ceil(duration_seconds * sample_rate))
    samples = array.array("f")
    for index in range(total_samples):
        t = index / sample_rate
        relative = t - boundary_seconds
        frequency = from_frequency if relative < 0.0 else to_frequency
        samples.append(float(math.sin(2.0 * math.pi * frequency * relative) * 0.20))
    return samples


def calibrate_audio_detector_biases(sample_rate, frequencies):
    if sample_rate <= 0 or not frequencies:
        return {}
    boundary_seconds = 0.30
    biases = {}
    for to_index in range(len(frequencies)):
        from_index = (to_index - 1) % len(frequencies)
        samples = synthesize_local_frequency_transition(
            sample_rate, frequencies[from_index], frequencies[to_index], boundary_seconds
        )
        refined = refine_audio_transitions(
            samples,
            sample_rate,
            frequencies,
            [{"from": from_index, "to": to_index, "time": boundary_seconds}],
        )
        if not refined:
            continue
        bias = parse_float(refined[0].get("time"), boundary_seconds) - boundary_seconds
        if math.isfinite(bias) and abs(bias) <= MAX_AUDIO_REFINEMENT_DELTA_SECONDS:
            biases[(from_index, to_index)] = bias
    return biases


def apply_audio_detector_bias_correction(transitions, biases):
    corrected = []
    for transition in transitions:
        updated = dict(transition)
        key = (updated.get("from"), updated.get("to"))
        bias = parse_float(biases.get(key), 0.0)
        if bias:
            updated["raw_time"] = updated.get("time")
            updated["detector_bias_seconds"] = bias
            updated["time"] = parse_float(updated.get("time"), 0.0) - bias
        corrected.append(updated)
    return corrected


def match_transition_offset_points(reference, candidate, max_window):
    points = []
    missing = 0
    used = set()
    for ref in reference:
        choices = [
            (abs(transition_signed_offset(item, ref)), index, item)
            for index, item in enumerate(candidate)
            if index not in used and ref["to"] == item["to"] and abs(transition_signed_offset(item, ref)) <= max_window
        ]
        if not choices:
            missing += 1
            continue
        _, index, item = min(choices, key=lambda value: value[0])
        used.add(index)
        points.append(
            {
                "reference_time": parse_float(ref.get("time"), 0.0),
                "candidate_time": parse_float(item.get("time"), 0.0),
                "offset_seconds": transition_signed_offset(item, ref),
                "to": ref.get("to"),
            }
        )
    return points, missing


def match_transition_offsets(reference, candidate, max_window):
    points, missing = match_transition_offset_points(reference, candidate, max_window)
    offsets = [point["offset_seconds"] for point in points]
    return offsets, missing


def transition_signed_offset(candidate, reference):
    candidate_time = parse_float(candidate.get("time"), float("nan"))
    reference_time = parse_float(reference.get("time"), float("nan"))
    if not math.isfinite(candidate_time) or not math.isfinite(reference_time):
        return float("inf")
    lo = parse_float(reference.get("time_min"), reference_time)
    hi = parse_float(reference.get("time_max"), reference_time)
    if not math.isfinite(lo):
        lo = reference_time
    if not math.isfinite(hi):
        hi = reference_time
    if lo > hi:
        lo, hi = hi, lo
    if candidate_time < lo:
        return candidate_time - lo
    if candidate_time > hi:
        return candidate_time - hi
    return 0.0


def summarize_offsets_ms(offsets):
    values = [offset * 1000.0 for offset in offsets]
    if not values:
        return {
            "matched": 0,
            "max_abs": 0.0,
            "mean_signed": 0.0,
            "median_signed": 0.0,
            "min_signed": 0.0,
            "max_signed": 0.0,
            "span": 0.0,
        }
    min_signed = min(values)
    max_signed = max(values)
    return {
        "matched": len(values),
        "max_abs": max(abs(value) for value in values),
        "mean_signed": statistics.fmean(values),
        "median_signed": statistics.median(values),
        "min_signed": min_signed,
        "max_signed": max_signed,
        "span": max_signed - min_signed,
    }


def compute_offset_slope_ms_per_minute(points):
    usable = [
        (
            parse_float(point.get("reference_time"), float("nan")),
            parse_float(point.get("offset_seconds"), float("nan")) * 1000.0,
        )
        for point in points
    ]
    usable = [
        (time_value, offset_ms)
        for time_value, offset_ms in usable
        if math.isfinite(time_value) and math.isfinite(offset_ms)
    ]
    if len(usable) < 2:
        return 0.0
    mean_time = statistics.fmean(time_value for time_value, _ in usable)
    mean_offset = statistics.fmean(offset_ms for _, offset_ms in usable)
    denom = sum((time_value - mean_time) ** 2 for time_value, _ in usable)
    if denom <= 0.0:
        return 0.0
    slope_ms_per_second = (
        sum((time_value - mean_time) * (offset_ms - mean_offset) for time_value, offset_ms in usable) / denom
    )
    return slope_ms_per_second * 60.0


def offset_slope_is_acceptable(
    offsets,
    offset_stats,
    slope_ms_per_minute,
    max_slope_ms_per_minute,
    min_excursion_ms,
    frame_quantization_excursion_ms=0.0,
    max_abs_guard_ms=None,
    max_mean_guard_ms=None,
):
    if not offsets:
        return False
    if abs(slope_ms_per_minute) <= max_slope_ms_per_minute:
        return True
    span_ms = parse_float(offset_stats.get("span"), 0.0)
    excursion_limit_ms = max(min_excursion_ms, frame_quantization_excursion_ms)
    if span_ms > excursion_limit_ms:
        return False
    if max_abs_guard_ms is not None and parse_float(offset_stats.get("max_abs"), 999999.0) > max_abs_guard_ms:
        return False
    mean_signed_abs = abs(parse_float(offset_stats.get("mean_signed"), 999999.0))
    if max_mean_guard_ms is not None and mean_signed_abs > max_mean_guard_ms:
        return False
    return True


def analyze_ce_log_text(text):
    counts = {name: len(pattern.findall(text)) for name, pattern in STRICT_CE_PATTERNS.items()}
    counts["audio_late_app_source_backlog"] = count_unjoined_late_app_source_backlog(text)
    raw_extreme_drift = counts["audio_extreme_drift"]
    live_extreme_drift, historical_stop_force_drain = count_audio_extreme_drift_events(text)
    classified_extreme_drift = live_extreme_drift + historical_stop_force_drain
    counts["audio_extreme_drift"] = live_extreme_drift + max(
        0, raw_extreme_drift - classified_extreme_drift
    )
    counts["audio_stop_force_drain_backlog"] += historical_stop_force_drain
    return counts


def count_unjoined_late_app_source_backlog(text):
    live_join_sources = {match.group(1) for match in LATE_APP_LIVE_JOIN_SRC_RE.finditer(text)}

    typed_stop_sources = {}
    for match in STOP_AUDIO_SOURCE_TYPE_RE.finditer(text):
        process = match.group(2)
        typed_stop_sources[match.group(1)] = process != "-"

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
        if int(match.group(2)) >= 1000 and source not in live_join_sources:
            count += 1
    if matched_structured_line:
        return count
    return len(STRICT_CE_PATTERNS["audio_late_app_source_backlog"].findall(text))


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


def analyze_ce_log(path):
    if not path:
        return {}
    return analyze_ce_log_text(path.read_text(encoding="utf-8", errors="replace"))


def analyze_app_log_text(text):
    return {name: len(pattern.findall(text)) for name, pattern in STRICT_APP_PATTERNS.items()}


def analyze_app_log(path):
    if not path:
        return {}
    return analyze_app_log_text(path.read_text(encoding="utf-8", errors="replace"))


def make_check(name, passed, actual, expected, failure_class):
    return {
        "name": name,
        "passed": bool(passed),
        "actual": actual,
        "expected": expected,
        "failure_class": failure_class,
    }


def fast_motion_missing_limit(video_summary, max_motion_error_frames):
    if not video_summary.get("fast_motion_available", False):
        return 0
    return max(
        max_motion_error_frames,
        math.ceil(video_summary.get("frames", 0) * FAST_MOTION_ISOLATED_DECODE_MISSING_RATIO),
    )
