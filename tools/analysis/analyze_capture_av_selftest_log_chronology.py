def _self_test_log_chronology_and_aliases():
    # Calendar dates are part of the shared wall-clock domain. A recording may
    # span midnight, month/year boundaries, or more than one day.
    for before, after in (
        ("2026-10-09 23:59:59.999", "2026-10-10 00:00:00.000"),
        ("2026-10-31 23:59:59.999", "2026-11-01 00:00:00.000"),
        ("2026-12-31 23:59:59.999", "2027-01-01 00:00:00.000"),
    ):
        start = parse_log_timestamp_us(f"[{before}] [INFO] before")
        end = parse_log_timestamp_us(f"[{after}] [INFO] after")
        assert end - start == 1000, (before, after, start, end)
    assert parse_log_timestamp_us("[2026-10-10 00:00:00.000001]") % 1000000 == 1
    assert parse_log_timestamp_us("[2026-02-30 00:00:00.000]") == -1
    assert parse_log_timestamp_us("undated") == -1

    media = (
        "[2026-10-09 23:59:58.000] [INFO] [EncoderThread] Recording live liveStartQpc=1000000\n"
        "[2026-10-09 23:59:59.000] [INFO] [AppLatency] WARNING: app audio before midnight\n"
        "[2026-10-10 00:00:01.000] [INFO] [AppLatency] WARNING: app audio after midnight\n"
        "[2026-10-10 00:00:02.000] [INFO] [Media] Stopping recording...\n"
        "[2026-10-10 00:00:03.000] [INFO] [AppLatency] WARNING: app audio stop drain\n"
    )
    evidence = parse_media_triage(media)
    latency = summarize_app_audio_latency(evidence, analyze_log_text(media), parse_stop_start_wall_us(media))
    assert latency["warning_count"] == 2
    assert latency["stop_drain_warning_count"] == 1
    window = build_full_recording_perf_window_info(media, [])
    assert window and window["active"]
    assert window["end_s"] == 4.0
    selected = filter_media_text_for_recording_window(media, window)
    assert "before midnight" in selected and "after midnight" in selected
    assert "Stopping recording" not in selected and "stop drain" not in selected
    narrow = build_recording_window_info(media, "2:4", [])
    selected = filter_media_text_for_recording_window(media, narrow)
    assert "after midnight" in selected and "before midnight" not in selected

    # DropIngress and Ingress.decimated are aliases of the same per-window
    # counter, not distinct loss domains. Preserve older single-field logs.
    with tempfile.TemporaryDirectory() as directory:
        session = Path(directory)
        for fields, expected in (
            ("DropIngress: 49 | Ingress: accepted=10 decimated=49", 49),
            ("DropIngress: 9 | Ingress: accepted=10 decimated=9", 9),
            ("DropIngress: 7", 7),
            ("Ingress: accepted=10 decimated=5", 5),
            ("DropIngress: 0 | Ingress: accepted=10 decimated=0", 0),
            ("Input: 10", 0),
        ):
            (session / "media.log").write_text(f"[WGC Perf] {fields}\n", encoding="utf-8")
            report = classify_session_triage(session)
            actual = report["evidence"]["wgc_perf_worst"]["ingress_decimated"]
            assert actual == expected, (fields, actual, expected)
