

def update_wgc_smoothness_item_from_line(item, line):
    smoothness_extra = WGC_SMOOTHNESS_EXTRA_RE.search(line)
    if smoothness_extra:
        item.update(
            {
                "sync_delay_source_limited_holds": parse_int(smoothness_extra.group(1)),
                "sync_delay_policy_holds": parse_int(smoothness_extra.group(2)),
                "startup_reserve_frames": parse_int(smoothness_extra.group(3)),
                "startup_reserve_span_us": parse_int(smoothness_extra.group(4)),
                "startup_delay_target_us": parse_int(smoothness_extra.group(5)),
                "startup_reserve_selected": parse_int(smoothness_extra.group(6)),
                "startup_reserve_reason": smoothness_extra.group(7),
            }
        )

    smoothness_buffer = WGC_SMOOTHNESS_BUFFER_RE.search(line)
    if smoothness_buffer:
        groups = smoothness_buffer.groupdict()
        item.update(
            {
                "smoothness_buffer_enabled": parse_int(groups.get("enabled")),
                "smoothness_buffer_target_ms": parse_int(groups.get("target_ms")),
                "smoothness_buffer_actual_frames": parse_int(groups.get("actual_frames")),
                "smoothness_buffer_retained_frames": parse_int(groups.get("retained_frames")),
                "smoothness_buffer_desired_frames": parse_int(groups.get("desired_frames")),
                "smoothness_buffer_delay_ms": parse_float(groups.get("delay_ms")),
                "smoothness_buffer_pool_slots": parse_int(groups.get("pool_slots")),
                "pool_lifetime_evidence": 1 if groups.get("source_buffers") is not None else 0,
                "smoothness_source_frame_pool_buffers": parse_int(groups.get("source_buffers")),
                "smoothness_budget_surfaces": parse_int(groups.get("budget_surfaces")),
                "smoothness_sync_frames": parse_int(groups.get("sync_frames")),
                "smoothness_extra_frames": parse_int(groups.get("extra_frames")),
                "smoothness_retained_frame_cap": parse_int(groups.get("retained_cap")),
                "smoothness_reserved_free_slots": parse_int(groups.get("reserved_free_slots")),
                "smoothness_safety_slots": parse_int(groups.get("safety_slots")),
                "smoothness_retained_cap_trim": parse_int(groups.get("retained_cap_trim")),
                "wgc_ingress_accepted": parse_int(groups.get("ingress_accepted")),
                "wgc_ingress_decimated": parse_int(groups.get("ingress_decimated")),
                "wgc_ingress_accepted_playout_soft": parse_int(groups.get("ingress_play_soft")),
                "wgc_ingress_accepted_playout_credit": parse_int(groups.get("ingress_play_credit")),
                "wgc_ingress_retained_frames": parse_int(groups.get("ingress_retained")),
                "wgc_ingress_retained_cap": parse_int(groups.get("ingress_cap")),
                "wgc_ingress_low_water": parse_int(groups.get("ingress_low_water")),
                "pool_lease_max": parse_int(groups.get("leased_max")),
                "pool_free_now": parse_int(groups.get("free_now")),
                "pool_free_min": parse_int(groups.get("free_min")),
                "pool_pressure_trim": parse_int(groups.get("pool_pressure_trim")),
                "pool_saturated_drops": parse_int(groups.get("pool_saturated_drops")),
                "pool_overwrite_prevented": parse_int(groups.get("overwrite_prevented")),
                "pool_lease_mismatches": parse_int(groups.get("lease_mismatches")),
                "smoothness_buffer_vram_mb": parse_float(groups.get("vram_mb") or "0"),
                "smoothness_buffer_cap_limited": parse_int(groups.get("cap_limited")),
                "smoothness_buffer_reason": groups.get("reason") or "",
            }
        )

    ingress = WGC_SMOOTHNESS_INGRESS_RE.search(line)
    if ingress:
        groups = ingress.groupdict()
        item.update(
            {
                "wgc_ingress_accepted": parse_int(groups.get("accepted")),
                "wgc_ingress_decimated": parse_int(groups.get("decimated")),
                "wgc_ingress_retained_frames": parse_int(groups.get("retained")),
                "wgc_ingress_retained_cap": parse_int(groups.get("cap")),
                "wgc_ingress_low_water": parse_int(groups.get("low_water")),
                "wgc_ingress_accepted_low_water": parse_int(groups.get("acc_low_water")),
                "wgc_ingress_accepted_recovery": parse_int(groups.get("acc_recovery")),
                "wgc_ingress_accepted_source_below": parse_int(groups.get("acc_source_below")),
                "wgc_ingress_accepted_healthy": parse_int(groups.get("acc_healthy")),
                "wgc_ingress_accepted_playout_soft": parse_int(groups.get("acc_play_soft")),
                "wgc_ingress_accepted_playout_credit": parse_int(groups.get("acc_play_credit")),
                "wgc_ingress_decimated_soft_reserve": parse_int(groups.get("dec_soft_reserve")),
                "wgc_ingress_decimated_hard_reserve": parse_int(groups.get("dec_hard_reserve")),
                "wgc_ingress_decimated_credit": parse_int(groups.get("dec_credit")),
                "wgc_ingress_soft_reserve_pressure": parse_int(groups.get("soft_pressure")),
                "wgc_ingress_hard_reserve_pressure": parse_int(groups.get("hard_pressure")),
                "wgc_duplicate_timestamps_seen": parse_int(groups.get("dup_ts_seen")),
                "wgc_duplicate_timestamps_skipped": parse_int(groups.get("dup_ts_skipped")),
                "wgc_ingress_last_reason": groups.get("last_reason") or "",
            }
        )

    source = WGC_SMOOTHNESS_SOURCE_RE.search(line)
    if source:
        groups = source.groupdict()
        item.update(
            {
                "wgc_source_accepted_total": parse_int(groups.get("accepted_total")),
                "wgc_source_cfr_ticks_total": parse_int(groups.get("cfr_ticks_total")),
                "wgc_source_rolling_accepted": parse_int(groups.get("rolling_accepted")),
                "wgc_source_rolling_cfr_ticks": parse_int(groups.get("rolling_cfr_ticks")),
                "wgc_source_rolling_deficit": parse_int(groups.get("rolling_deficit")),
                "wgc_source_rolling_surplus": parse_int(groups.get("rolling_surplus")),
                "wgc_source_last_window_accepted": parse_int(groups.get("last_window_accepted")),
                "wgc_source_last_window_cfr_ticks": parse_int(groups.get("last_window_cfr_ticks")),
                "wgc_source_window_slots": parse_int(groups.get("window_slots")),
            }
        )

    delay_realization = WGC_DELAY_REALIZATION_RE.search(line)
    if delay_realization:
        item.update(
            {
                "delay_reservoir_low_water_frames": parse_int(delay_realization.group(1)),
                "delay_reservoir_target_frames": parse_int(delay_realization.group(2)),
                "delay_reservoir_low_water_ticks": parse_int(delay_realization.group(3)),
                "realized_delay_avg_us": parse_int(delay_realization.group(4)),
                "realized_delay_min_us": parse_int(delay_realization.group(5)),
                "realized_delay_max_us": parse_int(delay_realization.group(6)),
                "delay_residual_avg_signed_us": parse_int(delay_realization.group(7)),
                "delay_residual_avg_abs_us": parse_int(delay_realization.group(8)),
                "delay_residual_max_us": parse_int(delay_realization.group(9)),
                "delay_residual_p95_us": parse_int(delay_realization.group(10)),
                "delay_residual_late_max_us": parse_int(delay_realization.group(11)),
                "delay_residual_early_max_us": parse_int(delay_realization.group(12)),
            }
        )

    floor = WGC_SMOOTHNESS_FLOOR_RE.search(line)
    if floor:
        item.update(
            {
                "smooth_floor_source": floor.group(1),
                "smooth_floor_configured": parse_int(floor.group(2)),
                "smooth_floor_ms": parse_int(floor.group(3)),
                "smooth_floor_requested_us": parse_int(floor.group(4)),
                "smooth_floor_delay_us": parse_int(floor.group(5)),
                "smooth_floor_clamped_by": floor.group(6),
                "smooth_floor_realized_target_us": parse_int(floor.group(7)),
                "smooth_floor_delivery_gap_avg_us": parse_int(floor.group(8)),
                "smooth_floor_delivery_gap_max_us": parse_int(floor.group(9)),
                "smooth_floor_source_jitter_avg_us": parse_int(floor.group(10)),
                "smooth_floor_source_jitter_max_us": parse_int(floor.group(11)),
                "smooth_floor_realized_min_us": parse_int(floor.group(12)),
                "smooth_floor_realized_avg_us": parse_int(floor.group(13)),
                "smooth_floor_realized_max_us": parse_int(floor.group(14)),
                "smooth_floor_residual_late_max_us": parse_int(floor.group(15)),
                "smooth_floor_av_content_delay_active": parse_int(floor.group(16)),
            }
        )

    delay_raw = WGC_DELAY_RAW_RESIDUAL_RE.search(line)
    if delay_raw:
        item.update(
            {
                "raw_residual_avg_signed_us": parse_int(delay_raw.group(1)),
                "raw_residual_avg_abs_us": parse_int(delay_raw.group(2)),
                "raw_residual_max_us": parse_int(delay_raw.group(3)),
                "raw_residual_p95_us": parse_int(delay_raw.group(4)),
                "raw_residual_late_max_us": parse_int(delay_raw.group(5)),
                "raw_residual_early_max_us": parse_int(delay_raw.group(6)),
                "predicted_residual_avg_signed_us": parse_int(delay_raw.group(7)),
                "predicted_residual_avg_abs_us": parse_int(delay_raw.group(8)),
                "predicted_residual_p95_us": parse_int(delay_raw.group(9)),
                "predicted_residual_late_max_us": parse_int(delay_raw.group(10)),
                "raw_minus_predicted_avg_signed_us": parse_int(delay_raw.group(11)),
                "raw_minus_predicted_avg_abs_us": parse_int(delay_raw.group(12)),
                "raw_minus_predicted_max_us": parse_int(delay_raw.group(13)),
            }
        )

    delay_relaxed = WGC_DELAY_RELAXED_RE.search(line)
    if delay_relaxed:
        item.update(
            {
                "delay_relaxed_selections": parse_int(delay_relaxed.group(1)),
                "delay_relaxed_max_us": parse_int(delay_relaxed.group(2)),
                "delay_relaxed_rejected_sync": parse_int(delay_relaxed.group(3)),
                "delay_repeat_cluster_pressure": parse_int(delay_relaxed.group(4)),
                "delay_repeat_cluster_max_ticks": parse_int(delay_relaxed.group(5)),
                "delay_relaxed_better_target": parse_int(delay_relaxed.group(6)),
                "delay_relaxed_repeat_cluster": parse_int(delay_relaxed.group(7)),
                "delay_relaxed_rejected_headroom": parse_int(delay_relaxed.group(8)),
                "delay_relaxed_rejected_cost": parse_int(delay_relaxed.group(9)),
                "delay_soft_late_rejected": parse_int(delay_relaxed.group(10)),
                "delay_soft_late_accepted": parse_int(delay_relaxed.group(11)),
                "delay_older_frame_avoided_repeat": parse_int(delay_relaxed.group(12)),
                "delay_source_limited_repeats": parse_int(delay_relaxed.group(13)),
                "delay_source_recovery_holds": parse_int(delay_relaxed.group(14)),
                "delay_source_recovery_ticks": parse_int(delay_relaxed.group(15)),
            }
        )

    delay_repeat_rescue = WGC_DELAY_REPEAT_RESCUE_RE.search(line)
    if delay_repeat_rescue:
        repeat_fields = {
            "delay_repeat_rescue_success": parse_int(delay_repeat_rescue.group(1)),
            "delay_repeat_rescue_attempts": parse_int(delay_repeat_rescue.group(2)),
            "delay_repeat_rescue_rejected_sync": parse_int(delay_repeat_rescue.group(3)),
            "delay_repeat_rescue_rejected_headroom": parse_int(delay_repeat_rescue.group(4)),
            "delay_repeat_rescue_rejected_cost": parse_int(delay_repeat_rescue.group(5)),
            "delay_repeat_promoted_before_repeat": parse_int(delay_repeat_rescue.group(6)),
            "delay_repeat_promotion_attempts": parse_int(delay_repeat_rescue.group(7)),
            "delay_repeat_promotion_rejected_soft": parse_int(delay_repeat_rescue.group(8)),
            "delay_repeat_safe_after_promotion": parse_int(delay_repeat_rescue.group(9)),
            "delay_repeat_safe_candidate": parse_int(delay_repeat_rescue.group(10)),
            "delay_repeat_no_safe_candidate": parse_int(delay_repeat_rescue.group(11)),
            "delay_repeat_window_healthy": parse_int(delay_repeat_rescue.group(14)),
            "delay_repeat_window_recoverable": parse_int(delay_repeat_rescue.group(15)),
            "delay_repeat_window_source_limited": parse_int(delay_repeat_rescue.group(16)),
            "delay_repeat_state_healthy": parse_int(delay_repeat_rescue.group(17)),
            "delay_repeat_state_recoverable": parse_int(delay_repeat_rescue.group(18)),
            "delay_repeat_state_source_limited": parse_int(delay_repeat_rescue.group(19)),
            "delay_repeat_state_hard_stall": parse_int(delay_repeat_rescue.group(20)),
            "delay_repeat_state_post_stall": parse_int(delay_repeat_rescue.group(21)),
            "delay_post_stall_safe_frames": parse_int(delay_repeat_rescue.group(22)),
            "delay_repeat_reserve_depth_max": parse_int(delay_repeat_rescue.group(23)),
            "delay_repeat_reserve_span_max_us": parse_int(delay_repeat_rescue.group(24)),
        }
        if delay_repeat_rescue.group(12) is not None:
            repeat_fields["delay_repeat_soft_safe_candidate"] = parse_int(delay_repeat_rescue.group(12))
            repeat_fields["delay_repeat_no_soft_safe_candidate"] = parse_int(delay_repeat_rescue.group(13))
        item.update(repeat_fields)

    repeat_named_fields = {
        "delayNearCapAccepted": "delay_near_cap_accepted",
        "delayHardOnlyCandidates": "delay_hard_only_candidates",
        "delaySyncProtectedRepeats": "delay_sync_protected_repeats",
        "delayOldestSoftSafeAgeMax": "delay_oldest_soft_safe_age_max_us",
        "delayUniformCadence": "delay_uniform_cadence",
        "delayUniformHold": "delay_uniform_hold",
        "delayPaceCapTrim": "delay_pace_cap_trim",
    }
    for field_name, key in repeat_named_fields.items():
        value = parse_named_int_field(line, field_name)
        if value is not None:
            item[key] = value

    lower_bound_fields = {
        "delayPostSelectionRejectedSync": "delay_post_selection_rejected_sync",
        "delayPostSelectionRescuedSync": "delay_post_selection_rescued_sync",
        "sourceRepeatLowerBound": "source_repeat_lower_bound",
        "syncSourceRepeatLowerBound": "sync_source_repeat_lower_bound",
        "deliveryRepeatLowerBound": "delivery_repeat_lower_bound",
        "policyNoSourceRepeats": "policy_no_source_repeats",
        "excessRepeats": "excess_repeats",
        "policyAddedRepeats": "policy_added_repeats",
        "excessRepeatClusters": "excess_repeat_clusters",
        "excessRepeatClusterMax": "excess_repeat_cluster_max_ticks",
        "smoothnessNotMaximal": "smoothness_not_maximal",
        "mixedPolicyFault": "mixed_policy_fault",
    }
    saw_lower_bound_tail = False
    for field_name, key in lower_bound_fields.items():
        value = parse_named_int_field(line, field_name)
        if value is None:
            continue
        item[key] = value
        if field_name != "delayPostSelectionRejectedSync":
            saw_lower_bound_tail = True

    if parse_named_int_field(line, "smoothnessNotMaximal") is not None:
        item["wgc_smoothness_verdict_complete"] = 1
    elif saw_lower_bound_tail:
        item["wgc_smoothness_evidence_incomplete"] = 1

    smooth_delay_fields = {
        "smoothTargetDelay": "smooth_target_delay_us",
        "smoothActualDelay": "smooth_actual_delay_us",
        "smoothDelayDeficit": "smooth_delay_deficit_us",
        "startupDelayDeficit": "startup_delay_deficit_us",
        "syncProtectedRepeats": "delay_sync_protected_repeats",
    }
    for field_name, key in smooth_delay_fields.items():
        value = parse_named_int_field(line, field_name)
        if value is not None:
            item[key] = value

    retained_fields = {
        "sourceFmt": ("source_format", parse_named_int_field),
        "retainedFmt": ("retained_format", parse_named_int_field),
        "compactRetained": ("compact_retained", parse_named_int_field),
        "sourceBudgetMB": ("source_budget_mb", parse_named_float_field),
        "copyBudgetMB": ("copy_budget_mb", parse_named_float_field),
        "sourceSurfaceMB": ("source_surface_mb", parse_named_float_field),
        "copySurfaceMB": ("copy_surface_mb", parse_named_float_field),
        "convertUs": ("convert_us", parse_named_int_field),
    }
    for field_name, (key, parser) in retained_fields.items():
        value = parser(line, field_name)
        if value is not None:
            item[key] = value
