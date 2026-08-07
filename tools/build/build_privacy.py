# Privacy-safe debug artifacts: the build tree lives under the developer's real
# Windows profile, and compilers/linkers bake absolute paths into PDBs and PE
# debug directories. Release artifacts must never leak the developer's user
# name, so the profile root is remapped in debug info (compile), images embed
# only bare PDB names (link), and the finalize stage scrubs/verifies the shipped
# binaries. test_privacy_paths covers tracked sources; this covers generated
# artifacts. Loaded before build_common.py so the native flag lists can embed
# PRIVACY_PREFIX_MAP_FLAGS at module level.

import os
import platform
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
        # Terminator handling matches _user_component_patterns: a negative
        # lookahead on name-continuation characters rather than an allowlist of
        # accepted terminators, which used to miss the same path followed by
        # `;`, `)`, a newline or the end of the buffer.
        text = re.sub(
            r"(?<=[\\/:])" + user + r"(?![A-Za-z0-9_-])",
            "<developer>",
            text,
        )
        # Path-derived identifiers defeat every rule above, because the
        # separators are gone: doxygen names its man pages after the escaped
        # absolute input path, so run 31192891717 logged the maintainer's user
        # name as `C__Users_<developer>_Programme_...` while the same path one line
        # earlier was correctly redacted. Anchoring on the mangled `Users`
        # component lets `_` terminate the name here without the general rule
        # having to accept it - which it must not, or it would rewrite the
        # leading fragment of any longer name that merely starts the same way.
        text = re.sub(
            r"(?<![A-Za-z0-9])Users_" + user + r"(?![A-Za-z0-9-])",
            "Users_<developer>",
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


def _user_component_patterns(user: str) -> Tuple[Any, Any]:
    """Compiled UTF-8 and UTF-16LE patterns matching the user name as a path
    component, so the redaction is length-preserving and never touches ordinary
    text. Observed leak spellings all share path-ish delimiters: C:\\Users\\<user>,
    C:/Users/<user>, C:<escaped>Users\\<user> (compiler/linker command-line
    records store doubled backslashes), and the MSYS /c/Users/<user> form.

    The trailing side is a negative lookahead on name-continuation characters
    rather than a list of accepted terminators. The earlier allowlist
    (`[\\/= "\\x00]`) silently missed the same path followed by `;`, `)`, `'`, a
    newline, or ending the buffer - and since the scrub and its verification pass
    share this pattern, such an occurrence would have been neither rewritten nor
    reported. Excluding only `[A-Za-z0-9_-]` still refuses to match a longer name
    that merely starts with this one, while treating `.` as a terminator so
    domain-profile folders (`C:\\Users\\<user>.DOMAIN`) are covered.

    Both spellings are regex-escaped: a user name containing `.` or `+` would
    otherwise turn into a metacharacter and over-match."""
    escaped_utf8 = re.escape(user.encode("utf-8"))
    escaped_utf16 = re.escape(user.encode("utf-16le"))
    utf8 = re.compile(rb"(?<=[\\/:])" + escaped_utf8 + rb"(?![A-Za-z0-9_-])")
    utf16 = re.compile(rb"(?<=[\\/:]\x00)" + escaped_utf16 + rb"(?!(?:[A-Za-z0-9_-]\x00))")
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


# The build machine name has no legitimate route into an artifact: no compiler,
# linker or packaging step records host state. It is therefore checked and never
# scrubbed - an occurrence means something new started embedding the host, which
# has to be understood rather than silently rewritten. (GitHub's runner does
# write the same name into Actions run logs, which no in-job step can prevent;
# that surface is handled by deleting the log, see
# `.github/workflows/release-log-cleanup.yml`.)
#
# Names shorter than this are skipped: a host called "PC" or "BUILD" occurs
# inside ordinary strings and mangled symbol names, so enforcing it would fail
# builds on coincidence rather than on a leak.
MACHINE_NAME_MIN_LENGTH = 8


def machine_name_spellings() -> List[str]:
    """Candidate spellings of this machine's name, longest first.

    Both `COMPUTERNAME` and the real node name are consulted: the environment
    variable is the spelling Windows tooling emits, but it is only a variable and
    can be reassigned, while `platform.node()` reads the actual host name. Taking
    both means overriding the variable cannot quietly disable the scan."""
    candidates = [os.environ.get("COMPUTERNAME", ""), platform.node()]
    unique: List[str] = []
    for candidate in candidates:
        name = candidate.strip()
        if name and not any(name.lower() == seen.lower() for seen in unique):
            unique.append(name)
    return sorted(unique, key=len, reverse=True)


def scannable_machine_names() -> List[str]:
    """Machine-name spellings long enough to match without false positives."""
    return [name for name in machine_name_spellings() if len(name) >= MACHINE_NAME_MIN_LENGTH]


def machine_name_scan_skip_reason() -> str:
    """Empty when the scan can run, else why it cannot - never silently skipped."""
    spellings = machine_name_spellings()
    if not spellings:
        return "no machine name available (COMPUTERNAME unset and platform.node() empty)"
    if not scannable_machine_names():
        return (
            f"machine name is shorter than {MACHINE_NAME_MIN_LENGTH} characters, "
            "which is too ambiguous to match without false positives"
        )
    return ""


def _machine_name_patterns(name: str) -> Tuple[Any, Any]:
    """Whole-token UTF-8 and UTF-16LE patterns for one machine-name spelling.

    Case-insensitive because tools spell host names in either case (`hostname`
    reports lower case, `COMPUTERNAME` upper), and token-bounded so the name
    cannot match inside a longer identifier that merely contains it."""
    utf8 = re.compile(
        rb"(?<![A-Za-z0-9])" + re.escape(name.encode("utf-8")) + rb"(?![A-Za-z0-9])",
        re.IGNORECASE,
    )
    utf16 = re.compile(
        rb"(?<![A-Za-z0-9]\x00)"
        + re.escape(name.encode("utf-16le"))
        + rb"(?!(?:[A-Za-z0-9]\x00))",
        re.IGNORECASE,
    )
    return utf8, utf16


def count_machine_name_hits(data: bytes) -> int:
    """Count whole-token occurrences of this machine's name in an artifact."""
    total = 0
    for name in scannable_machine_names():
        utf8_pattern, utf16_pattern = _machine_name_patterns(name)
        total += len(utf8_pattern.findall(data)) + len(utf16_pattern.findall(data))
    return total


def privacy_sanitize_log_text(text: str) -> str:
    """Redact developer-identifying paths from console/log output when the
    release workflow requests it (CE_PRIVACY_SANITIZE_LOGS=1), so GitHub Actions
    run logs never expose the maintainer's profile path. Local builds leave
    output untouched so real paths stay available for diagnostics."""
    if os.environ.get("CE_PRIVACY_SANITIZE_LOGS") != "1":
        return text
    return sanitize_privacy_paths(text)

