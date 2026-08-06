if False:
    signature_rate = 100.0
    signature_duration_s = 260
    signal_start_s = 180
    signal_end_s = 245
    delayed_samples = 45
    left_signature = array.array("f", [0.0]) * int(signature_rate * signature_duration_s)
    right_signature = array.array("f", [0.0]) * int(signature_rate * signature_duration_s)
    for index in range(int(signal_start_s * signature_rate), int(signal_end_s * signature_rate)):
        local = index - int(signal_start_s * signature_rate)
        # Broadband deterministic fixture: several incommensurate components plus a
        # nonperiodic stepped term give cross-correlation one unambiguous 450 ms peak.
        value = (
            0.52 * math.sin(local * 0.173)
            + 0.31 * math.sin(local * 0.071 + 0.4)
            + 0.17 * (((local * 37) % 101) / 50.0 - 1.0)
        )
        left_signature[index] = value
        if index + delayed_samples < len(right_signature):
            right_signature[index + delayed_samples] = value

    late_tracks = [
        {"audio_ordinal": 0, "signature_rate": signature_rate, "signature": left_signature},
        {"audio_ordinal": 1, "signature_rate": signature_rate, "signature": right_signature},
    ]
    late_correlations = analyze_inter_track_correlations(late_tracks, focus_times_s=[signal_start_s])
    assert len(late_correlations) == 1
    late_correlation = late_correlations[0]
    assert late_correlation["content_offset_detected"]
    assert late_correlation["supporting_window_count"] >= 2
    assert math.isclose(late_correlation["detected_offset_ms"], 450.0, abs_tol=10.1)
    assert any(window["window_start_s"] >= 160.0 for window in late_correlation["supporting_windows"])

    aligned_tracks = [
        late_tracks[0],
        {"audio_ordinal": 1, "signature_rate": signature_rate, "signature": left_signature},
    ]
    aligned_correlations = analyze_inter_track_correlations(aligned_tracks, focus_times_s=[signal_start_s])
    assert len(aligned_correlations) == 1
    assert not aligned_correlations[0]["content_offset_detected"]
    assert math.isclose(aligned_correlations[0]["offset_ms"], 0.0, abs_tol=0.01)

    focus_log = (
        "[AudioLoop] Late app source live join src=2 track=1 packetStart=8640000 trackCursor=8640000 "
        "joinCursor=8640000 suppressedGap=0 preservedGap=0 qpcStart=1\n"
        "[AudioEpoch] Capture owner accepted acknowledged transition src=2 track=1 type=2 process=game.exe "
        "epoch=2->3 requested=3 acknowledged=3 trackCursor=9600000 sourceCursor=9600000\n"
    )
    assert extract_audio_correlation_focus_times(focus_log) == [180.0, 200.0]

    short_stride = compute_audio_signature_stride(48000, {"duration": "1800"})
    long_stride = compute_audio_signature_stride(48000, {}, fallback_duration=7200.0)
    assert short_stride == 192
    assert math.ceil(7200 * 48000 / long_stride) <= 500000
    assert long_stride > short_stride

    print("self-test: PASS")
