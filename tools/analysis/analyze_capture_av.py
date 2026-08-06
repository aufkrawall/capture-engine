# This entry point executes ordered source fragments in its own module globals.
# Keeping one namespace preserves imports, monkeypatching, and CLI behavior.
from pathlib import Path as _SourcePath

_SOURCE_PARTS = (
    'analyze_capture_av_common.py',
    'analyze_capture_av_streams.py',
    'analyze_capture_av_correlation.py',
    'analyze_capture_av_log_parsers.py',
    'analyze_capture_av_wgc_smoothness.py',
    'analyze_capture_av_triage_parsers.py',
    'analyze_capture_av_perf_heuristics.py',
    'analyze_capture_av_wgc_faults.py',
    'analyze_capture_av_recovery_heuristics.py',
    'analyze_capture_av_session_triage.py',
    'analyze_capture_av_report.py',
    'analyze_capture_av_selftest.py',
    'analyze_capture_av_selftest_sessions.py',
    'analyze_capture_av_selftest_encoder.py',
    'analyze_capture_av_selftest_syncdelay.py',
    'analyze_capture_av_selftest_audio.py',
    'analyze_capture_av_selftest_correlation.py',
    'analyze_capture_av_main.py',
)
_SOURCE_TEXT = ""
for _source_name in _SOURCE_PARTS:
    _source_part = (_SourcePath(__file__).with_name(_source_name)).read_text(encoding="utf-8")
    _SOURCE_TEXT += _source_part
exec(compile(_SOURCE_TEXT, __file__, "exec"), globals(), globals())
del _SOURCE_TEXT, _SOURCE_PARTS, _SourcePath, _source_name, _source_part
