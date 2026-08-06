# This entry point executes ordered source fragments in its own module globals.
# Keeping one namespace preserves imports, monkeypatching, and CLI behavior.
from pathlib import Path as _SourcePath

_SOURCE_PARTS = (
    'analyze_av_sync_stimulus_common.py',
    'analyze_av_sync_stimulus_timing.py',
    'analyze_av_sync_stimulus_audio.py',
    'analyze_av_sync_stimulus_evaluate.py',
    'analyze_av_sync_stimulus_main.py',
)
_SOURCE_TEXT = ""
for _source_name in _SOURCE_PARTS:
    _source_part = (_SourcePath(__file__).with_name(_source_name)).read_text(encoding="utf-8")
    _SOURCE_TEXT += _source_part
exec(compile(_SOURCE_TEXT, __file__, "exec"), globals(), globals())
del _SOURCE_TEXT, _SOURCE_PARTS, _SourcePath, _source_name, _source_part
