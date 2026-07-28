

def has_wgc_post_stall_recovery_fault(media_evidence):
    has_soft_safe_evidence = any(
        "delay_repeat_soft_safe_candidate" in item for item in media_evidence["wgc_smoothness_summary"]
    )
    return any(
        item.get("delay_repeat_state_post_stall", 0) > 0
        and (
            (
                item.get("delay_repeat_soft_safe_candidate", 0) > 0
                if has_soft_safe_evidence
                else (
                    item.get("delay_repeat_safe_candidate", 0) > 0
                    or item.get("delay_repeat_safe_after_promotion", 0) > 0
                )
            )
            or item.get("policy_added_repeats", 0) > 0
        )
        for item in media_evidence["wgc_smoothness_summary"]
    )


def has_wgc_sync_delay_reserve_pressure(media_evidence):
    for item in media_evidence["wgc_smoothness_summary"]:
        requested_delay_ms = item.get("av_delay_ms", 0.0)
        if requested_delay_ms <= 0.0:
            continue
        bounded_source_limited = wgc_is_bounded_source_limited_active_delay(media_evidence, item)
        if item.get("sync_delay_policy_holds", 0) > 0 and not bounded_source_limited:
            continue
        if item.get("sync_delay_source_limited_holds", 0) > 0:
            return True
        if bounded_source_limited:
            return True

        realized_delay_matches = wgc_active_delay_matches_request(item)
        if realized_delay_matches and 0 < item.get("sync_delay_holds", 0) < 120:
            return True

    return False


def summarize_audio_ingest_starvation(media_evidence, sample_rate=48000):
    """Consumer-overrun evidence for the `logs/audiodeath` failure class.

    A CFR recording can end with sample-exact track lengths, zero packet gaps, and zero
    drift while every audio track is silent, because the exported cursor ran past the
    live capture edge and each later packet was destroyed as timeline overlap. Track
    length can never surface that, so the per-source destroyed-sample counter and the
    real-vs-silence split of each exported track are the strict signals.
    """
    entries = media_evidence.get("stop_audio_ingest", [])
    destroyed_samples = sum(item.get("starved_samples", 0) for item in entries)
    resync_events = sum(item.get("resync_events", 0) for item in entries)
    resync_samples = sum(item.get("resync_samples", 0) for item in entries)
    reservoir_peak_ms = max((item.get("reservoir_peak_ms", 0) for item in entries), default=0)
    affected = [item for item in entries if item.get("starved_samples", 0) > 0]

    legacy_overlap_sources = []
    if not entries:
        # Pre-counter logs: a source that discarded a large fraction of its own timeline as
        # packet overlap was starved by the consumer. Ordinary boundary de-duplication is a
        # few hundred samples per recording, so require both an absolute second of loss and
        # a meaningful share of the exported timeline before calling it a fault.
        encoded_by_source = {
            item["source"]: item.get("encoded_samples", 0)
            for item in media_evidence.get("stop_audio_sources", [])
        }
        for item in media_evidence.get("stop_audio_overlap", []):
            overlap = item.get("overlap_samples", 0)
            encoded = encoded_by_source.get(item.get("source"), 0)
            if overlap < sample_rate:
                continue
            if encoded > 0 and overlap * 50 < encoded:
                continue
            legacy_overlap_sources.append(
                {
                    "source": item.get("source"),
                    "track": None,
                    "starved_samples": overlap,
                    "process": "",
                }
            )
        if legacy_overlap_sources:
            destroyed_samples = sum(item["starved_samples"] for item in legacy_overlap_sources)
            affected = legacy_overlap_sources

    return {
        "evidence_available": bool(entries),
        "legacy_overlap_evidence": bool(legacy_overlap_sources),
        "destroyed_samples": destroyed_samples,
        "destroyed_ms": (destroyed_samples * 1000.0 / sample_rate) if sample_rate > 0 else 0.0,
        "resync_events": resync_events,
        "resync_samples": resync_samples,
        "reservoir_peak_ms": reservoir_peak_ms,
        "affected_sources": [
            {
                "source": item.get("source"),
                "track": item.get("track"),
                "starved_samples": item.get("starved_samples", 0),
                "process": item.get("process", ""),
            }
            for item in affected
        ],
    }


def summarize_stop_audio_shortfalls(media_evidence):
    short_tracks = [
        item for item in media_evidence["stop_audio_tracks"]
        if item["diff_samples"] < -48000 or item["diff_ms"] < -1000.0
    ]
    multi_source_short_tracks = [item for item in short_tracks if len(item["sources"]) > 1]
    return {
        "short_count": len(short_tracks),
        "multi_source_short_count": len(multi_source_short_tracks),
        "worst_shortfall_ms": min((item["diff_ms"] for item in short_tracks), default=0.0),
        "tracks": short_tracks,
    }


def is_sparse_app_source_silence(item):
    return (
        bool(item.get("process")) and item.get("process") != "-"
        and item.get("ring_peak_samples", 0) == 0
        and (item.get("pad_samples", 0) > 0 or item.get("ring_underruns", 0) > 0)
    )


