#!/usr/bin/env python3
"""Changelog management and validation tool.

Maintains, validates, and extracts entries from CHANGELOG.md according to the
guidelines in llm-wiki/changelog-guidelines.md, and generates structured,
ADHD-friendly GitHub release notes for automated release publishing.
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
from pathlib import Path
from typing import Dict, List, Optional, Tuple

ALLOWED_CATEGORIES = frozenset(
    {"New", "Improved", "Fixed", "Changed", "Deprecated", "Removed", "Security"}
)

DEFAULT_REPO_URL = "https://github.com/aufkrawall/capture-engine"


def find_project_root() -> Path:
    """Resolve project root containing CHANGELOG.md or AGENTS.md."""
    current = Path(__file__).resolve().parent
    while current.parent != current:
        if (current / "CHANGELOG.md").exists() or (current / "AGENTS.md").exists():
            return current
        current = current.parent
    return Path(os.getcwd())


def parse_changelog(text: str) -> Dict[str, Dict[str, List[str]]]:
    """Parse CHANGELOG markdown into structured sections, categories, and items."""
    sections: Dict[str, Dict[str, List[str]]] = {}
    current_version: Optional[str] = None
    current_category: Optional[str] = None
    current_item: Optional[str] = None

    lines = text.splitlines()
    for raw_line in lines:
        line = raw_line.rstrip()
        version_match = re.match(r"^##\s+(.+)$", line)
        if version_match:
            if current_item and current_version and current_category:
                sections[current_version][current_category].append(current_item.strip())
                current_item = None
            v_name = version_match.group(1).strip()
            current_version = v_name
            sections[v_name] = {}
            current_category = None
            continue

        cat_match = re.match(r"^###\s+(.+)$", line)
        if cat_match and current_version:
            if current_item and current_category:
                sections[current_version][current_category].append(current_item.strip())
                current_item = None
            cat_name = cat_match.group(1).strip()
            current_category = cat_name
            if cat_name not in sections[current_version]:
                sections[current_version][cat_name] = []
            continue

        item_match = re.match(r"^-\s+(.+)$", line)
        if item_match and current_version and current_category:
            if current_item:
                sections[current_version][current_category].append(current_item.strip())
            current_item = item_match.group(1).strip()
            continue

        # Continuation line for the current bullet item
        if current_item is not None and (line.startswith("  ") or line.startswith("\t")):
            current_item += " " + line.strip()
        elif current_item is not None and line.strip() == "":
            pass

    if current_item and current_version and current_category:
        sections[current_version][current_category].append(current_item.strip())

    return sections


def validate_changelog(text: str, strict_historical: bool = False) -> List[str]:
    """Validate CHANGELOG format against project standards."""
    errors: List[str] = []
    lines = text.splitlines()

    if not lines or not re.match(r"^#\s+Changelog\s*$", lines[0]):
        errors.append("File must start with top-level heading '# Changelog'")

    # Must contain ## Unreleased
    if not re.search(r"^##\s+Unreleased\s*$", text, re.MULTILINE):
        errors.append("Missing required '## Unreleased' section heading")

    current_section: Optional[str] = None
    current_category: Optional[str] = None
    in_unreleased = False
    unreleased_item_count = 0

    for line_no, raw_line in enumerate(lines, start=1):
        line = raw_line.rstrip()

        # Check section header
        if line.startswith("## "):
            current_section = line[3:].strip()
            current_category = None
            in_unreleased = current_section.lower() == "unreleased"
            if not in_unreleased and not re.match(r"^v0\.1\.\d+$", current_section):
                errors.append(
                    f"Line {line_no}: Invalid version header '{line}'. "
                    "Expected '## Unreleased' or '## v0.1.XXXX'"
                )
            continue

        # Check category header
        if line.startswith("### "):
            current_category = line[4:].strip()
            if current_section is None:
                errors.append(f"Line {line_no}: Category '{current_category}' outside of any version section")
            elif current_category not in ALLOWED_CATEGORIES:
                allowed_str = ", ".join(sorted(ALLOWED_CATEGORIES))
                errors.append(
                    f"Line {line_no}: Invalid category '{current_category}'. "
                    f"Allowed categories: {allowed_str}"
                )
            continue

        # Check bullet item
        if line.startswith("- "):
            item_text = line[2:].strip()
            if in_unreleased:
                unreleased_item_count += 1

            if current_category is None:
                errors.append(f"Line {line_no}: Bullet item outside of any '### Category' heading")
                continue

            # Check ADHD-friendly bold anchor: starts with **<Anchor>**
            # Strictly enforced for Unreleased and recent releases (or all if strict_historical)
            is_recent_or_unreleased = in_unreleased or (current_section and current_section >= "v0.1.6652")
            if is_recent_or_unreleased or strict_historical:
                bold_match = re.match(r"^\*\*([^*]*)\*\*(.*)$", item_text)
                if not bold_match:
                    errors.append(
                        f"Line {line_no}: Item must begin with bold anchor summary: '- **<Anchor>** <Details>'"
                    )
                else:
                    anchor = bold_match.group(1).strip()
                    rest = bold_match.group(2).strip()
                    if not anchor:
                        errors.append(f"Line {line_no}: Bold anchor cannot be empty")
                    if not rest:
                        errors.append(
                            f"Line {line_no}: Item lacks descriptive text after bold anchor '**{anchor}**'"
                        )

    # Warn or error on empty unreleased
    parsed = parse_changelog(text)
    unreleased_data = parsed.get("Unreleased", {})
    total_unreleased = sum(len(items) for items in unreleased_data.values())
    if unreleased_item_count != total_unreleased:
        errors.append("Internal parser mismatch in Unreleased item count")

    return errors


def normalize_version(version: str) -> str:
    """Normalize version string by stripping leading 'v'."""
    v = version.strip()
    if v.startswith("v") or v.startswith("V"):
        v = v[1:]
    return v


def unreleased_baseline_tag(text: str) -> Optional[str]:
    """Tag named by the '## Unreleased' section's 'Changes since [vX]' line, if present.

    That line records which release the Unreleased entries are measured against, so it is
    also the evidence that a promotion already happened: once `## Unreleased` says
    'Changes since [v0.1.6748]', its bullets belong to the cycle AFTER v0.1.6748 and must
    never be published as v0.1.6748's notes.
    """
    section = re.search(r"^##\s+Unreleased\s*\n(.*?)(?=^##\s|\Z)", text, re.DOTALL | re.MULTILINE)
    if not section:
        return None
    baseline = re.search(r"Changes since \[(v?0\.1\.\d+)\]", section.group(1))
    return baseline.group(1) if baseline else None


def extract_version_notes(text: str, version: str) -> Optional[str]:
    """Extract changelog notes for a version or Unreleased."""
    target_clean = normalize_version(version)
    parsed = parse_changelog(text)

    # Search keys
    matching_key: Optional[str] = None
    for key in parsed:
        if key.lower() == "unreleased" and target_clean.lower() == "unreleased":
            matching_key = key
            break
        key_clean = normalize_version(key)
        if key_clean == target_clean:
            matching_key = key
            break

    if not matching_key:
        return None

    cat_map = parsed[matching_key]
    if not cat_map:
        return ""

    out_lines: List[str] = []
    # Preferred category ordering
    order = ["New", "Improved", "Fixed", "Changed", "Deprecated", "Removed", "Security"]
    for cat in order:
        items = cat_map.get(cat)
        if items:
            out_lines.append(f"### {cat}\n")
            for item in items:
                out_lines.append(f"- {item}\n")
            out_lines.append("")

    for cat, items in cat_map.items():
        if cat not in order and items:
            out_lines.append(f"### {cat}\n")
            for item in items:
                out_lines.append(f"- {item}\n")
            out_lines.append("")

    return "\n".join(out_lines).strip()


class StaleUnreleasedError(RuntimeError):
    """The '## Unreleased' section cannot describe the version being released."""


def generate_release_notes(
    text: str,
    version: str,
    commit_sha: Optional[str] = None,
) -> str:
    """Generate complete GitHub release notes combining changelog entries with asset & license metadata."""
    clean_v = normalize_version(version)
    tag_name = f"v{clean_v}"

    # First look for specific version; if missing, fall back to Unreleased.
    #
    # The fallback is the NORMAL path, not a degraded one: the build number is a local
    # counter (tools/build/build_io.py:bump_and_write_build_version), so nobody knows the
    # release version until the runner has built it and `## Unreleased` is therefore still
    # un-promoted when the release is cut.
    #
    # What the fallback must never do is republish an already-released block. Once
    # --promote-release has run, `## Unreleased` states 'Changes since [<that tag>]', which
    # is the proof that its bullets belong to the NEXT cycle. Releasing that same version
    # again - a re-dispatch after a deleted tag, or a version input typo - would otherwise
    # silently re-publish the previous release's notes.
    notes = extract_version_notes(text, clean_v)
    if not notes:
        baseline = unreleased_baseline_tag(text)
        if baseline and normalize_version(baseline) == clean_v:
            raise StaleUnreleasedError(
                f"'## Unreleased' is measured against {tag_name} but no '## {tag_name}' section exists. "
                f"Those entries belong to the cycle AFTER {tag_name} and must not be published as its "
                f"notes. Restore the '## {tag_name}' section, or release a different version."
            )
        notes = extract_version_notes(text, "Unreleased")

    if not notes:
        notes = "_No specific feature notes recorded for this build._"

    commit_suffix = f" from {commit_sha}" if commit_sha else ""
    summary_line = f"Stable release {clean_v} built and fully verified{commit_suffix}."

    parts = [
        f"## {tag_name}\n",
        f"{summary_line}\n",
        "### Highlights & Changes\n",
        notes,
        "\n### Release Assets\n",
        "- `captureengine.7z`: core product binaries, PDBs, FFmpeg closure, and default configuration",
        "- `testapps.7z`: standalone graphics & frame generation test executables and diagnostic tools",
        "- `ffmpeg-corresponding-source.7z`: complete LGPL corresponding source archive",
        "- `latest_manifest.json` / `latest_summary.txt`: full build and verification evidence\n",
        "### Distribution & Licensing\n",
        "- FFmpeg is distributed as shared libraries under LGPL-2.1-or-later; see bundled notices.",
        "- Binaries are verified with PE hardening checks (ASLR, DEP/NX, CFG) and are not Authenticode-signed.",
    ]
    return "\n".join(parts)


def promote_unreleased(
    text: str,
    new_version: str,
    prev_tag: Optional[str] = None,
    repo_url: str = DEFAULT_REPO_URL,
) -> Tuple[str, bool]:
    """Promote ## Unreleased entries to a new version section and reset ## Unreleased."""
    clean_v = normalize_version(new_version)
    tag_name = f"v{clean_v}"

    unreleased_notes = extract_version_notes(text, "Unreleased")
    if not unreleased_notes:
        return text, False

    # Find previous version tag if not provided
    if not prev_tag:
        v_matches = re.findall(r"^##\s+v(0\.1\.\d+)", text, re.MULTILINE)
        if v_matches:
            prev_tag = f"v{v_matches[0]}"
        else:
            prev_tag = "v0.1.0"

    unreleased_pattern = re.compile(
        r"^##\s+Unreleased\s*\n.*?(?=^##\s+v|\Z)", re.DOTALL | re.MULTILINE
    )

    new_section = (
        "## Unreleased\n\n"
        f"Changes since [{tag_name}]({repo_url}/releases/tag/{tag_name}).\n\n"
        f"## {tag_name}\n\n"
        f"Changes since [{prev_tag}]({repo_url}/releases/tag/{prev_tag}).\n\n"
        f"{unreleased_notes}\n\n"
    )

    updated_text = unreleased_pattern.sub(new_section, text, count=1)
    return updated_text, True


