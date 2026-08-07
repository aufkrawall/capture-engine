import http.client
import os
import re
import tempfile
import unittest
import urllib.error
from pathlib import Path
from unittest import mock

import build
from tools import ffmpeg_dependencies as dependencies
from tools import source_download

# The manifest sits next to ffmpeg_dependencies.py in tools/, one level above this
# suite. Resolving it relative to __file__ keeps the tests runnable from any cwd.
MANIFEST_PATH = Path(__file__).resolve().parents[1] / "ffmpeg_dependencies.json"


class FfmpegDependencyManifestTest(unittest.TestCase):
    def test_manifest_has_pinned_build_order_and_versions(self) -> None:
        manifest_path = MANIFEST_PATH
        manifest = dependencies.load_dependency_manifest(str(manifest_path))
        self.assertEqual(manifest["toolchain_version"], "22.1.8")
        self.assertEqual(
            [dependency["name"] for dependency in manifest["dependencies"]],
            ["llvm-runtime", "libiconv", "opus", "libva", "onevpl", "libwinpthread", "aom", "svt-av1"],
        )
        self.assertIn("libaom.dll", dependencies.manifest_runtime_dlls(manifest))
        self.assertIn("libwinpthread-1.dll", dependencies.manifest_runtime_dlls(manifest))
        self.assertIn("libiconv-2.dll", dependencies.manifest_runtime_dlls(manifest))
        self.assertIn("libcharset-1.dll", dependencies.manifest_runtime_dlls(manifest, optional=True))
        self.assertNotIn("libcharset-1.dll", dependencies.manifest_runtime_dlls(manifest))
        for dependency in manifest["dependencies"]:
            self.assertEqual(len(dependency["source_package_pgp_keys"]), 1)
            self.assertEqual(
                dependency["source_package_pgp_keys"][0],
                "5F944B027F7FE2091985AA2EFA11531AA0AA7F57",
            )
            self.assertTrue(dependency["source_package_signature_url"].endswith(".src.tar.zst.sig"))

    def test_manifest_fingerprint_is_stable(self) -> None:
        manifest_path = MANIFEST_PATH
        self.assertEqual(
            dependencies.dependency_manifest_fingerprint(str(manifest_path)),
            dependencies.dependency_manifest_fingerprint(str(manifest_path)),
        )

    def test_manifest_fingerprint_changes_with_build_policy(self) -> None:
        manifest_path = MANIFEST_PATH
        original = dependencies.dependency_manifest_fingerprint(str(manifest_path))
        with mock.patch.object(
            dependencies,
            "DEPENDENCY_BUILD_CONFIGURATION_VERSION",
            dependencies.DEPENDENCY_BUILD_CONFIGURATION_VERSION + 1,
        ):
            changed = dependencies.dependency_manifest_fingerprint(str(manifest_path))
        self.assertNotEqual(original, changed)

        with mock.patch.object(
            dependencies,
            "DEPENDENCY_COMPILE_FLAGS",
            dependencies.DEPENDENCY_COMPILE_FLAGS + " -fno-stack-protector",
        ):
            changed = dependencies.dependency_manifest_fingerprint(str(manifest_path))
        self.assertNotEqual(original, changed)

    def test_force_rebuild_ignores_current_cached_prefix(self) -> None:
        manifest_path = MANIFEST_PATH
        with tempfile.TemporaryDirectory() as temp_dir:
            builder = dependencies.SourceDependencyBuilder(
                temp_dir,
                os.path.join(temp_dir, "msys2"),
                manifest_path=str(manifest_path),
            )
            runtime_dlls = dependencies.manifest_runtime_dlls(builder.manifest)
            Path(builder.bin_dir).mkdir(parents=True)
            for dll_name in runtime_dlls:
                (Path(builder.bin_dir) / dll_name).touch()

            with (
                mock.patch.object(builder, "_is_complete", return_value=True) as is_complete,
                mock.patch.object(builder, "_reset_outputs") as reset_outputs,
                mock.patch.object(builder, "_build_dependency", return_value=[]) as build_dependency,
                mock.patch.object(builder, "_verify_runtime_guard_cf", return_value={}),
            ):
                builder.ensure(force_rebuild=True)

            is_complete.assert_not_called()
            reset_outputs.assert_called_once_with()
            self.assertEqual(build_dependency.call_count, len(builder.manifest["dependencies"]))

    def test_reuses_current_cached_prefix_without_force_rebuild(self) -> None:
        manifest_path = MANIFEST_PATH
        with tempfile.TemporaryDirectory() as temp_dir:
            builder = dependencies.SourceDependencyBuilder(
                temp_dir,
                os.path.join(temp_dir, "msys2"),
                manifest_path=str(manifest_path),
            )
            with (
                mock.patch.object(builder, "_is_complete", return_value=True),
                mock.patch.object(builder, "_verify_runtime_guard_cf", return_value={}),
                mock.patch.object(builder, "_reset_outputs") as reset_outputs,
                mock.patch.object(builder, "_build_dependency") as build_dependency,
            ):
                self.assertEqual(builder.ensure(), builder.prefix)

            reset_outputs.assert_not_called()
            build_dependency.assert_not_called()

    def test_ffmpeg_required_components_and_cache_version_are_current(self) -> None:
        build_source = build.read_source_text()
        self.assertIn('"--enable-encoder=libaom_av1"', build_source)
        self.assertIn('"--enable-decoder=libaom_av1"', build_source)
        self.assertNotIn('"--enable-encoder=libaom-av1"', build_source)
        self.assertIn('"--enable-bsf=hevc_metadata,av1_metadata"', build_source)
        # Deliberate tripwire: changing the configure flags above must come with a
        # cache-version bump, so this pin is updated by hand when that happens.
        self.assertIn("FFMPEG_BUILD_CONFIGURATION_VERSION = 10", build_source)