def summarize_started_app_source_health(media_evidence, log_summary):
    counts = log_summary["counts"] if log_summary else {}
    stop_sources = media_evidence["stop_audio_sources"]
    late_join_sources = [
        item for item in stop_sources
        if item.get("late_join_suppressed_samples", 0) > 0 or item.get("late_join_preserved_samples", 0) > 0
    ]
    backlog_sources = [
        item for item in stop_sources
        if item.get("process") and item.get("process") != "-"
        and item.get("packet_gap_samples", 0) >= 48000
        and item.get("late_join_suppressed_samples", 0) == 0
    ]
    underrun_sources = [
        item for item in stop_sources if item.get("ring_underruns", 0) > 0 or item.get("pad_samples", 0) > 0
    ]
    sparse_silence_sources = [item for item in underrun_sources if is_sparse_app_source_silence(item)]
    active_underrun_sources = [item for item in underrun_sources if not is_sparse_app_source_silence(item)]
    return {
        "late_join_live_count": counts.get("audio_late_app_live_join", 0),
        "late_source_backlog_count": counts.get("audio_late_app_source_backlog", 0),
        "app_gap_silence_count": counts.get("audio_app_source_gap_silence", 0),
        "late_join_sources": late_join_sources,
        "backlog_sources": backlog_sources,
        "underrun_sources": underrun_sources,
        "sparse_silence_sources": sparse_silence_sources,
        "active_underrun_sources": active_underrun_sources,
    }


def summarize_app_audio_latency(media_evidence, log_summary, stop_start_wall_us=-1):
    warning_rows = media_evidence.get("app_latency_warnings", [])
    if warning_rows:
        stop_warning_count = sum(
            1
            for item in warning_rows
            if stop_start_wall_us >= 0 and item.get("timestamp_us", -1) >= stop_start_wall_us
        )
        warning_count = len(warning_rows) - stop_warning_count
    else:
        stop_warning_count = 0
        warning_count = log_summary["counts"].get("audio_app_latency_elevated", 0) if log_summary else 0
    stop_drain_only = stop_warning_count > 0 and warning_count == 0
    sources = media_evidence.get("stop_app_audio_latency", [])
    elevated_sources = []
    stop_context_sources = []
    for item in sources:
        excess_avg = item.get("excess_avg_ms")
        excess_max = item.get("excess_max_ms")
        if excess_avg is not None and excess_max is not None:
            elevated = excess_avg >= 40.0 or excess_max >= 80
        else:
            elevated = item.get("avg_ms", 0.0) >= 250.0 or item.get("max_ms", 0) >= 300
        if elevated and stop_drain_only and not item.get("phase_split", False):
            stop_context_sources.append(item)
        elif elevated:
            elevated_sources.append(item)

    queue_overrun_packets = sum(item.get("queue_overrun_packets", 0) for item in sources)
    queue_overrun_frames = sum(item.get("queue_overrun_frames", 0) for item in sources)
    underruns = sum(item.get("underruns", 0) for item in sources)
    catastrophic_resync_events = sum(item.get("catastrophic_resync_events", 0) for item in sources)
    integrity_fault = (
        queue_overrun_packets > 0
        or queue_overrun_frames > 0
        or underruns > 0
        or catastrophic_resync_events > 0
    )
    # Legacy logs without a final latency distribution cannot prove that live warnings stayed
    # inside the moving video-delay target. Preserve their strict classification. Current logs can
    # clear warning chatter only with an explicit, non-elevated, integrity-clean stop summary.
    warning_without_summary = warning_count > 0 and not sources
    fault_evidence = bool(elevated_sources) or integrity_fault or warning_without_summary
    warning_only_context = warning_count > 0 and not fault_evidence

    return {
        "warning_count": warning_count,
        "stop_drain_warning_count": stop_warning_count,
        "stop_drain_only": stop_drain_only and not elevated_sources,
        "source_count": len(sources),
        "elevated_source_count": len(elevated_sources),
        "worst_avg_ms": max((item.get("avg_ms", 0.0) for item in sources), default=0.0),
        "worst_max_ms": max((item.get("max_ms", 0) for item in sources), default=0),
        "worst_excess_avg_ms": max(
            (item.get("excess_avg_ms") for item in sources if item.get("excess_avg_ms") is not None),
            default=0.0,
        ),
        "worst_excess_max_ms": max(
            (item.get("excess_max_ms") for item in sources if item.get("excess_max_ms") is not None),
            default=0,
        ),
        "max_comp_percent": max((item.get("max_comp_percent", 0.0) for item in sources), default=0.0),
        "queue_overrun_packets": queue_overrun_packets,
        "queue_overrun_frames": queue_overrun_frames,
        "underruns": underruns,
        "catastrophic_resync_events": catastrophic_resync_events,
        "integrity_fault": integrity_fault,
        "warning_without_summary": warning_without_summary,
        "fault_evidence": fault_evidence,
        "warning_only_context": warning_only_context,
        "sources": sources,
        "elevated_sources": elevated_sources,
        "stop_context_sources": stop_context_sources,
    }
