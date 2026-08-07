import email.message
import http.client
import os
import re
import subprocess
import tempfile
import unittest
import urllib.error
from pathlib import Path
from typing import Sequence
from unittest import mock

import build
from tools import dependency_build_policy as policy
from tools import dependency_pgp
from tools import ffmpeg_dependencies as dependencies
from tools import rehearse_dependency_closure as rehearsal
from tools import source_download

# The manifest sits next to ffmpeg_dependencies.py in tools/, one level above this
# suite. Resolving it relative to __file__ keeps the tests runnable from any cwd.
MANIFEST_PATH = Path(__file__).resolve().parents[1] / "ffmpeg_dependencies.json"
PROJECT_ROOT = Path(__file__).resolve().parents[2]
MSYS_BASH = PROJECT_ROOT / "build" / "msys64" / "usr" / "bin" / "bash.exe"
MSYS_GPG = PROJECT_ROOT / "build" / "msys64" / "usr" / "bin" / "gpg.exe"


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

    def test_rebuild_discards_the_keyring_so_vendored_keys_are_reimported(self) -> None:
        # The blind spot behind run 31207385807: _reset_outputs kept `gnupg/`, so
        # has_fingerprint() short-circuited on a key imported weeks earlier and the
        # vendored-import path never ran locally. The runner deletes ffmpeg_build
        # wholesale and therefore always ran it, so a broken key was visible only
        # there. Sockets must survive - gpg-agent/keyboxd put them in that
        # directory and a locked socket would make removal fail.
        with tempfile.TemporaryDirectory() as temp_dir:
            builder = dependencies.SourceDependencyBuilder(
                temp_dir,
                os.path.join(temp_dir, "msys2"),
                manifest_path=str(MANIFEST_PATH),
            )
            Path(builder.gnupg_dir).mkdir(parents=True)
            keyrings = [Path(builder.gnupg_dir) / name for name in dependency_pgp.KEYRING_FILES]
            for keyring in keyrings:
                keyring.write_bytes(b"stale keyring")
            socket = Path(builder.gnupg_dir) / "S.gpg-agent"
            socket.write_bytes(b"socket stand-in")

            builder._reset_outputs()

            for keyring in keyrings:
                self.assertFalse(keyring.exists(), f"{keyring.name} survived the rebuild reset")
            self.assertTrue(socket.exists(), "the reset must not touch gpg-agent sockets")
            self.assertTrue(os.path.isdir(builder.gnupg_dir), "the gnupg directory itself must survive")

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
            pkgbuild_path.write_text("build() {\n  true\n}\n", encoding="utf-8", newline="\n")
            policy.inject_dependency_build_policy(
                str(pkgbuild_path),
                "/c/private dependency prefix",
                "/clang64/lib",
                ["mingw-w64-clang-x86_64-opus"],
                dependencies.DEPENDENCY_COMPILE_FLAGS,
                dependencies.DEPENDENCY_LINK_FLAGS,
            )
            content = pkgbuild_path.read_text(encoding="utf-8")
            self.assertIn(policy.DEPENDENCY_BUILD_POLICY_MARKER, content)
            self.assertIn("-march=x86-64", content)
            self.assertIn("-mguard=cf", content)
            self.assertIn("-fstack-protector-strong", content)
            self.assertIn("-D_FORTIFY_SOURCE=2", content)
            self.assertIn("-Wl,--guard-cf", content)
            self.assertIn("PKG_CONFIG_PATH=", content)
            # makepkg refuses to source a PKGBUILD containing CRLF, so the
            # appended block must keep LF even though this is a Windows build.
            self.assertNotIn(b"\r\n", pkgbuild_path.read_bytes())
            with self.assertRaises(dependencies.DependencyBuildError):
                policy.inject_dependency_build_policy(
                    str(pkgbuild_path),
                    "/c/private dependency prefix",
                    "/clang64/lib",
                    ["mingw-w64-clang-x86_64-opus"],
                    dependencies.DEPENDENCY_COMPILE_FLAGS,
                    dependencies.DEPENDENCY_LINK_FLAGS,
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


RECIPE_TEMPLATE = """
_realname=opus
pkgbase=mingw-w64-${_realname}
pkgname=("${MINGW_PACKAGE_PREFIX}-${_realname}"
         "${MINGW_PACKAGE_PREFIX}-${_realname}-docs")
pkgver=1.6.1

build() {
  echo UPSTREAM_BUILD_RAN
}

package_opus() { :; }
package_opus-docs() { :; }

# template start; name=mingw-w64-splitpkg-wrappers; version=1.0;
for _name in "${pkgname[@]}"; do
  _short="package_${_name#${MINGW_PACKAGE_PREFIX}-}"
  _func="$(declare -f "${_short}")"
  eval "${_func/#${_short}/package_${_name}}"
done
# template end;
"""

# Mirrors makepkg's own sequence: source the recipe (fail the build if sourcing
# fails, as source_safe does), then run build() in an extracted source tree.
RECIPE_DRIVER = """
set -u
export MINGW_PACKAGE_PREFIX=mingw-w64-clang-x86_64
if ! source ./PKGBUILD; then
  echo SOURCE_FAILED
  exit 3
fi
printf 'PKGNAME:%s\\n' "${pkgname[*]}"
for _n in "${pkgname[@]}"; do
  declare -F "package_${_n}" >/dev/null || { echo "MISSING_WRAPPER:${_n}"; exit 4; }
done
srcdir="${PWD}/src"
build
"""


@unittest.skipUnless(MSYS_BASH.is_file(), "MSYS2 bash is required to execute the generated recipe policy")
class DependencyBuildPolicyShellTest(unittest.TestCase):
    """The policy is generated shell that only ever runs inside a 40-minute
    release build, so a syntax or logic slip in it is expensive to discover.
    These execute it the way makepkg does instead of pattern-matching the text."""

    def _write_recipe(
        self,
        directory: Path,
        package_outputs: Sequence[str],
        recipe: str = RECIPE_TEMPLATE,
    ) -> None:
        pkgbuild = directory / "PKGBUILD"
        pkgbuild.write_text(recipe, encoding="utf-8", newline="\n")
        policy.inject_dependency_build_policy(
            str(pkgbuild),
            "/c/private dependency prefix",
            "/clang64/lib",
            package_outputs,
            dependencies.DEPENDENCY_COMPILE_FLAGS,
            dependencies.DEPENDENCY_LINK_FLAGS,
        )
        (directory / "driver.sh").write_text(RECIPE_DRIVER, encoding="utf-8", newline="\n")

    def _run_recipe(self, directory: Path) -> "subprocess.CompletedProcess[str]":
        # A login shell with CHERE_INVOKING is how SourceDependencyBuilder
        # invokes makepkg-mingw, and it is what puts the MSYS utilities the
        # policy uses (find) on PATH. Running plain `bash script` instead would
        # silently exercise a different environment than the release job.
        environment = dict(os.environ, CHERE_INVOKING="1", MSYSTEM="CLANG64")
        return subprocess.run(
            [str(MSYS_BASH), "-lc", "source ./driver.sh"],
            cwd=str(directory),
            env=environment,
            capture_output=True,
            text=True,
            check=False,
        )

    def test_pkgbuild_is_reduced_to_the_declared_package_outputs(self) -> None:
        # Upstream splits off subpackages nothing here consumes; makepkg would
        # still build, package and compress each of them.
        with tempfile.TemporaryDirectory() as temp_dir:
            directory = Path(temp_dir)
            self._write_recipe(directory, ["mingw-w64-clang-x86_64-opus"])
            doxyfile = directory / "src" / "opus-1.6.1" / "doc" / "Doxyfile.in"
            doxyfile.parent.mkdir(parents=True)
            doxyfile.write_text("GENERATE_MAN = YES\nGENERATE_HTML = YES\n", encoding="utf-8", newline="\n")

            result = self._run_recipe(directory)

            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("PKGNAME:mingw-w64-clang-x86_64-opus\n", result.stdout)
            self.assertNotIn("opus-docs", result.stdout)
            # The retained name must still have its split-package wrapper, and
            # the upstream build body must still run after the policy step.
            self.assertNotIn("MISSING_WRAPPER", result.stdout)
            self.assertIn("UPSTREAM_BUILD_RAN", result.stdout)
            # Doxyfile.in, not just a literal Doxyfile: for meson/cmake recipes
            # the effective configuration is generated from the template during
            # the build, so patching only the generated file would be too late.
            restricted = doxyfile.read_text(encoding="utf-8")
            self.assertIn("GENERATE_MAN = NO", restricted)
            self.assertLess(restricted.index("GENERATE_MAN = YES"), restricted.index("GENERATE_MAN = NO"))

    def test_declared_output_the_recipe_does_not_provide_fails_closed(self) -> None:
        # An upstream subpackage rename must stop the build at the recipe, not
        # produce a closure that is silently missing a library.
        with tempfile.TemporaryDirectory() as temp_dir:
            directory = Path(temp_dir)
            self._write_recipe(directory, ["mingw-w64-clang-x86_64-opus-renamed"])

            result = self._run_recipe(directory)

            self.assertEqual(result.returncode, 3, result.stdout + result.stderr)
            self.assertIn("SOURCE_FAILED", result.stdout)
            self.assertIn("does not declare package output", result.stderr)

    def test_recipe_without_a_build_function_is_left_alone(self) -> None:
        # The build() wrapper is guarded: re-defining build() for a recipe that
        # has none would make makepkg start running one.
        recipe = RECIPE_TEMPLATE.replace("build() {\n  echo UPSTREAM_BUILD_RAN\n}\n", "")
        self.assertNotIn("build()", recipe)
        with tempfile.TemporaryDirectory() as temp_dir:
            directory = Path(temp_dir)
            self._write_recipe(directory, ["mingw-w64-clang-x86_64-opus"], recipe=recipe)
            (directory / "driver.sh").write_text(
                RECIPE_DRIVER.replace(
                    "\nbuild\n",
                    "\nif declare -F build >/dev/null; then echo BUILD_DEFINED; fi\nexit 0\n",
                ),
                encoding="utf-8",
                newline="\n",
            )

            result = self._run_recipe(directory)

            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("PKGNAME:mingw-w64-clang-x86_64-opus\n", result.stdout)
            self.assertNotIn("BUILD_DEFINED", result.stdout)

    def test_generated_policy_is_syntactically_valid_shell(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            directory = Path(temp_dir)
            self._write_recipe(directory, ["mingw-w64-clang-x86_64-opus"])
            result = subprocess.run(
                [str(MSYS_BASH), "-n", "./PKGBUILD"],
                cwd=str(directory),
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertEqual(result.returncode, 0, result.stderr)


class RehearsalDetectorTest(unittest.TestCase):
    """The rehearsal tool is only useful if its checks can actually fail.

    Its first version used a path heuristic ("is there a `doc` component?"), which
    would have reported libiconv's own `share/man/man3` and needed reasoning to
    clear. These are the real observed names from both sides.
    """

    def test_flags_the_doxygen_man_names_that_broke_the_release(self) -> None:
        for name in (
            "C__Users_TestUser_Programme_build_captureproject_ffmpeg_build_dependencies_"
            "recipes_opus_mingw-w64-opus_src_opus-1.6.1_include_.3",
            "C__Users_TestUser_Programme_build_runner-work_capture-engine_capture-engine_"
            "ffmpeg_build_dependencies_recipes_opus_mingw-w64-opus_src_opus-1.6.1_.3",
        ):
            self.assertTrue(rehearsal.is_path_derived_name(name), name)

    def test_does_not_flag_ordinary_generated_or_installed_files(self) -> None:
        # libiconv installs real man pages; doxygen's other backends hash their
        # names. None of these encode a path, so none may be reported.
        for name in (
            "iconv_open.3",
            "opus.h.3",
            "opus_multistream_ctls.3",
            "dir_fe80300f08587586fe06c8824e04b727.tex",
            "dir_b20e91b0a5fe8ce313ec317fba02c47c.html",
            "libopus-0.dll",
            "mingw-w64-clang-x86_64-opus-1.6.1-1-any.pkg.tar.zst",
            "noname",
            "PKGBUILD",
        ):
            self.assertFalse(rehearsal.is_path_derived_name(name), name)


class DependencyBuildPolicyTest(unittest.TestCase):
    def test_man_output_stays_disabled_and_html_stays_enabled(self) -> None:
        # GENERATE_MAN is the load-bearing override: for an input directory doxygen
        # names the man page after the escaped absolute path
        # ("C__Users_..._src_opus-1.6.1_include_.3"), which reached 313 characters
        # on the release runner's workspace - 27 deeper than a dev checkout, which
        # lands at 259, one under Windows' 260-character MAX_PATH. Measured: man is
        # the only backend that does this; the rest hash their names.
        self.assertIn("GENERATE_MAN = NO", policy.DOCUMENTATION_OUTPUT_OVERRIDES)
        # HTML must stay on. It is the only doc output the recipes' targets declare
        # and install, and opus's package function moves `share/doc`, so disabling
        # it would fail packaging rather than save anything.
        for override in policy.DOCUMENTATION_OUTPUT_OVERRIDES:
            self.assertNotIn("GENERATE_HTML", override)
        rendered = policy.render_build_policy("/c/p", "/c/lib", ["pkg"], "-O2", "-s")
        # Applied by wrapping build(): the configuration files do not exist
        # until makepkg has extracted the sources.
        self.assertIn("_captureproject_restrict_documentation_output", rendered)
        self.assertIn("declare -f build", rendered)

    def test_policy_covers_doxyfile_templates_not_only_generated_files(self) -> None:
        self.assertIn("Doxyfile*", policy.DOXYGEN_CONFIG_PATTERNS)
        rendered = policy.render_build_policy("/c/p", "/c/lib", ["pkg"], "-O2", "-s")
        self.assertIn("-iname 'Doxyfile*'", rendered)

    def test_policy_text_feeds_the_dependency_build_fingerprint(self) -> None:
        # What the policy *does* - which subpackages get built, which doc formats
        # get generated - must invalidate the cached prefix. Otherwise a changed
        # policy leaves a previously built closure looking current, the same trap
        # that let a changed FFmpeg source pin keep shipping the old FFmpeg.
        original = dependencies.dependency_manifest_fingerprint(str(MANIFEST_PATH))
        with mock.patch.object(
            policy,
            "DOCUMENTATION_OUTPUT_OVERRIDES",
            policy.DOCUMENTATION_OUTPUT_OVERRIDES + ("GENERATE_MAN = YES",),
        ):
            changed = dependencies.dependency_manifest_fingerprint(str(MANIFEST_PATH))
        self.assertNotEqual(original, changed)

    def test_policy_requires_at_least_one_declared_output(self) -> None:
        # An empty package_outputs would reduce pkgname to nothing and makepkg
        # would build no package at all; load_dependency_manifest rejects it too.
        with self.assertRaises(dependencies.DependencyBuildError):
            policy.render_build_policy("/c/p", "/c/lib", [], "-O2", "-s")

    def test_manifest_declares_no_documentation_subpackage_as_an_output(self) -> None:
        manifest = dependencies.load_dependency_manifest(str(MANIFEST_PATH))
        for dependency in manifest["dependencies"]:
            for output in dependency["package_outputs"]:
                self.assertFalse(
                    output.endswith(("-docs", "-doc")),
                    f"{dependency['name']} declares documentation subpackage {output} as a build output",
                )


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
            # hdrs must be an email.message.Message, not {}: pyright rejects the
            # dict, and the lint stage never saw it because the commit that added
            # this test landed after the last lint run.
            raise urllib.error.HTTPError(
                "https://example.invalid/x", 404, "Not Found", email.message.Message(), None
            )

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
