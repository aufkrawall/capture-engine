# This entry point executes ordered source fragments in its own module globals.
# Keeping one namespace preserves imports, monkeypatching, and CLI behavior.
from pathlib import Path as _SourcePath

_SOURCE_PARTS = (
    'tools/build/build_privacy.py',
    'tools/build/build_common.py',
    'tools/build/build_bootstrap.py',
    'tools/build/build_io.py',
    'tools/build/build_fg_sdk.py',
    'tools/build/build_lhm_plugin.py',
    'tools/build/build_linux_msys2.py',
    'tools/build/build_ffmpeg.py',
    'tools/build/build_toolchain.py',
    'tools/build/build_compile_db.py',
    'tools/build/build_tests.py',
    'tools/build/build_preflight.py',
    'tools/build/build_testapps.py',
    'tools/build/build_vulkan_layer.py',
    'tools/build/build_project.py',
    'tools/build/build_project_finalize.py',
    'tools/build/build_corresponding_source.py',
    'tools/build/build_packaging.py',
    'tools/build/build_cli.py',
)
_SOURCE_BODY_PARTS = ()
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
