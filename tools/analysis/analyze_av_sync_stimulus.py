# This entry point executes ordered source fragments in its own module globals.
# Keeping one namespace preserves imports, monkeypatching, and CLI behavior.
from pathlib import Path as _SourcePath

_SOURCE_PARTS = (
    'analyze_av_sync_stimulus_part_001.py',
    'analyze_av_sync_stimulus_part_002.py',
    'analyze_av_sync_stimulus_part_003.py',
    'analyze_av_sync_stimulus_part_004.py',
    'analyze_av_sync_stimulus_part_005.py',
)
_SOURCE_BODY_PARTS = ()
_SOURCE_TEXT = ""
for _source_name in _SOURCE_PARTS:
    _source_part = (_SourcePath(__file__).with_name(_source_name)).read_text(encoding="utf-8")
    if _source_name in _SOURCE_BODY_PARTS:
        _source_part = _source_part.split("\n", 1)[1]
    _SOURCE_TEXT += _source_part
exec(compile(_SOURCE_TEXT, __file__, "exec"), globals(), globals())
del _SOURCE_TEXT, _SOURCE_PARTS, _SOURCE_BODY_PARTS, _SourcePath, _source_name, _source_part
