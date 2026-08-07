# MIT License
#
# Copyright (c) 2026 aufkrawall
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in
# all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
# SOFTWARE.

"""Project build policy appended to every pinned MSYS2 dependency recipe.

Split out of ffmpeg_dependencies.py, which had reached the 800-line ceiling.
The policy is a block of shell appended to the PKGBUILD *after* makepkg's own
configuration has already replaced the caller's flags, so it always wins. It
carries three things:

1. Hardening and prefix wiring - the closure compiles against the private
   dependency prefix with the project's CFG/stack-protector flags.
2. Output restriction - only the subpackages the manifest declares as
   `package_outputs` are built, and a declared output the recipe does not
   actually provide fails the build instead of silently yielding nothing.
3. Documentation restriction - doxygen is limited to output formats whose file
   names do not encode the absolute source path (see
   DOCUMENTATION_OUTPUT_OVERRIDES).

The rendered text feeds the dependency build fingerprint (see
`policy_fingerprint`), so changing any of it invalidates the cached closure
rather than quietly shipping a prefix built under the previous policy.
"""

from __future__ import annotations

import hashlib
import shlex
from typing import Sequence


class DependencyBuildError(RuntimeError):
    """Raised when a pinned FFmpeg dependency cannot be source-built safely."""


DEPENDENCY_BUILD_POLICY_MARKER = "# captureproject source-dependency build policy"

# GENERATE_MAN is the load-bearing one. For an input *directory* doxygen names
# the man page after the escaped absolute path
# ("C__Users_..._src_opus-1.6.1_include_.3", 152 characters for opus), which put
# the file at 313 characters on the release runner's workspace - 27 deeper than a
# dev checkout - so doxygen failed with "Could not open file ... for writing" and
# ninja stopped. A dev build lands at 259, one under Windows' 260-character
# MAX_PATH, which is why this stayed latent.
#
# Measured, not assumed: man is the *only* backend that derives names from the
# input path. LaTeX, RTF, XML, DocBook and HTML all use a content hash
# ("dir_fe80300f08587586fe06c8824e04b727.tex", 40 characters). The other four are
# off because nothing in the closure consumes doxygen output and generating them
# is pure build cost - not because they are a path-length risk.
#
# HTML deliberately stays on: it is the only doc output the recipes' targets
# declare and install, and opus's package function moves `share/doc` and so
# requires it. That is what makes this restriction lossless.
#
# Later assignments win in a doxygen configuration file, and these are appended
# at the end, so they override whatever the recipe's own configuration set.
DOCUMENTATION_OUTPUT_OVERRIDES = (
    "GENERATE_MAN = NO",
    "GENERATE_LATEX = NO",
    "GENERATE_RTF = NO",
    "GENERATE_XML = NO",
    "GENERATE_DOCBOOK = NO",
)

# Matched case-insensitively against the extracted source tree. `Doxyfile*`
# covers the generated `Doxyfile` as well as the `Doxyfile.in` templates that
# meson/cmake/autotools expand at configure time - opus ships the latter, so
# patching only a literal `Doxyfile` would miss it.
DOXYGEN_CONFIG_PATTERNS = ("Doxyfile*", "*.doxyfile*", "doxygen.cfg*")

# Rendered with plain token substitution rather than an f-string: the block is
# mostly shell parameter expansions, and escaping every brace made the previous
# smaller version hard to read and easy to get wrong.
_POLICY_TEMPLATE = """

@MARKER@
_captureproject_prefix=@PREFIX@
_captureproject_msys_lib=@MSYS_LIB@
CPPFLAGS+=" -I${_captureproject_prefix}/include"
CFLAGS+=" @COMPILE_FLAGS@ -I${_captureproject_prefix}/include"
CXXFLAGS+=" @COMPILE_FLAGS@ -I${_captureproject_prefix}/include"
LDFLAGS+=" @LINK_FLAGS@ -L${_captureproject_prefix}/lib -L${_captureproject_msys_lib}"
PKG_CONFIG_PATH="${_captureproject_prefix}/lib/pkgconfig:${PKG_CONFIG_PATH:-}"
CMAKE_PREFIX_PATH="${_captureproject_prefix}:${CMAKE_PREFIX_PATH:-}"
export CPPFLAGS CFLAGS CXXFLAGS LDFLAGS PKG_CONFIG_PATH CMAKE_PREFIX_PATH

# Build only the subpackages the manifest declares. Upstream recipes split off
# subpackages nothing here consumes (opus-docs, iconv, winpthreads) and makepkg
# would still build, package and compress every one of them. Reducing pkgname
# after the recipe's split-package wrappers have been generated is safe: the
# wrapper for each retained name already exists.
#
# The membership check is the fail-closed half. An upstream subpackage rename
# would otherwise leave pkgname holding a name no package function provides, or
# silently build a differently named package that the extraction step then can
# not find - so it is rejected here, while the recipe is still on screen.
_captureproject_package_outputs=(@PACKAGE_OUTPUTS@)
for _captureproject_output in "${_captureproject_package_outputs[@]}"; do
  if [[ " ${pkgname[*]} " != *" ${_captureproject_output} "* ]]; then
    printf '==> ERROR: captureproject build policy: recipe does not declare package output %s (declares: %s)\\n' \\
      "${_captureproject_output}" "${pkgname[*]}" >&2
    return 1
  fi
done
pkgname=("${_captureproject_package_outputs[@]}")

# Documentation is never a package output, and the man/LaTeX generators derive
# their file names from the absolute source path, which exceeds MAX_PATH on a
# deep workspace. Applied by wrapping build(): the configuration files only
# exist once makepkg has extracted the sources, and for meson/cmake recipes the
# effective Doxyfile is generated from Doxyfile.in during the build itself.
_captureproject_restrict_documentation_output() {
  local _captureproject_config
  while IFS= read -r -d '' _captureproject_config; do
    {
      echo ''
      echo '# captureproject: path-independent documentation output only'
@DOCUMENTATION_OVERRIDE_ECHOES@
    } >> "${_captureproject_config}"
    printf '  -> captureproject build policy: restricted documentation output in %s\\n' \\
      "${_captureproject_config#${srcdir}/}" >&2
  done < <(find "${srcdir}" -type f \\( @DOXYGEN_CONFIG_TESTS@ \\) -print0)
}

_captureproject_upstream_build="$(declare -f build)"
if [[ -n ${_captureproject_upstream_build} ]]; then
  eval "${_captureproject_upstream_build/#build/_captureproject_upstream_build_function}"
  build() {
    _captureproject_restrict_documentation_output
    _captureproject_upstream_build_function
  }
fi
"""


