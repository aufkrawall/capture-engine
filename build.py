# This entry point executes ordered source fragments in its own module globals.
# Keeping one namespace preserves imports, monkeypatching, and CLI behavior.
from pathlib import Path as _SourcePath

_SOURCE_PARTS = (
    'tools/build/build_part_001.py',
    'tools/build/build_part_002.py',
    'tools/build/build_part_003.py',
    'tools/build/build_part_004.py',
    'tools/build/build_part_005.py',
    'tools/build/build_part_006.py',
    'tools/build/build_part_007.py',
    'tools/build/build_part_008.py',
    'tools/build/build_part_009.py',
    'tools/build/build_part_010.py',
    'tools/build/build_part_011.py',
    'tools/build/build_part_012.py',
    'tools/build/build_part_013.py',
    'tools/build/build_part_014.py',
    'tools/build/build_part_015.py',
    'tools/build/build_part_016.py',
)
_SOURCE_BODY_PARTS = (
    'tools/build/build_part_014.py',
)
_SOURCE_TEXT = ""
for _source_name in _SOURCE_PARTS:
    _source_part = (_SourcePath(__file__).parent / _source_name).read_text(encoding="utf-8")
    if _source_name in _SOURCE_BODY_PARTS:
        _source_part = _source_part.split("\n", 1)[1]
    _SOURCE_TEXT += _source_part
exec(compile(_SOURCE_TEXT, __file__, "exec"), globals(), globals())


def read_source_text(
    _source_parts=_SOURCE_PARTS,
    _source_body_parts=_SOURCE_BODY_PARTS,
    _source_path=_SourcePath,
) -> str:
    source_text = ""
    for name in _source_parts:
        part = (_source_path(__file__).parent / name).read_text(encoding="utf-8")
        if name in _source_body_parts:
            part = part.split("\n", 1)[1]
        source_text += part
    return source_text


del _SOURCE_TEXT, _SOURCE_BODY_PARTS, _source_name, _source_part