def check_worktree(project_root: Path) -> Tuple[bool, str]:
    """Verify if modified code files are accompanied by a modified CHANGELOG.md."""
    try:
        res = subprocess.run(
            ["git", "status", "--porcelain"],
            cwd=project_root,
            capture_output=True,
            text=True,
            check=True,
        )
    except Exception as ex:
        return True, f"Could not check git status: {ex}"

    lines = res.stdout.splitlines()
    code_extensions = {".cpp", ".h", ".hpp", ".c", ".py"}
    code_dirs = {"hook", "captureengine", "mediaengine", "common"}

    has_code_changes = False
    has_changelog_change = False

    for line in lines:
        if len(line) < 4:
            continue
        filepath = line[3:].strip()
        p = Path(filepath)
        if p.name.lower() == "changelog.md":
            has_changelog_change = True
        elif p.suffix in code_extensions and any(d in p.parts for d in code_dirs):
            has_code_changes = True

    if has_code_changes and not has_changelog_change:
        return False, (
            "Code files have been modified in the worktree, but CHANGELOG.md has not been updated! "
            "Agents must update CHANGELOG.md under '## Unreleased' before committing."
        )

    return True, "Worktree is compliant."


def main() -> int:
    parser = argparse.ArgumentParser(description="Changelog management tool")
    parser.add_argument(
        "--changelog",
        type=Path,
        default=None,
        help="Path to CHANGELOG.md (default: resolve from workspace)",
    )
    parser.add_argument("--validate", action="store_true", help="Validate CHANGELOG.md format")
    parser.add_argument(
        "--strict-historical",
        action="store_true",
        help="Enforce bold anchors on older historical releases prior to v0.1.6652",
    )
    parser.add_argument("--extract-notes", type=str, help="Extract notes for version or 'Unreleased'")
    parser.add_argument(
        "--generate-release-notes",
        action="store_true",
        help="Generate full GitHub release notes markdown",
    )
    parser.add_argument("--version", type=str, help="Target version (e.g. 0.1.6652)")
    parser.add_argument("--commit", type=str, help="Target commit SHA")
    parser.add_argument("--output", type=Path, help="Output file to write generated notes")
    # The version may be given as this flag's own value (`--promote-release 0.1.6748`) or
    # through `--version`, because both spellings were in circulation and the second one
    # used to abort with "expected one argument" instead of doing anything.
    parser.add_argument(
        "--promote-release",
        type=str,
        nargs="?",
        const="",
        help="Promote Unreleased section to the given release version (or the one from --version)",
    )
    parser.add_argument("--prev-tag", type=str, help="Previous release tag (e.g. v0.1.6261)")
    parser.add_argument(
        "--check-worktree",
        action="store_true",
        help="Check that CHANGELOG.md is modified if code files are modified",
    )

    args = parser.parse_args()
    root = find_project_root()
    changelog_path = args.changelog or (root / "CHANGELOG.md")

    if args.check_worktree:
        ok, msg = check_worktree(root)
        if not ok:
            print(f"ERROR: {msg}", file=sys.stderr)
            return 1
        print(f"OK: {msg}")
        return 0

    if not changelog_path.exists():
        print(f"ERROR: Changelog file not found at {changelog_path}", file=sys.stderr)
        return 1

    text = changelog_path.read_text(encoding="utf-8")

    if args.validate:
        errors = validate_changelog(text, strict_historical=args.strict_historical)
        if errors:
            print(f"CHANGELOG validation failed with {len(errors)} error(s):", file=sys.stderr)
            for err in errors:
                print(f"  - {err}", file=sys.stderr)
            return 1
        print(f"CHANGELOG.md format is valid ({changelog_path})")
        return 0

    if args.extract_notes:
        notes = extract_version_notes(text, args.extract_notes)
        if notes is None:
            print(f"ERROR: Version '{args.extract_notes}' not found", file=sys.stderr)
            return 1
        print(notes)
        return 0

    if args.generate_release_notes:
        if not args.version:
            print("ERROR: --version is required for --generate-release-notes", file=sys.stderr)
            return 1
        try:
            rel_notes = generate_release_notes(text, args.version, commit_sha=args.commit)
        except StaleUnreleasedError as ex:
            print(f"ERROR: {ex}", file=sys.stderr)
            return 1
        if extract_version_notes(text, normalize_version(args.version)) is None:
            # Expected for a normal release; say so anyway, so the operator is reminded of
            # the promotion step the release itself cannot perform.
            print(
                f"NOTE: no '## v{normalize_version(args.version)}' section yet; used '## Unreleased'. "
                f"After the release, run: python tools/manage_changelog.py "
                f"--promote-release {normalize_version(args.version)}",
                file=sys.stderr,
            )
        if args.output:
            args.output.write_text(rel_notes + "\n", encoding="utf-8")
            print(f"Wrote release notes to {args.output}")
        else:
            print(rel_notes)
        return 0

    if args.promote_release is not None:
        promote_version = args.promote_release or (args.version or "")
        if not promote_version:
            print(
                "ERROR: --promote-release needs a version, either as its own value "
                "(--promote-release 0.1.6748) or via --version",
                file=sys.stderr,
            )
            return 1
        updated, promoted = promote_unreleased(
            text,
            promote_version,
            prev_tag=args.prev_tag,
        )
        if not promoted:
            print("WARNING: No unreleased notes found to promote", file=sys.stderr)
            return 1
        changelog_path.write_text(updated, encoding="utf-8")
        print(f"Promoted unreleased changes to v{normalize_version(promote_version)}")
        return 0

    parser.print_help()
    return 0


if __name__ == "__main__":
    sys.exit(main())
