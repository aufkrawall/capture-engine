#include "dx12_av_sync_test_internal.h"

#include <cstdio>
#include <cwchar>

void WriteManifest() {
    const std::wstring path = ExeSiblingPath(L"dx12_av_sync_test_manifest.json");
    FILE* out = _wfopen(path.c_str(), L"w");
    if (!out) {
        testapp::Log("AVSYNC WARNING could not write manifest\n");
        return;
    }
    fprintf(out, "{\n");
    fprintf(out, "  \"schema\": \"ce-avsync-stimulus-v1\",\n");
    fprintf(out, "  \"process_id\": %lu,\n", GetCurrentProcessId());
    fprintf(out, "  \"requested_width\": %d,\n", g_RequestedWidth);
    fprintf(out, "  \"requested_height\": %d,\n", g_RequestedHeight);
    fprintf(out, "  \"width\": %d,\n", g_WindowWidth);
    fprintf(out, "  \"height\": %d,\n", g_WindowHeight);
    fprintf(out, "  \"fullscreen\": %d,\n", g_Fullscreen ? 1 : 0);
    fprintf(out, "  \"window_chrome\": %d,\n", g_WindowChrome ? 1 : 0);
    fprintf(out, "  \"topmost\": %d,\n", g_Topmost ? 1 : 0);
    fprintf(out, "  \"borderless_windowed\": %d,\n", (!g_Fullscreen && !g_WindowChrome) ? 1 : 0);
    fprintf(out, "  \"vsync\": %d,\n", g_VSync ? 1 : 0);
    fprintf(out, "  \"allow_tearing_requested\": %d,\n", g_TearingRequested ? 1 : 0);
    fprintf(out, "  \"allow_tearing_supported\": %d,\n", g_TearingSupported ? 1 : 0);
    fprintf(out, "  \"allow_tearing_active\": %d,\n", g_TearingActive ? 1 : 0);
    fprintf(out, "  \"present_sync_interval\": %d,\n", g_VSync ? 1 : 0);
    fprintf(out, "  \"target_fps\": %d,\n", g_TargetFps);
    fprintf(out, "  \"duration_seconds\": %d,\n", g_DurationSeconds);
    fprintf(out, "  \"analysis_start_seconds\": %.6f,\n", g_AnalysisStartSeconds);
    fprintf(out, "  \"pre_start_seconds\": %.6f,\n", testapp::avsync::kPreStartSeconds);
    fprintf(out, "  \"event_period_seconds\": %.6f,\n", testapp::avsync::kEventPeriodSeconds);
    fprintf(out, "  \"visual_marker_version\": %d,\n", testapp::avsync::kVisualMarkerVersion);
    fprintf(out, "  \"encoder_stress_scene\": %d,\n", g_EncoderStressScene ? 1 : 0);
    fprintf(out, "  \"encoder_stress_scene_reserved_event_sample\": [0.40, 0.38, 0.60, 0.52],\n");
    const auto stressLayout = testapp::avsync::ComputeEncoderStressLayout(g_WindowWidth, g_WindowHeight, kMarkerMargin,
                                                                          kMarkerTile, kMarkerGap);
    fprintf(out,
            "  \"encoder_stress_layout\": {\"valid\": %d, \"tile\": %d, \"left\": %d, \"right\": %d, "
            "\"top\": %d, \"bottom\": %d, \"columns\": %d, \"rows\": %d, "
            "\"reserve\": [%d, %d, %d, %d]},\n",
            stressLayout.valid ? 1 : 0, stressLayout.tile, stressLayout.left, stressLayout.right, stressLayout.top,
            stressLayout.bottom, stressLayout.columns, stressLayout.rows, stressLayout.reserveLeft,
            stressLayout.reserveTop, stressLayout.reserveRight, stressLayout.reserveBottom);
    fprintf(out, "  \"qpc_frequency\": %lld,\n", static_cast<long long>(g_QpcFreq.QuadPart));
    fprintf(out, "  \"app_start_qpc\": %lld,\n", static_cast<long long>(g_AppStartQpc.QuadPart));
    fprintf(out, "  \"stimulus_start_qpc\": %lld,\n", static_cast<long long>(g_StimulusStartQpc.QuadPart));
    fprintf(out, "  \"events\": [\n");
    for (int i = 0; i < static_cast<int>(testapp::avsync::kPalette.size()); ++i) {
        const auto state = testapp::avsync::StateAt(static_cast<double>(i) * testapp::avsync::kEventPeriodSeconds);
        fprintf(out,
                "    {\"index\": %d, \"palette\": %d, \"time\": %.6f, \"frequency_hz\": %.3f, "
                "\"rgb\": [%u, %u, %u]}%s\n",
                i, state.paletteIndex, state.eventStartSeconds, state.frequencyHz, state.color.r, state.color.g,
                state.color.b, i + 1 == static_cast<int>(testapp::avsync::kPalette.size()) ? "" : ",");
    }
    fprintf(out, "  ],\n");
    fprintf(out, "  \"source_stalls\": [\n");
    for (size_t i = 0; i < g_SourceStalls.size(); ++i) {
        const auto& stall = g_SourceStalls[i];
        const double actualStart = stall.actualBeginQpc.QuadPart > 0
                                       ? QpcToSeconds(stall.actualBeginQpc.QuadPart - g_StimulusStartQpc.QuadPart)
                                       : -1.0;
        const double actualEnd = stall.actualEndQpc.QuadPart > 0
                                     ? QpcToSeconds(stall.actualEndQpc.QuadPart - g_StimulusStartQpc.QuadPart)
                                     : -1.0;
        const double expectedRepeatSpan =
            static_cast<double>(stall.suppressedPresentCount) / static_cast<double>(std::max(1, g_TargetFps));
        fprintf(out,
                "    {\"index\": %zu, \"requested_start_seconds\": %.6f, \"requested_duration_ms\": %.3f, "
                "\"requested_end_seconds\": %.6f, \"actual_begin_qpc\": %lld, \"actual_end_qpc\": %lld, "
                "\"actual_start_seconds\": %.6f, \"actual_end_seconds\": %.6f, "
                "\"suppressed_present_count\": %llu, \"expected_repeat_span_seconds\": %.6f, "
                "\"tolerance_seconds\": %.6f}%s\n",
                i, stall.spec.startSeconds, stall.spec.durationSeconds * 1000.0, stall.spec.EndSeconds(),
                static_cast<long long>(stall.actualBeginQpc.QuadPart),
                static_cast<long long>(stall.actualEndQpc.QuadPart), actualStart, actualEnd,
                static_cast<unsigned long long>(stall.suppressedPresentCount), expectedRepeatSpan,
                testapp::avsync::kDefaultSourceStallToleranceSeconds, i + 1 == g_SourceStalls.size() ? "" : ",");
    }
    fprintf(out, "  ],\n");
    fprintf(out, "  \"frame_marker_bits\": %d,\n", testapp::avsync::kFrameMarkerBits);
    fprintf(out, "  \"frame_marker_redundancy\": \"primary_inverse_checksum_parity\",\n");
    fprintf(out, "  \"marker_tile\": %ld,\n", static_cast<long>(kMarkerTile));
    fprintf(out, "  \"marker_margin\": %ld,\n", static_cast<long>(kMarkerMargin));
    fprintf(out, "  \"marker_gap\": %ld,\n", static_cast<long>(kMarkerGap));
    fprintf(out, "  \"motion_lane_margin\": %ld,\n", static_cast<long>(kMarkerMargin));
    fprintf(out, "  \"motion_lane_count\": %d,\n", 2);
    fprintf(out, "  \"motion_lane_top_ratio\": %.6f,\n", 0.72);
    fprintf(out, "  \"motion_lane_height\": %d,\n", 42);
    fprintf(out, "  \"motion_lane_bar_width\": %d,\n", 96);
    fprintf(out, "  \"motion_lane_speed_cycles_per_second\": %.6f,\n", 0.25);
    fprintf(out, "  \"motion_lane_expected_function\": \"fmod(stimulus_seconds*0.25,1.0)\",\n");
    fprintf(out, "  \"fast_motion_lane_top_ratio\": %.6f,\n", 0.82);
    fprintf(out, "  \"fast_motion_lane_bar_width\": %d,\n", 48);
    fprintf(out, "  \"fast_motion_lane_speed_cycles_per_second\": %.6f,\n", 1.0);
    fprintf(out, "  \"fast_motion_lane_expected_function\": \"fmod(stimulus_seconds*1.0,1.0)\",\n");
    fprintf(out, "  \"audio_requested_buffer_ms\": %d,\n", g_AudioBufferMs);
    fprintf(out, "  \"audio_stimulus_lead_ms\": %.3f,\n", g_Audio.AudioLeadMs());
    fprintf(out, "  \"audio_render_latency_us\": %llu,\n",
            static_cast<unsigned long long>(g_Audio.StreamLatency100ns() / 10));
    fprintf(out, "  \"audio_buffer_frames\": %u,\n", g_Audio.BufferFrames());
    fprintf(out, "  \"audio_clock_available\": %d,\n", g_Audio.HasAudioClock() ? 1 : 0);
    fprintf(out, "  \"audio_clock_scheduling\": %d,\n", g_Audio.AudioClockSchedulingEnabled() ? 1 : 0);
    fprintf(out, "  \"audio_clock_frequency\": %llu,\n",
            static_cast<unsigned long long>(g_Audio.AudioClockFrequency()));
    const double avgPresentDeltaMs =
        g_PresentDeltaCount == 0 ? 0.0 : g_PresentDeltaSumMs / static_cast<double>(g_PresentDeltaCount);
    fprintf(out, "  \"frame_pacing\": {\n");
    fprintf(out, "    \"timer_high_resolution\": %d,\n", g_FrameTimerHighResolution ? 1 : 0);
    fprintf(out, "    \"target_interval_ms\": %.6f,\n", TargetFrameIntervalMs());
    fprintf(out, "    \"spike_threshold_ms\": %.6f,\n", FramePacingSpikeThresholdMs());
    fprintf(out, "    \"present_delta_count\": %llu,\n", static_cast<unsigned long long>(g_PresentDeltaCount));
    fprintf(out, "    \"average_present_delta_ms\": %.6f,\n", avgPresentDeltaMs);
    fprintf(out, "    \"max_present_delta_ms\": %.6f,\n", g_MaxPresentDeltaMs);
    fprintf(out, "    \"spike_count\": %llu,\n", static_cast<unsigned long long>(g_FramePacingSpikeCount));
    fprintf(out, "    \"warmup_spike_count\": %llu,\n", static_cast<unsigned long long>(g_WarmupPacingSpikeCount));
    fprintf(out, "    \"event_boundary_spike_count\": %llu,\n",
            static_cast<unsigned long long>(g_EventBoundaryPacingSpikeCount));
    fprintf(out, "    \"planned_source_gap_count\": %llu\n", static_cast<unsigned long long>(g_PlannedPresentGapCount));
    fprintf(out, "  }\n");
    fprintf(out, "}\n");
    fclose(out);
    testapp::Log("AVSYNC START manifest=%ls\n", path.c_str());
}
void LogEventSchedule() {
    for (int i = 0; i < static_cast<int>(testapp::avsync::kPalette.size()); ++i) {
        const auto state = testapp::avsync::StateAt(static_cast<double>(i) * testapp::avsync::kEventPeriodSeconds);
        testapp::Log("AVSYNC EVENT index=%d palette=%d time=%.6f freq=%.1f rgb=%u,%u,%u\n", i, state.paletteIndex,
                     state.eventStartSeconds, state.frequencyHz, state.color.r, state.color.g, state.color.b);
    }
}
void LogSourceStallSchedule() {
    for (size_t i = 0; i < g_SourceStalls.size(); ++i) {
        const auto& stall = g_SourceStalls[i];
        testapp::Log(
            "AVSYNC EVENT source_stall index=%zu requestedStart=%.6f requestedDurationMs=%.3f requestedEnd=%.6f "
            "toleranceSeconds=%.6f\n",
            i, stall.spec.startSeconds, stall.spec.durationSeconds * 1000.0, stall.spec.EndSeconds(),
            testapp::avsync::kDefaultSourceStallToleranceSeconds);
    }
}
