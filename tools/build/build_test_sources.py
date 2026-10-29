

# Product units that unit_tests.exe compiles itself, and the lists' readers. build.py runs this fragment before
# build_compile_db.py.

# runtime/ units the tests-only build compiles; find_module_source("runtime", ...) resolves them.
TESTS_ONLY_PSEUDO_OVERLAY_SOURCES = (
    "pseudo_overlay.cpp",
    "pseudo_overlay_state.cpp",
    "pseudo_overlay_render.cpp",
    "pseudo_overlay_thread.cpp",
    "pseudo_overlay_sync.cpp",
)

TESTS_ONLY_SENSOR_SOURCES = (
    "sensor_plugin.cpp", "native_sensors.cpp", "pawnio_setup.cpp", "pawnio_setup_core.cpp", "pawnio_driver.cpp",
    "pawnio_workers.cpp",
    "sensor_broker.cpp", "elevation_client.cpp", "startup_preferences.cpp", "startup_control.cpp",
    "startup_autostart.cpp", "elevation_setup.cpp", "elevation_runtime.cpp", "elevation_removal.cpp",
)

TESTS_ONLY_RUNTIME_SOURCES = (
    "recording_owner.cpp", "engine.cpp", "engine_lifecycle.cpp", "engine_recovery.cpp", "runtime.cpp",
    "store_settings_source.cpp",
    "settings_reader.cpp",
    "host_children.cpp", "role_thread_runs.cpp", "process_residue.cpp", "helper_transport.cpp", "hotkey_input_hook.cpp",
    "global_hotkeys.cpp", "inject_control.cpp", "screenshotter.cpp", "launch_command.cpp",
    "target_launcher.cpp", "desktop_overlay.cpp", "settings_store.cpp", "setup_actions.cpp",
    "session_claim.cpp", "diagnostics_session.cpp",
    "engine_setup.cpp",
)

# libcengine foundations (llm-wiki/library, M4): the runtime event queue and the C ABI shim's entry
# points that need no engine. Compiled into unit_tests.exe only until the runtime module joins the
# product build. Tests and these sources share one image, hence CE_STATIC_LIBRARY (no dllimport).
RUNTIME_FOUNDATION_TEST_SOURCES = (
    "runtime/core/event_queue.cpp",
    "runtime/api/api_status.cpp",
    "runtime/api/api_version.cpp",
    "runtime/api/handle_registry.cpp",
    "runtime/api/runtime_desc.cpp",
    "runtime/api/capi_text.cpp",
    "runtime/api/runtime_package.cpp",
    "runtime/api/runtime_lifecycle.cpp",
    "runtime/api/runtime_status.cpp",
    "runtime/api/runtime_commands.cpp",
    "runtime/api/runtime_events.cpp",
    "runtime/api/runtime_log.cpp",
    "runtime/api/runtime_settings.cpp",
    "runtime/api/monitor_list.cpp",
)

# Pure frontend units (llm-wiki/library/05): command-line parser, auto-record script. They reference no
# ce_* function and no common/ code, so unit_tests.exe links them as they are.
FRONTEND_TEST_SOURCES = (
    "frontend/app/auto_record.cpp",
    "frontend/app/command_line.cpp",
    "frontend/app/setup_model.cpp",
)

# Media-role units the unit tests drive through their own interface (no capture or desktop needed).
TESTS_ONLY_MEDIA_SOURCES = ("status_overlay_sync.cpp", "media_session_state.cpp", "media_role_context.cpp",
                            "media_gpu_priority_claims.cpp", "recording_recovery_role.cpp")
# The role host's per-process settings owner (captureengine/app); find_module_source("captureengine", ...) resolves it.
# mediaengine_loader.cpp is the media role's loader of mediaengine.dll; windows_gpu_scheduling.cpp answers its adapter
# LUID queries. test_mediaengine_loader.cpp includes the loader inside its own namespace, so it does not clash.
# The window heartbeat's Present-trace consumer (route-level tests).
TESTS_ONLY_DISPLAY_TIMING_SOURCES = ("present_progress_feed.cpp",)

TESTS_ONLY_APP_SOURCES = ("role_configuration.cpp", "mediaengine_loader.cpp", "windows_gpu_scheduling.cpp")
# The logger role entry and its shared-ring source: the tests drive RunLoggerRole through injected rings.
TESTS_ONLY_LOGGER_SOURCES = ("logger_service.cpp", "logger_ring_source.cpp")
# The inject role (RunInjectRole) and its link closure: the tests run it on a thread over the in-memory channel. The
# role's whitelist-free configuration keeps InjectionManager from being constructed (inject_main.cpp, allowInjection).
TESTS_ONLY_INJECT_ROLE_SOURCES = (
    "inject_config.cpp", "inject_config_publication.cpp", "inject_lifecycle.cpp", "inject_main.cpp",
    "injection.cpp", "injection_inject.cpp", "injection_manager.cpp", "injection_ota_watchdog.cpp",
    "injection_process_exit.cpp", "injection_runtime_exclusion.cpp", "injection_security.cpp",
    "injection_startup_launch.cpp", "injection_wmi_events.cpp", "process_start_etw.cpp", "process_start_poll.cpp",
)