class FfmpegDependencyPeHelperTest(unittest.TestCase):
    def test_removes_read_only_extracted_package_tree(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            extracted_tree = Path(temp_dir) / "recipe"
            read_only_object = extracted_tree / ".git" / "objects" / "00" / "object"
            read_only_object.parent.mkdir(parents=True)
            read_only_object.write_bytes(b"object")
            read_only_object.chmod(0o444)

            dependencies.remove_tree(str(extracted_tree))

            self.assertFalse(extracted_tree.exists())

    def test_injects_policy_after_makepkg_configuration(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            pkgbuild_path = Path(temp_dir) / "PKGBUILD"
            pkgbuild_path.write_text("build() {\n  true\n}\n", encoding="utf-8")
            dependencies.inject_dependency_build_policy(
                str(pkgbuild_path),
                "/c/private dependency prefix",
                "/clang64/lib",
            )
            content = pkgbuild_path.read_text(encoding="utf-8")
            self.assertIn(dependencies.DEPENDENCY_BUILD_POLICY_MARKER, content)
            self.assertIn("-march=x86-64", content)
            self.assertIn("-mguard=cf", content)
            self.assertIn("-fstack-protector-strong", content)
            self.assertIn("-D_FORTIFY_SOURCE=2", content)
            self.assertIn("-Wl,--guard-cf", content)
            self.assertIn("PKG_CONFIG_PATH=", content)
            with self.assertRaises(dependencies.DependencyBuildError):
                dependencies.inject_dependency_build_policy(
                    str(pkgbuild_path),
                    "/c/private dependency prefix",
                    "/clang64/lib",
                )

    def test_requires_effective_guard_cf_metadata(self) -> None:
        output = """
        IMAGE_DLL_CHARACTERISTICS_GUARD_CF (0x4000)
        GuardCFFunctionTable: 0x180001000
        GuardCFFunctionCount: 17
        CF_FUNCTION_TABLE_PRESENT (0x400)
        """
        self.assertEqual(dependencies.parse_guard_cf_function_count(output), 17)
        with self.assertRaises(dependencies.DependencyBuildError):
            invalid_output = output.replace("GuardCFFunctionCount: 17", "GuardCFFunctionCount: 0")
            dependencies.parse_guard_cf_function_count(invalid_output)

    def test_parses_imported_dll_names_case_insensitively(self) -> None:
        output = """
        DLL Name: KERNEL32.dll
        DLL Name: libcharset-1.dll
        DLL name: image.dll
        """
        self.assertEqual(
            dependencies.parse_pe_import_names(output),
            {"kernel32.dll", "libcharset-1.dll", "image.dll"},
        )

    def test_system_dll_allowlist_does_not_allow_msys_runtime(self) -> None:
        self.assertTrue(dependencies.is_windows_system_dll("KERNEL32.dll"))
        self.assertTrue(dependencies.is_windows_system_dll("api-ms-win-crt-runtime-l1-1-0.dll"))
        self.assertFalse(dependencies.is_windows_system_dll("libgcc_s_seh-1.dll"))
        self.assertFalse(dependencies.is_windows_system_dll("libcharset-1.dll"))

    def test_selects_the_latest_matching_package_archive(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            old_archive = Path(temp_dir) / "mingw-w64-clang-x86_64-opus-1.6.1-1-any.pkg.tar.zst"
            new_archive = Path(temp_dir) / "mingw-w64-clang-x86_64-opus-1.6.1-1-any-2.pkg.tar.zst"
            docs_archive = Path(temp_dir) / "mingw-w64-clang-x86_64-opus-docs-1.6.1-1-any.pkg.tar.zst"
            old_archive.touch()
            new_archive.touch()
            docs_archive.touch()
            os.utime(old_archive, (1, 1))
            os.utime(new_archive, (2, 2))
            os.utime(docs_archive, (3, 3))
            self.assertEqual(
                dependencies.select_package_archive(
                    temp_dir,
                    "mingw-w64-clang-x86_64-opus",
                    "1.6.1",
                ),
                str(new_archive),
            )


class FfmpegVendoredPgpKeyTest(unittest.TestCase):
    """Every pinned signing key must ship in-repo, not come from a keyserver.

    The builder's keyring is not persisted between builds, so each closure build
    re-imported the keys. Fetching them needs dirmngr, which cannot start in the
    Actions runner's non-interactive context, so the release job could never build
    the dependency closure at all. Vendoring removes that dependency; the
    fingerprint check still decides trust, so this is not a weakening.
    """

    def test_every_pinned_fingerprint_has_a_vendored_key(self) -> None:
        manifest = dependencies.load_dependency_manifest(str(MANIFEST_PATH))
        pinned = set()
        for dependency in manifest["dependencies"]:
            for field in ("pgp_keys", "source_package_pgp_keys"):
                pinned.update(fingerprint.upper() for fingerprint in dependency.get(field, []))
        missing = [
            fingerprint
            for fingerprint in sorted(pinned)
            if not os.path.exists(os.path.join(dependencies.PGP_KEY_DIR, f"{fingerprint}.asc"))
        ]
        self.assertEqual(
            [],
            missing,
            "Pinned PGP keys without a vendored tools/pgp-keys/<fingerprint>.asc; the "
            "release job cannot fetch them (no dirmngr on the runner):\n" + "\n".join(missing),
        )

    def test_vendored_keys_are_armored_and_named_by_fingerprint(self) -> None:
        for entry in sorted(os.listdir(dependencies.PGP_KEY_DIR)):
            if not entry.endswith(".asc"):
                continue
            self.assertRegex(entry, r"^[0-9A-F]{40}\.asc$", f"{entry} is not named by fingerprint")
            with open(os.path.join(dependencies.PGP_KEY_DIR, entry), "r", encoding="utf-8") as handle:
                self.assertIn("BEGIN PGP PUBLIC KEY BLOCK", handle.read(200), f"{entry} is not armored")

    def test_vendored_import_is_attempted_before_any_keyserver(self) -> None:
        # Order matters: a keyserver round trip must never be on the normal path.
        source = Path(dependencies.__file__).read_text(encoding="utf-8")
        vendored_at = source.index("PGP_KEY_DIR, f\"{fingerprint.upper()}.asc\"")
        keyserver_at = source.index("for keyserver in keyservers:")
        self.assertLess(vendored_at, keyserver_at)


class FfmpegDownloadTrustTest(unittest.TestCase):
    def test_downloads_use_the_toolchain_ca_bundle_without_disabling_verification(self) -> None:
        # The runner's Python could not verify downloads.xiph.org ("unable to get
        # local issuer certificate") while other hosts verified fine in the same
        # run. Trust is taken from the MSYS2 bundle the build already depends on.
        # Verification must never be switched off: the tarballs are SHA256- and
        # PGP-checked, but that is defence in depth, not a licence to drop TLS.
        source = Path(source_download.__file__).read_text(encoding="utf-8")
        self.assertIn("tls-ca-bundle.pem", source)
        self.assertIn("context=ssl_context", source)
        for forbidden in ("_create_unverified_context", "CERT_NONE", "check_hostname = False"):
            self.assertNotIn(forbidden, source)

    def test_missing_bundle_falls_back_to_default_verification(self) -> None:
        self.assertIsNone(source_download.toolchain_ssl_context(os.path.join(tempfile.gettempdir(), "no-msys")))


class SourceDownloadRetryTest(unittest.TestCase):
    """A release that has compiled for half an hour must survive one dropped TCP
    connection - run 31192282693 died on exactly that ("Remote end closed
    connection without response" from github.com) with no retry."""

    def _download(self, opener, attempts=4):
        slept: list = []
        with mock.patch.object(source_download.urllib.request, "urlopen", opener):
            source_download.download_file(
                "https://example.invalid/x.tar.gz",
                os.path.join(self.tmp, "x.tar.gz"),
                attempts=attempts,
                sleep=slept.append,
            )
        return slept

    def setUp(self) -> None:
        self._dir = tempfile.TemporaryDirectory()
        self.tmp = self._dir.name
        self.addCleanup(self._dir.cleanup)

    def test_transient_disconnect_is_retried_then_succeeds(self) -> None:
        calls = {"n": 0}

        class Response:
            def read(self, *_a):
                return b""

            def __enter__(self):
                return self

            def __exit__(self, *_a):
                return False

        def opener(*_args, **_kwargs):
            calls["n"] += 1
            if calls["n"] < 3:
                raise http.client.RemoteDisconnected("Remote end closed connection without response")
            return Response()

        slept = self._download(opener)
        self.assertEqual(calls["n"], 3)
        self.assertEqual(len(slept), 2)  # backoff only between attempts

    def test_http_404_is_not_retried(self) -> None:
        # A wrong pinned URL is a bug, not weather: fail immediately.
        calls = {"n": 0}

        def opener(*_args, **_kwargs):
            calls["n"] += 1
            raise urllib.error.HTTPError("https://example.invalid/x", 404, "Not Found", {}, None)

        with self.assertRaises(urllib.error.HTTPError):
            self._download(opener)
        self.assertEqual(calls["n"], 1)

    def test_attempts_are_bounded_and_the_error_is_raised(self) -> None:
        def opener(*_args, **_kwargs):
            raise http.client.RemoteDisconnected("nope")

        with self.assertRaises(http.client.RemoteDisconnected):
            self._download(opener, attempts=3)

    def test_partial_download_never_lands_at_the_destination(self) -> None:
        # An interrupted body must not leave a truncated archive that a later run
        # would treat as already fetched.
        destination = os.path.join(self.tmp, "x.tar.gz")

        def opener(*_args, **_kwargs):
            raise http.client.IncompleteRead(b"partial")

        with self.assertRaises(http.client.IncompleteRead):
            self._download(opener, attempts=1)
        self.assertFalse(os.path.exists(destination))
        self.assertFalse(os.path.exists(destination + ".tmp"))


class FfmpegSourcePinTest(unittest.TestCase):
    """The shipped FFmpeg must come from a named source, built by the release job."""

    def test_source_is_pinned_and_feeds_the_build_fingerprint(self) -> None:
        # Until 2026-08-07 the clone tracked master HEAD, so two builds a week
        # apart were not the same product. The ref must also feed the
        # configuration fingerprint: --skip-updates builds return early when
        # prebuilt DLLs exist, before the source is consulted, so otherwise a pin
        # change would silently keep shipping the previous FFmpeg.
        self.assertRegex(build.FFMPEG_SOURCE_REF, r"^(n\d+\.\d+[\w.]*|[0-9a-f]{40})$")
        source = build.read_source_text()
        self.assertIn("ref=FFMPEG_SOURCE_REF", source)
        self.assertIn("digest.update(FFMPEG_SOURCE_REF.encode(", source)

    def test_pin_keeps_the_nmr_aac_coder_available(self) -> None:
        # NMR exists only on FFmpeg master: it landed after the 9.0 release branch
        # was cut, so every released tag drops the encoder to twoloop and fails the
        # AAC tests with "Undefined constant ... 'nmr'". A tag pin is valid only
        # once an upstream release actually carries NMR.
        # mediaengine/audio_encoder.cpp selects it explicitly.
        if not build._is_commit_ref(build.FFMPEG_SOURCE_REF):
            self.fail(
                "FFMPEG_SOURCE_REF is a release tag; confirm the tag contains the NMR "
                "aac_coder (grep AAC_CODER_NMR libavcodec/aacenc.c) before allowing it"
            )

    def test_release_workflow_builds_ffmpeg_instead_of_junctioning_it(self) -> None:
        # Every shipped binary must be produced by the run that publishes it, so a
        # later GitHub artifact attestation covers what it claims to. Junctioning
        # ffmpeg_build to the maintainer's dev tree made releases reuse DLLs
        # compiled locally days earlier: self-built, but not built by the job.
        workflow = Path(__file__).resolve().parents[2] / ".github" / "workflows" / "release-stable.yml"
        text = workflow.read_text(encoding="utf-8")
        junction_targets = re.findall(r"@\('([^']+)',\s*\(Join-Path \$root", text)
        self.assertNotIn(
            "ffmpeg_build",
            junction_targets,
            "release-stable.yml junctions ffmpeg_build again; the release would ship "
            "FFmpeg and its dependency closure built outside the job",
        )
        self.assertIn("Reset FFmpeg build tree", text)
        # The reset must not recurse through a junction: on Windows that follows
        # the link and would delete the maintainer's real toolchain tree.
        self.assertIn("[System.IO.Directory]::Delete(", text)


if __name__ == "__main__":
    unittest.main()
