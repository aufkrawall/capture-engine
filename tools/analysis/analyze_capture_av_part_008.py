

def wgc_is_bounded_source_limited_active_delay(media_evidence, item):
    return (
        wgc_active_delay_matches_request(item)
        and wgc_has_delay_residual_evidence(item)
        and wgc_has_source_limited_delay_context(media_evidence, item)
        and not wgc_has_mixed_policy_pressure(item)
        and wgc_late_residual_is_bounded(item)
    )


def wgc_is_sync_protected_source_limited_ceiling(media_evidence, item):
    return (
        wgc_is_bounded_source_limited_active_delay(media_evidence, item)
        and item.get("source_repeat_lower_bound", 0) > 0
        and item.get("excess_repeats", 0) == 0
        and item.get("policy_added_repeats", 0) == 0
        and item.get("smoothness_not_maximal", 0) == 0
        and item.get("delay_post_selection_rejected_sync", 0) == 0
        and item.get("delay_soft_late_accepted", 0) == 0
        and item.get("delay_repeat_soft_safe_candidate", 0) == 0
        and item.get("delay_sync_protected_repeats", 0) > 0
    )


def has_wgc_timestamp_domain_mismatch(media_evidence):
    for item in media_evidence["wgc_smoothness_summary"]:
        if item.get("av_delay_ms", 0.0) <= 0.0 or not wgc_has_raw_delay_residual_evidence(item):
            continue
        predicted_bounded = (
            item.get("delay_residual_avg_abs_us", 0) <= 5000
            and item.get("delay_residual_p95_us", 0) <= 10000
            and item.get("delay_residual_late_max_us", 0) <= 10000
        )
        raw_unbounded = (
            item.get("raw_residual_avg_abs_us", 0) > 5000
            or item.get("raw_residual_p95_us", 0) > 10000
            or item.get("raw_residual_late_max_us", 0) > WGC_RESIDUAL_ISOLATED_MAX_FAULT_US
        )
        if predicted_bounded and raw_unbounded:
            return True
    return False


def has_wgc_active_delay_post_selection_reject(media_evidence):
    return any(
        item.get("delay_post_selection_rejected_sync", 0) > 0
        for item in media_evidence["wgc_smoothness_summary"]
    )


def has_wgc_av_sync_delay_residual_fault(media_evidence, source_limited_playout_maximal=None):
    for item in media_evidence["wgc_smoothness_summary"]:
        if item.get("av_delay_ms", 0.0) <= 0.0:
            continue

        # Newer WGC summaries log actual selected-frame delay realization. Treat
        # this as stronger evidence than startup/effective configuration parity.
        residual_logged = wgc_has_delay_residual_evidence(item)
        realized_delay_matches = wgc_active_delay_matches_request(item)
        bounded_source_limited = wgc_is_bounded_source_limited_active_delay(media_evidence, item)
        if (
            realized_delay_matches
            and item.get("sync_delay_policy_holds", 0) >= 10
            and item.get("too_new_lead_max_us", 0) > 10000
            and not bounded_source_limited
        ):
            return True
        if not residual_logged:
            continue
        if wgc_source_limited_delay_is_context(
            media_evidence, item, source_limited_playout_maximal
        ):
            continue

        if item.get("delay_residual_avg_abs_us", 0) > 5000:
            return True
        if item.get("delay_residual_p95_us", 0) > 10000:
            return True
        if (
            item.get("sync_delay_policy_holds", 0) > 0
            and item.get("delay_residual_late_max_us", 0) > 10000
        ):
            return True
        if item.get("delay_residual_late_max_us", 0) > WGC_RESIDUAL_ISOLATED_MAX_FAULT_US:
            return True
        if item.get("raw_residual_avg_abs_us", 0) > 5000:
            return True
        if item.get("raw_residual_p95_us", 0) > 10000:
            return True
        if (
            item.get("sync_delay_policy_holds", 0) > 0
            and item.get("raw_residual_late_max_us", 0) > 10000
        ):
            return True
        if item.get("raw_residual_late_max_us", 0) > WGC_RESIDUAL_ISOLATED_MAX_FAULT_US:
            return True
        if (
            item.get("delay_residual_max_us", 0) > 10000
            and not (
                bounded_source_limited
                and item.get("delay_residual_early_max_us", 0) >= item.get("delay_residual_max_us", 0)
            )
        ):
            return True

    return False


