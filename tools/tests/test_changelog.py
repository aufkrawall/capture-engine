"""Regression tests for changelog management and validation."""

from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

from tools.manage_changelog import (
    extract_version_notes,
    generate_release_notes,
    parse_changelog,
    promote_unreleased,
    validate_changelog,
)

SAMPLE_VALID_CHANGELOG = """# Changelog

## Unreleased

Changes since [v0.1.6652](https://github.com/aufkrawall/capture-engine/releases/tag/v0.1.6652).

### New

- **FidelityFX CAS/RCAS sharpening:** added post-processing sharpening on D3D11, D3D12, and Vulkan.

### Fixed

- **DirectX 12 fence drain:** fixed crash when shutting down overlay during fence synchronization.

## v0.1.6652

Changes since [v0.1.6261](https://github.com/aufkrawall/capture-engine/releases/tag/v0.1.6261).

### Improved

- **Faster startup:** eliminated thread enumeration stalls.
"""


class ChangelogTest(unittest.TestCase):
    def setUp(self) -> None:
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)

    def test_valid_changelog_parses_cleanly(self) -> None:
        parsed = parse_changelog(SAMPLE_VALID_CHANGELOG)
        self.assertIn("Unreleased", parsed)
        self.assertIn("v0.1.6652", parsed)
        self.assertIn("New", parsed["Unreleased"])
        self.assertIn("Fixed", parsed["Unreleased"])
        self.assertEqual(len(parsed["Unreleased"]["New"]), 1)
        self.assertTrue(parsed["Unreleased"]["New"][0].startswith("**FidelityFX CAS/RCAS sharpening:**"))

    def test_validate_accepts_valid_changelog(self) -> None:
        errors = validate_changelog(SAMPLE_VALID_CHANGELOG)
        self.assertEqual(errors, [])

    def test_validate_catches_missing_top_heading(self) -> None:
        text = "## Unreleased\n\n### New\n- **Feature:** details\n"
        errors = validate_changelog(text)
        self.assertTrue(any("heading '# Changelog'" in e for e in errors))

    def test_validate_catches_missing_unreleased(self) -> None:
        text = "# Changelog\n\n## v0.1.100\n\n### New\n- **Feature:** details\n"
        errors = validate_changelog(text)
        self.assertTrue(any("Missing required '## Unreleased'" in e for e in errors))

    def test_validate_catches_invalid_category(self) -> None:
        text = (
            "# Changelog\n\n"
            "## Unreleased\n\n"
            "### RandomStuff\n"
            "- **Feature:** details\n"
        )
        errors = validate_changelog(text)
        self.assertTrue(any("Invalid category 'RandomStuff'" in e for e in errors))

    def test_validate_enforces_bold_anchors_for_unreleased(self) -> None:
        text = (
            "# Changelog\n\n"
            "## Unreleased\n\n"
            "### New\n"
            "- Plain bullet without bold anchor\n"
        )
        errors = validate_changelog(text)
        self.assertTrue(any("Item must begin with bold anchor summary" in e for e in errors))

    def test_validate_catches_empty_bold_anchor(self) -> None:
        text = (
            "# Changelog\n\n"
            "## Unreleased\n\n"
            "### New\n"
            "- **** Missing anchor content\n"
        )
        errors = validate_changelog(text)
        self.assertTrue(any("Bold anchor cannot be empty" in e for e in errors))

    def test_validate_catches_empty_description_after_anchor(self) -> None:
        text = (
            "# Changelog\n\n"
            "## Unreleased\n\n"
            "### New\n"
            "- **Anchor Only**\n"
        )
        errors = validate_changelog(text)
        self.assertTrue(any("lacks descriptive text" in e for e in errors))

    def test_extract_version_notes(self) -> None:
        notes_unreleased = extract_version_notes(SAMPLE_VALID_CHANGELOG, "Unreleased")
        self.assertIsNotNone(notes_unreleased)
        assert notes_unreleased is not None
        self.assertIn("### New", notes_unreleased)
        self.assertIn("### Fixed", notes_unreleased)

        notes_6652 = extract_version_notes(SAMPLE_VALID_CHANGELOG, "0.1.6652")
        self.assertIsNotNone(notes_6652)
        assert notes_6652 is not None
        self.assertIn("### Improved", notes_6652)
        self.assertIn("Faster startup", notes_6652)

        notes_missing = extract_version_notes(SAMPLE_VALID_CHANGELOG, "0.1.9999")
        self.assertIsNone(notes_missing)

    def test_generate_release_notes(self) -> None:
        rel_notes = generate_release_notes(
            SAMPLE_VALID_CHANGELOG,
            version="0.1.6652",
            commit_sha="abcdef0123456789",
        )
        self.assertIn("## v0.1.6652", rel_notes)
        self.assertIn("built and fully verified from abcdef0123456789", rel_notes)
        self.assertIn("Faster startup", rel_notes)
        self.assertIn("Release Assets", rel_notes)
        self.assertIn("captureengine.7z", rel_notes)
        self.assertIn("ffmpeg-corresponding-source.7z", rel_notes)
        self.assertIn("Distribution & Licensing", rel_notes)

    def test_promote_unreleased(self) -> None:
        promoted_text, ok = promote_unreleased(
            SAMPLE_VALID_CHANGELOG,
            new_version="0.1.6700",
            prev_tag="v0.1.6652",
        )
        self.assertTrue(ok)
        self.assertIn("## Unreleased", promoted_text)
        self.assertIn("Changes since [v0.1.6700]", promoted_text)
        self.assertIn("## v0.1.6700", promoted_text)
        self.assertIn("Changes since [v0.1.6652]", promoted_text)
        self.assertIn("FidelityFX CAS/RCAS sharpening", promoted_text)

        # Validate that the promoted changelog is structurally valid
        errors = validate_changelog(promoted_text)
        self.assertEqual(errors, [])

    def test_live_repo_changelog_is_valid(self) -> None:
        repo_changelog = Path(__file__).resolve().parents[2] / "CHANGELOG.md"
        self.assertTrue(repo_changelog.exists(), f"Expected CHANGELOG.md at {repo_changelog}")
        content = repo_changelog.read_text(encoding="utf-8")
        errors = validate_changelog(content)
        self.assertEqual(errors, [], "Repository CHANGELOG.md failed validation:\n" + "\n".join(errors))


if __name__ == "__main__":
    unittest.main()
