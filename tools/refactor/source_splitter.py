#!/usr/bin/env python3
"""Reassemble .inl source fragments and split logical sources into .cpp units.

Semantic-unit facade: the implementation lives in source_splitter_part_00X.py,
executed in this module's namespace so imports, CLI behavior, and the
`scan` / `split_source` test surface stay identical to the single-file version.
"""
from pathlib import Path as _SourcePath

_SOURCE_PARTS = (
    'source_splitter_part_001.py',
    'source_splitter_part_002.py',
    'source_splitter_part_003.py',
    'source_splitter_part_004.py',
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
