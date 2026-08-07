# Privacy-safe debug artifacts: the build tree lives under the developer's real
# Windows profile, and compilers/linkers bake absolute paths into PDBs and PE
# debug directories. Release artifacts must never leak the developer's user
# name, so the profile root is remapped in debug info (compile), images embed
# only bare PDB names (link), and the finalize stage scrubs/verifies the shipped
# binaries. test_privacy_paths covers tracked sources; this covers generated
# artifacts. Loaded before build_common.py so the native flag lists can embed
# PRIVACY_PREFIX_MAP_FLAGS at module level.

import os
import re

from typing import Any, List, Tuple


def profile_path_spellings() -> List[str]:
    """Return the developer profile root in the spellings build tools emit."""
    profile = os.environ.get("USERPROFILE")
    if not profile:
        return []
    return list(dict.fromkeys((profile, profile.replace("\\", "/"))))


def privacy_prefix_map_flags() -> List[str]:
    """-ffile-prefix-map flags rewriting the profile root in compiler debug info."""
    flags: List[str] = []
    for spelling in profile_path_spellings():
        sep = "/" if "/" in spelling else "\\"
        flags.append(f"-ffile-prefix-map={spelling}=C:{sep}Users{sep}<developer>")
    return flags


PRIVACY_PREFIX_MAP_FLAGS = privacy_prefix_map_flags()


def sanitize_privacy_paths(text: str) -> str:
    """Replace the developer profile root in text with a neutral placeholder."""
    for spelling in profile_path_spellings():
        sep = "/" if "/" in spelling else "\\"
        text = text.replace(spelling, f"C:{sep}Users{sep}<developer>")
    spellings = profile_path_spellings()
    if spellings:
        user = re.escape(os.path.basename(spellings[0]))
        text = re.sub(
            r"(?<=[\\/:])" + user + r"(?=[\\/=\" \x00])",
            "<developer>",
            text,
        )
    return text


def sanitize_privacy_values(obj: Any) -> Any:
    """Deep-copy a manifest/summary structure with all path strings sanitized."""
    if isinstance(obj, dict):
        return {key: sanitize_privacy_values(value) for key, value in obj.items()}
    if isinstance(obj, list):
        return [sanitize_privacy_values(value) for value in obj]
    if isinstance(obj, str):
        return sanitize_privacy_paths(obj)
    return obj


def redact_user_component(user: str) -> str:
    """Length-identical redaction of a Windows user component for in-place edits."""
    if not user:
        return user
    return ("redact" * ((len(user) + 5) // 6))[: len(user)]


def _user_component_patterns(user: str) -> Tuple[bytes, bytes]:
    """Compiled UTF-8 and UTF-16LE patterns matching the user name as a path
    component, so the redaction is length-preserving and never touches ordinary
    text. Observed leak spellings all share path-ish delimiters: C:\\Users\\<user>,
    C:/Users/<user>, C:<escaped>Users\\<user> (compiler/linker command-line
    records store doubled backslashes), and the MSYS /c/Users/<user> form."""
    utf8 = re.compile(
        rb"(?<=[\\/:])" + re.escape(user).encode("utf-8") + rb"(?=[\\/= \"\x00])"
    )
    utf16 = re.compile(
        rb"(?<=[\\/:]\x00)"
        + user.encode("utf-16le")
        + rb"(?=(?:[\\/=\" ]\x00)|\x00\x00)"
    )
    return utf8, utf16


def scrub_profile_path_bytes(data: bytes) -> bytes:
    """Length-preserving replacement of the user-name path component in binary
    artifacts.

    PDB streams store path strings inside length-prefixed records, so the
    redaction must keep every byte offset identical; only the user component
    itself is replaced with a same-length filler, in every spelling the build
    tools emit (plain and escaped backslashes, forward slashes, MSYS drive
    paths; UTF-8 and UTF-16LE)."""
    spellings = profile_path_spellings()
    if not spellings:
        return data
    user = os.path.basename(spellings[0])
    redacted_user = redact_user_component(user)
    if not redacted_user:
        return data
    utf8_pattern, utf16_pattern = _user_component_patterns(user)
    data = utf8_pattern.sub(redacted_user.encode("utf-8"), data)
    data = utf16_pattern.sub(redacted_user.encode("utf-16le"), data)
    return data


def count_profile_path_hits(data: bytes) -> int:
    """Count path-component occurrences of the developer user name."""
    spellings = profile_path_spellings()
    if not spellings:
        return 0
    user = os.path.basename(spellings[0])
    utf8_pattern, utf16_pattern = _user_component_patterns(user)
    return len(utf8_pattern.findall(data)) + len(utf16_pattern.findall(data))