def render_build_policy(
    prefix: str,
    msys_lib: str,
    package_outputs: Sequence[str],
    compile_flags: str,
    link_flags: str,
) -> str:
    """Return the policy block to append to a dependency PKGBUILD.

    `prefix` and `msys_lib` are MSYS-style paths; `package_outputs` are the
    fully prefixed MSYS2 package names from the manifest, which is the spelling
    the recipe's own `pkgname` carries once MINGW_PACKAGE_PREFIX is applied.
    """
    if not package_outputs:
        raise DependencyBuildError("Dependency build policy requires at least one package output")

    override_echoes = "\n".join(
        f"      echo {shlex.quote(override)}" for override in DOCUMENTATION_OUTPUT_OVERRIDES
    )
    config_tests = " -o ".join(f"-iname {shlex.quote(pattern)}" for pattern in DOXYGEN_CONFIG_PATTERNS)
    replacements = {
        "@MARKER@": DEPENDENCY_BUILD_POLICY_MARKER,
        "@PREFIX@": shlex.quote(prefix),
        "@MSYS_LIB@": shlex.quote(msys_lib),
        "@COMPILE_FLAGS@": compile_flags,
        "@LINK_FLAGS@": link_flags,
        "@PACKAGE_OUTPUTS@": " ".join(shlex.quote(output) for output in package_outputs),
        "@DOCUMENTATION_OVERRIDE_ECHOES@": override_echoes,
        "@DOXYGEN_CONFIG_TESTS@": config_tests,
    }
    policy = _POLICY_TEMPLATE
    for token, value in replacements.items():
        policy = policy.replace(token, value)
    leftover = sorted(token for token in replacements if token in policy)
    if leftover:
        raise DependencyBuildError(f"Dependency build policy left unsubstituted tokens: {', '.join(leftover)}")
    return policy


def inject_dependency_build_policy(
    pkgbuild_path: str,
    prefix: str,
    msys_lib: str,
    package_outputs: Sequence[str],
    compile_flags: str,
    link_flags: str,
) -> None:
    """Append policy after makepkg's config has replaced the caller's flags.

    Written with an explicit LF newline: makepkg refuses to source a PKGBUILD
    that contains CRLF, so the platform default would break every recipe.
    """
    with open(pkgbuild_path, "r", encoding="utf-8") as pkgbuild_file:
        existing = pkgbuild_file.read()
    if DEPENDENCY_BUILD_POLICY_MARKER in existing:
        raise DependencyBuildError(f"Dependency PKGBUILD already contains the project build policy: {pkgbuild_path}")

    policy = render_build_policy(prefix, msys_lib, package_outputs, compile_flags, link_flags)
    with open(pkgbuild_path, "a", encoding="utf-8", newline="\n") as pkgbuild_file:
        pkgbuild_file.write(policy)


def policy_fingerprint(compile_flags: str, link_flags: str) -> str:
    """Content fingerprint of the policy itself, for the dependency build state.

    Rendered against fixed placeholder inputs so it tracks the policy text and
    the flags, not the machine it runs on. Without this, editing what the policy
    *does* - which subpackages get built, which documentation formats get
    generated - would leave a previously built prefix looking current, the same
    trap that let a changed FFmpeg source pin keep shipping the old FFmpeg.
    """
    policy = render_build_policy(
        "/captureproject/prefix",
        "/captureproject/msys/lib",
        ("captureproject-package-output",),
        compile_flags,
        link_flags,
    )
    return hashlib.sha256(policy.encode("utf-8")).hexdigest()