def has_wgc_audio_late_risk(media_evidence, source_limited_playout_maximal=None):
    for item in media_evidence["wgc_smoothness_summary"]:
        if item.get("av_delay_ms", 0.0) <= 0.0:
            continue

        soft_late_accepted = item.get("delay_soft_late_accepted", 0)
        near_cap_accepted = item.get("delay_near_cap_accepted", 0)
        if wgc_source_limited_delay_is_context(
            media_evidence, item, source_limited_playout_maximal
        ):
            continue
        source_limited_ceiling = wgc_is_sync_protected_source_limited_ceiling(media_evidence, item)
        if soft_late_accepted >= WGC_AUDIO_LATE_RISK_SOFT_ACCEPT_MIN_COUNT:
            return True
        if near_cap_accepted >= WGC_AUDIO_LATE_RISK_SOFT_ACCEPT_MIN_COUNT and not source_limited_ceiling:
            return True

        if (
            not source_limited_ceiling
            and (
                item.get("delay_residual_p95_us", 0) > WGC_AUDIO_LATE_RISK_P95_US
                or item.get("raw_residual_p95_us", 0) > WGC_AUDIO_LATE_RISK_P95_US
                or item.get("predicted_residual_p95_us", 0) > WGC_AUDIO_LATE_RISK_P95_US
            )
        ):
            return True

        # A single late max near the hard cap is acceptable during true source-limited
        # stalls. It becomes actionable when the selector accepted relaxed frames in
        # that region, because that can make audio feel late despite exact mux/audio
        # durations.
        accepted_relaxed_frames = soft_late_accepted + near_cap_accepted
        if accepted_relaxed_frames < WGC_AUDIO_LATE_RISK_SOFT_ACCEPT_MIN_COUNT:
            continue
        if source_limited_ceiling and soft_late_accepted == 0:
            continue
        if (
            item.get("delay_residual_late_max_us", 0) >= WGC_AUDIO_LATE_RISK_NEAR_CAP_US
            or item.get("raw_residual_late_max_us", 0) >= WGC_AUDIO_LATE_RISK_NEAR_CAP_US
            or item.get("predicted_residual_late_max_us", 0) >= WGC_AUDIO_LATE_RISK_NEAR_CAP_US
        ):
            return True

    return False


def has_wgc_sync_delay_policy_fault(media_evidence):
    for item in media_evidence["wgc_smoothness_summary"]:
        requested_delay_ms = item.get("av_delay_ms", 0.0)
        if requested_delay_ms <= 0.0:
            continue

        policy_holds = item.get("sync_delay_policy_holds", 0)
        if policy_holds >= 10:
            if (
                wgc_is_bounded_source_limited_active_delay(media_evidence, item)
                and item.get("delay_residual_late_max_us", 0) <= 10000
                and item.get("raw_residual_late_max_us", 0) <= 10000
            ):
                continue
            return True

        source_holds = item.get("sync_delay_source_limited_holds", 0)
        if source_holds > 0 or policy_holds > 0:
            continue

        # Compatibility for logs before source-limited/policy split: extreme realized-delay
        # hold clusters are a visual policy fault, but not proof that the A/V delay itself
        # was unrealized when startup/effective delay already match the request.
        realized_delay_matches = wgc_active_delay_matches_request(item)
        if not realized_delay_matches:
            continue
        sync_delay_holds = item.get("sync_delay_holds", 0)
        too_new_lead_us = item.get("too_new_lead_max_us", 0)
        if sync_delay_holds >= 120:
            return True
        if sync_delay_holds >= 30 and too_new_lead_us >= max(100000, int(requested_delay_ms * 3000.0)):
            return True

    return False