# The media role (RunMediaRole) and its link closure, the same units build_cengine.py links into cengine.dll: the tests
# run it on a thread over the in-memory channel (test_media_role_thread.cpp). media_process_host.cpp and
# process_loopback_worker_host.cpp belong to the helper process only and stay out. Units already listed above are not
# repeated here. frame_submission_inject.cpp and frame_submission_d3d11.cpp are compiled into
# test_frame_submission_adapters.cpp's own object (it includes them), so they stay out.
TESTS_ONLY_MEDIA_ROLE_SOURCES = (
    "dxgi_dup_capture.cpp", "inject_source_read.cpp",
    "media_main.cpp", "media_main_encoder_00_session.cpp", "media_main_encoder_00_session_diagnostics.cpp",
    "media_main_encoder_00_session_shutdown.cpp", "media_main_encoder_01_loop_start.cpp",
    "media_main_encoder_04_loop_wgc_target.cpp", "media_main_encoder_04_loop_wgc_target_pop.cpp",
    "media_main_encoder_04_loop_wgc_target_stats.cpp", "media_main_encoder_05a_loop_wgc_select.cpp",
    "media_main_encoder_05b_loop_wgc_select_uniform.cpp", "media_main_encoder_05c_loop_inject_select.cpp",
    "media_main_encoder_05d_loop_inject_horizon.cpp", "media_main_encoder_06_loop_startup.cpp",
    "media_main_encoder_06_loop_startup_inject.cpp", "media_main_encoder_06_loop_startup_warmup_sync.cpp",
    "media_main_encoder_07_loop_emit.cpp", "media_main_encoder_08_loop_encode.cpp",
    "media_main_encoder_09_loop_health.cpp", "media_main_encoder_10_mux_budget.cpp",
    "media_main_encoder_content_delay.cpp", "media_main_encoder_inject_display_timing.cpp",
    "media_main_priority.cpp", "media_main_recording.cpp", "media_main_start.cpp", "media_main_start_loop.cpp",
    "media_main_start_shutdown.cpp", "media_main_start_targets.cpp", "media_main_threads_inject.cpp",
    "media_main_threads_wgc.cpp", "media_main_wgc.cpp", "media_main_window.cpp", "media_role_entry.cpp",
    "screen_grab_privacy_runtime.cpp", "screenshot.cpp", "wgc_border_access.cpp", "wgc_capture.cpp",
    "wgc_capture_duplication.cpp",
    "wgc_capture_frame.cpp", "wgc_capture_frame_pump.cpp", "wgc_capture_helpers.cpp",
    "wgc_capture_impl_format.cpp", "wgc_capture_impl_gpu_timing.cpp", "wgc_capture_impl_pool.cpp",
    "wgc_capture_init.cpp", "wgc_capture_pool.cpp", "wgc_capture_pool_budget.cpp", "wgc_capture_queue.cpp",
    "wgc_capture_raii_guards.cpp", "wgc_capture_retained_yuv.cpp", "wgc_capture_session.cpp",
    "wgc_capture_state.cpp", "wgc_capture_stats.cpp",
)


def tests_only_captureengine_sources() -> List[str]:
    """captureengine/ units unit_tests.exe compiles itself (found by name). The compile and the tests-only
    warning about uncompiled product sources both read this one list."""
    return (sorted(STRICT_FP_SCREENSHOT_SOURCES) + list(TESTS_ONLY_SENSOR_SOURCES) + list(TESTS_ONLY_MEDIA_SOURCES)
            + list(TESTS_ONLY_MEDIA_ROLE_SOURCES) + list(TESTS_ONLY_APP_SOURCES) + list(TESTS_ONLY_LOGGER_SOURCES)
            + list(TESTS_ONLY_INJECT_ROLE_SOURCES) + list(TESTS_ONLY_DISPLAY_TIMING_SOURCES))


def tests_only_runtime_sources() -> List[str]:
    """runtime/ units unit_tests.exe compiles itself (found by name)."""
    return list(TESTS_ONLY_PSEUDO_OVERLAY_SOURCES) + list(TESTS_ONLY_RUNTIME_SOURCES)