def has_wgc_cfr_smoothness_not_maximal(media_evidence):
    for item in media_evidence["wgc_smoothness_summary"]:
        if item.get("smoothness_not_maximal", 0) > 0:
            return True
        if item.get("delay_post_selection_rejected_sync", 0) > 0:
            return True
        if item.get("policy_added_repeats", 0) >= WGC_CFR_SMOOTHNESS_EXCESS_REPEAT_FAULT_MIN_COUNT:
            return True
        if (
            item.get("policy_added_repeats", 0) >= WGC_CFR_SMOOTHNESS_POLICY_REPEAT_NOTICE_MIN_COUNT
            and item.get("live", 0) > 0
            and (item.get("policy_added_repeats", 0) * 1000) // item.get("live", 1)
            >= WGC_CFR_SMOOTHNESS_POLICY_REPEAT_NOTICE_PERMILLE
        ):
            return True
        if item.get("excess_repeats", 0) >= WGC_CFR_SMOOTHNESS_EXCESS_REPEAT_FAULT_MIN_COUNT:
            return True
        if item.get("excess_repeat_cluster_max_ticks", 0) >= WGC_CFR_SMOOTHNESS_EXCESS_REPEAT_CLUSTER_FAULT_TICKS:
            return True
    return False


def has_wgc_startup_smoothness_underfilled(media_evidence):
    for item in media_evidence["wgc_smoothness_summary"]:
        smoothness_attempted = (
            item.get("smooth_target_delay_us", 0) > 0
            or item.get("smoothness_buffer_retained_frames", 0) > 0
            or item.get("smoothness_buffer_desired_frames", 0) > 0
        )
        if not smoothness_attempted:
            continue
        if item.get("smooth_delay_deficit_us", 0) >= 8000:
            return True
        if item.get("startup_delay_deficit_us", 0) >= 8000:
            return True
        reason = item.get("startup_reserve_reason", "")
        if reason in ("partial_span_timeout", "reserve_timeout", "low_water_timeout"):
            return True
    return any(
        quality.get("smooth_delay_deficit_us", 0) >= 8000
        or quality.get("startup_delay_deficit_us", 0) >= 8000
        for quality in media_evidence["wgc_quality"]
    )


def wgc_clean_source_coverage_items(media_evidence):
    return [
        item
        for item in media_evidence["wgc_source_coverage"]
        if item.get("best_effort", 0) > 0
        and item.get("source_repeat_lower_bound", 0) > 0
        and item.get("duplicates", 0) == item.get("source_repeat_lower_bound", 0)
        and item.get("excess_repeats", 0) == 0
        and item.get("policy_added_repeats", 0) == 0
        and item.get("clean_encoder_mux", 0) > 0
        and item.get("clean_pool", 0) > 0
        and item.get("clean_selection", 0) > 0
    ]


def has_wgc_clean_source_limited_coverage(media_evidence):
    return bool(wgc_clean_source_coverage_items(media_evidence))


def has_wgc_source_limited_playout_maximal(media_evidence):
    if has_wgc_clean_source_limited_coverage(media_evidence):
        return True
    coverage_items = [
        item
        for item in media_evidence["wgc_source_coverage"]
        if item.get("source_repeat_lower_bound", 0) > 0
        and item.get("policy_added_repeats", 0) == 0
        and item.get("clean_encoder_mux", 0) > 0
        and item.get("clean_pool", 0) > 0
        and parse_hex_flags(item.get("encoder_overload", "0x0")) == 0
        and item.get("mux_backpressure", 0) == 0
        and item.get("pool_pressure", 0) == 0
    ]
    if not coverage_items or has_wgc_repeat_with_safe_candidate(media_evidence):
        return False
    for item in media_evidence["wgc_smoothness_summary"]:
        excess_repeats = item.get("excess_repeats", 0)
        matching_coverage = [
            coverage
            for coverage in coverage_items
            if coverage.get("excess_repeats", 0) == excess_repeats
            and coverage.get("source_repeat_lower_bound", 0) == item.get("source_repeat_lower_bound", 0)
        ]
        live = max(
            item.get("live", 0),
            max((coverage.get("live", 0) for coverage in matching_coverage), default=0),
        )
        allowed_accounting_excess = max(5, live // 1000)
        if (
            live > 0
            and item.get("wgc_smoothness_verdict_complete", 0) > 0
            and item.get("source_repeat_lower_bound", 0) > 0
            and excess_repeats <= allowed_accounting_excess
            and item.get("policy_added_repeats", 0) == 0
            and item.get("excess_repeat_clusters", 0) == 0
            and item.get("excess_repeat_cluster_max_ticks", 0) == 0
            and item.get("smoothness_not_maximal", 0) == 0
            and item.get("mixed_policy_fault", 0) == 0
            and item.get("delay_post_selection_rejected_sync", 0) == 0
            and item.get("wgc_smoothness_evidence_incomplete", 0) == 0
            and matching_coverage
        ):
            return True
    return False


def has_wgc_backend_transition_source_limited_playout_maximal(media_evidence, backend_history):
    """Allow bounded lower-bound uncertainty across a proven capture-backend transition.

    A DXGI duplication access-loss fallback changes producer cadence and resets the source
    accounting domain. The aggregate lower bound can therefore undercount a small number of
    necessary holds even when the runtime's complete policy verdict proves that CE added no
    avoidable repeat cluster. Keep this exception transition-scoped and require exact output,
    clean capacity/pool state, and complete selection-policy evidence.
    """
    has_dxgi_to_wgc_fallback = any(
        previous == "dxgi_dup" and current == "wgc"
        for previous, current in zip(backend_history, backend_history[1:])
    )
    if not has_dxgi_to_wgc_fallback or not has_source_starvation(media_evidence):
        return False
    if not has_exact_final_mux_evidence(media_evidence):
        return False

    coverage_items = [
        item
        for item in media_evidence["wgc_source_coverage"]
        if item.get("source_repeat_lower_bound", 0) > 0
        and item.get("policy_added_repeats", 0) == 0
        and item.get("clean_encoder_mux", 0) > 0
        and item.get("clean_pool", 0) > 0
        and parse_hex_flags(item.get("encoder_overload", "0x0")) == 0
        and item.get("mux_backpressure", 0) == 0
        and item.get("pool_pressure", 0) == 0
    ]
    if not coverage_items:
        return False

    for item in media_evidence["wgc_smoothness_summary"]:
        lower_bound = item.get("source_repeat_lower_bound", 0)
        excess_repeats = item.get("excess_repeats", 0)
        allowed_transition_excess = max(5, int(math.ceil(lower_bound * 0.10)))
        matching_coverage = any(
            coverage.get("source_repeat_lower_bound", 0) == lower_bound
            and coverage.get("excess_repeats", 0) == excess_repeats
            for coverage in coverage_items
        )
        if (
            lower_bound > 0
            and excess_repeats <= allowed_transition_excess
            and item.get("wgc_smoothness_verdict_complete", 0) > 0
            and item.get("policy_added_repeats", 0) == 0
            and item.get("excess_repeat_clusters", 0) == 0
            and item.get("excess_repeat_cluster_max_ticks", 0) == 0
            and item.get("smoothness_not_maximal", 0) == 0
            and item.get("mixed_policy_fault", 0) == 0
            and item.get("sync_delay_policy_holds", 0) == 0
            and item.get("delay_post_selection_rejected_sync", 0) == 0
            and item.get("wgc_smoothness_evidence_incomplete", 0) == 0
            and item.get("delay_soft_late_accepted", 0) == 0
            and item.get("delay_repeat_soft_safe_candidate", 0) == 0
            and matching_coverage
        ):
            return True
    return False


def wgc_source_delivery_period_us(media_evidence):
    worst_delivered_fps = min(
        (
            item.get("worst_delivered_fps", 0)
            for item in media_evidence["wgc_summary"]
            if item.get("worst_delivered_fps", 0) > 0
        ),
        default=0,
    )
    if worst_delivered_fps <= 0:
        return 0
    return int(math.ceil(1000000.0 / worst_delivered_fps))


def wgc_near_cap_window_pressure(media_evidence, window_us=WGC_AUDIO_LATE_RISK_WINDOW_US):
    samples = []
    day_offset_us = 0
    previous_raw_timestamp_us = -1
    for event in media_evidence["wgc_cadence_events"]:
        if "nearCap" not in event:
            continue
        raw_timestamp_us = parse_int(event.get("timestamp_us"), -1)
        if raw_timestamp_us < 0:
            raw_timestamp_us = parse_log_timestamp_us(event.get("line", ""))
        if raw_timestamp_us < 0:
            continue
        if (
            previous_raw_timestamp_us >= 0
            and previous_raw_timestamp_us - raw_timestamp_us > 12 * 60 * 60 * 1000 * 1000
        ):
            day_offset_us += 24 * 60 * 60 * 1000 * 1000
        previous_raw_timestamp_us = raw_timestamp_us
        timestamp_us = raw_timestamp_us + day_offset_us
        samples.append((timestamp_us, max(0, parse_int(event.get("nearCap"), 0))))
    samples.sort()

    accepted_total = sum(count for _timestamp_us, count in samples)
    max_accepted = 0
    max_start_us = -1
    left = 0
    rolling = 0
    for right, (timestamp_us, count) in enumerate(samples):
        rolling += count
        while left <= right and timestamp_us - samples[left][0] >= window_us:
            rolling -= samples[left][1]
            left += 1
        if rolling > max_accepted:
            max_accepted = rolling
            max_start_us = samples[left][0]
    return {
        "window_us": window_us,
        "timestamped_windows": len(samples),
        "accepted_total": accepted_total,
        "max_accepted": max_accepted,
        "max_start_us": max_start_us,
    }


def wgc_near_cap_acceptance_is_isolated(media_evidence, item):
    accepted_total = item.get("delay_near_cap_accepted", 0)
    if accepted_total < WGC_AUDIO_LATE_RISK_SOFT_ACCEPT_MIN_COUNT:
        return True
    pressure = wgc_near_cap_window_pressure(media_evidence)
    return (
        pressure["timestamped_windows"] > 0
        and pressure["accepted_total"] >= accepted_total
        and pressure["max_accepted"] < WGC_AUDIO_LATE_RISK_SOFT_ACCEPT_MIN_COUNT
    )


def wgc_source_limited_delay_is_context(
    media_evidence, item, source_limited_playout_maximal=None
):
    """True when delay variation is bounded by proven source delivery holes, not CE policy.

    A low-cadence desktop/variable-FPS source can make selected-frame age vary by its own
    delivery interval even though CFR output, A/V endpoints, and CE's lower-bound playout are
    exact. Keep genuinely actionable policy/safe-candidate/timestamp evidence strict, while
    scaling the context ceiling to the worst observed delivered-source interval.
    """
    source_period_us = wgc_source_delivery_period_us(media_evidence)
    if source_period_us <= 0:
        return False
    if source_limited_playout_maximal is None:
        source_limited_playout_maximal = has_wgc_source_limited_playout_maximal(media_evidence)
    if not (
        source_limited_playout_maximal
        and wgc_active_delay_matches_request(item)
        and wgc_has_source_limited_delay_context(media_evidence, item)
        and item.get("wgc_smoothness_verdict_complete", 0) > 0
        and item.get("source_repeat_lower_bound", 0) > 0
        and item.get("policy_added_repeats", 0) == 0
        and item.get("smoothness_not_maximal", 0) == 0
        and item.get("mixed_policy_fault", 0) == 0
        and item.get("sync_delay_policy_holds", 0) == 0
        and item.get("delay_post_selection_rejected_sync", 0) == 0
        and item.get("wgc_smoothness_evidence_incomplete", 0) == 0
        and item.get("delay_soft_late_accepted", 0) == 0
        and wgc_near_cap_acceptance_is_isolated(media_evidence, item)
        and item.get("delay_repeat_soft_safe_candidate", 0) == 0
        and item.get("delay_sync_protected_repeats", 0) > 0
    ):
        return False

    avg_limit_us = max(5000, int(math.ceil(source_period_us / 2.0)))
    p95_limit_us = max(10000, source_period_us)
    late_max_limit_us = max(WGC_RESIDUAL_ISOLATED_MAX_FAULT_US, source_period_us * 2)
    spread_limit_us = max(WGC_REALIZED_DELAY_INSTABILITY_SPREAD_US, source_period_us * 2)
    if item.get("delay_residual_avg_abs_us", 0) > avg_limit_us:
        return False
    if item.get("delay_residual_p95_us", 0) > p95_limit_us:
        return False
    if item.get("delay_residual_late_max_us", 0) > late_max_limit_us:
        return False
    if wgc_has_raw_delay_residual_evidence(item):
        if item.get("raw_residual_avg_abs_us", 0) > avg_limit_us:
            return False
        if item.get("raw_residual_p95_us", 0) > p95_limit_us:
            return False
        if item.get("raw_residual_late_max_us", 0) > late_max_limit_us:
            return False
    if item.get("predicted_residual_p95_us", 0) > p95_limit_us:
        return False
    if item.get("predicted_residual_late_max_us", 0) > late_max_limit_us:
        return False
    return wgc_realized_delay_spread_us(item) <= spread_limit_us


def wgc_realized_delay_spread_us(item):
    """Realized content-delay spread (max - min) for an active-delay smoothness summary item.

    A realized minimum of 0 is NOT "no data": it is a genuine FULL COLLAPSE of the content delay
    (the worst case -- the delay disengaged and video ran near-live), which is exactly the collapse
    half of the GPU-bound realized-delay rubber-band. Only an absent/zero MAX means no realization
    samples, so gate on delay_max alone and clamp a negative min to 0.
    """
    delay_max = item.get("realized_delay_max_us", 0)
    delay_min = item.get("realized_delay_min_us", 0)
    if delay_max <= 0:
        return 0
    if delay_min < 0:
        delay_min = 0
    if delay_max < delay_min:
        return 0
    return delay_max - delay_min


def wgc_active_delay_variation_is_source_context(
    media_evidence, item, source_limited_playout_maximal=None
):
    return wgc_source_limited_delay_is_context(
        media_evidence, item, source_limited_playout_maximal
    )


def has_wgc_active_delay_realized_delay_unstable(
    media_evidence, source_limited_playout_maximal=None
):
    """The realized content delay rubber-bands on an active-delay run.

    This is the GPU-bound under-delivery judder signature: the displayed content age swings by
    more than ~1.5 frame intervals while track lengths/PTS stay equal, so plain duration/sync
    checks (and the runtime's own ``smoothnessNotMaximal``) report the run as fine. Surfaced as a
    distinct verdict so the abnormal-judder condition is unambiguous in triage.
    """
    for item in media_evidence["wgc_smoothness_summary"]:
        if item.get("av_delay_ms", 0.0) <= 0.0:
            continue
        if (
            wgc_realized_delay_spread_us(item) >= WGC_REALIZED_DELAY_INSTABILITY_SPREAD_US
            and not wgc_active_delay_variation_is_source_context(
                media_evidence, item, source_limited_playout_maximal
            )
        ):
            return True
    return False


def has_wgc_source_limited_delay_variation_context(
    media_evidence, source_limited_playout_maximal=None
):
    return any(
        item.get("av_delay_ms", 0.0) > 0.0
        and wgc_realized_delay_spread_us(item) >= WGC_REALIZED_DELAY_INSTABILITY_SPREAD_US
        and wgc_active_delay_variation_is_source_context(
            media_evidence, item, source_limited_playout_maximal
        )
        for item in media_evidence["wgc_smoothness_summary"]
    )


def has_wgc_source_limited_smoothness_ceiling(media_evidence):
    return any(
        wgc_is_sync_protected_source_limited_ceiling(media_evidence, item)
        for item in media_evidence["wgc_smoothness_summary"]
    )


def has_wgc_source_coverage_best_effort(media_evidence):
    return bool(wgc_clean_source_coverage_items(media_evidence))


def has_wgc_smoothness_evidence_incomplete(media_evidence):
    return any(
        item.get("wgc_smoothness_evidence_incomplete", 0) > 0
        for item in media_evidence["wgc_smoothness_summary"]
    )


def has_wgc_pool_slot_lifetime_fault(media_evidence):
    return any(item.get("pool_lease_mismatch", 0) > 0 for item in media_evidence["wgc_perf"]) or any(
        item.get("pool_lease_mismatches", 0) > 0 for item in media_evidence["wgc_smoothness_summary"]
    )


def has_wgc_pool_saturated_safe_drop(media_evidence):
    return any(item.get("pool_saturated_drops", 0) > 0 for item in media_evidence["wgc_perf"]) or any(
        item.get("pool_saturated_drops", 0) > 0 for item in media_evidence["wgc_smoothness_summary"]
    )


def has_wgc_ingress_decimated(media_evidence):
    return any(
        item.get("drop_ingress", 0) > 0 or item.get("ingress_decimated", 0) > 0
        for item in media_evidence["wgc_perf"]
    ) or any(item.get("wgc_ingress_decimated", 0) > 0 for item in media_evidence["wgc_smoothness_summary"])


def has_wgc_uniform_playout_ingress_double_decimation(media_evidence):
    for item in media_evidence["wgc_smoothness_summary"]:
        if item.get("delay_uniform_hold", 0) <= 0:
            continue
        if item.get("wgc_ingress_decimated", 0) <= 0:
            continue
        accepted = item.get("wgc_source_rolling_accepted", 0)
        cfr_ticks = item.get("wgc_source_rolling_cfr_ticks", 0)
        surplus = item.get("wgc_source_rolling_surplus", 0)
        if (accepted > 0 and cfr_ticks > 0 and accepted >= cfr_ticks) or surplus > 0:
            return True
    return False


def has_wgc_copy_pool_pressure(media_evidence):
    return (
        has_wgc_pool_saturated_safe_drop(media_evidence)
        or has_wgc_ingress_decimated(media_evidence)
        or any(item.get("smoothness_retained_cap_trim", 0) > 0 for item in media_evidence["wgc_smoothness_summary"])
    )


def has_wgc_pool_evidence_missing(media_evidence):
    summary_needs_pool_evidence = any(
        item.get("smoothness_buffer_enabled", 0) > 0
        or item.get("smoothness_buffer_retained_frames", 0) > 0
        or item.get("smoothness_buffer_pool_slots", 0) > 0
        for item in media_evidence["wgc_smoothness_summary"]
    )
    summary_has_pool_evidence = any(
        item.get("pool_lifetime_evidence", 0) > 0 for item in media_evidence["wgc_smoothness_summary"]
    )
    perf_has_pool_evidence = any(item.get("pool_lease_evidence", False) for item in media_evidence["wgc_perf"])
    return summary_needs_pool_evidence and not (summary_has_pool_evidence or perf_has_pool_evidence)


def has_wgc_repeat_with_safe_candidate(media_evidence):
    has_soft_safe_evidence = any(
        "delay_repeat_soft_safe_candidate" in item for item in media_evidence["wgc_smoothness_summary"]
    )
    if has_soft_safe_evidence:
        return any(
            item.get("delay_repeat_soft_safe_candidate", 0) > 0
            and item.get("policy_added_repeats", 0) > 0
            for item in media_evidence["wgc_smoothness_summary"]
        )
    return any(
        (
            item.get("delay_repeat_safe_after_promotion", 0) > 0
            or item.get("delay_repeat_safe_candidate", 0) > 0
        )
        and item.get("policy_added_repeats", 0) > 0
        for item in media_evidence["wgc_smoothness_summary"]
    )
