"""Machine-wide bare mirrors for the pinned git sources of the FFmpeg build.

Every stable release deletes `ffmpeg_build` so the run that publishes FFmpeg also
compiles it. That used to include a full network clone of FFmpeg (and of
nv-codec-headers) on every attempt, and the network was the single most frequent
reason a release died: git.ffmpeg.org and git.videolan.org outages, a pin that is not
on the default branch, dropped connections (runs 31215691866, 40093203714,
41091557390, 41313352394, 41315640134).

The trust model does not change. A pin is a commit id; index-pack names every object
it receives by hashing it, and the working checkout is verified to be exactly the
pinned commit, as with the network clone this replaces. The mirror only removes the
round trip: a release clones
from it locally (hardlinked packs, seconds instead of a minute) and goes to the
network only when the pinned commit is genuinely missing - which the release
preflight resolves before any compile starts.

Pinned commits are kept reachable under `refs/ce-pins/` so a commit that is on no
branch (fetched by id) survives garbage collection of the mirror.

Two git executables, on purpose. MSYS2's git hangs in fetch's connectivity check on
every local-transport fetch or push when a native process starts it (measured
2026-11-01: `git fetch <local path>` never returns, while the same command from Git for
Windows takes 0.1 s). So the bare mirror - which has no working tree, so line-ending
configuration cannot touch it - is maintained by Git for Windows, and the working
checkout is still produced by the toolchain's MSYS2 git with a local clone (hardlinked
packs, no transport) and a checkout, exactly as the network clone produced it before.
"""

from __future__ import annotations

import os
import re
import shutil
import stat
import subprocess
from typing import Callable, List, Mapping, Optional, Sequence

from tools.machine_lock import MachineLock

Logger = Callable[[str], None]
Runner = Callable[..., object]

_COMMIT = re.compile(r"^[0-9a-fA-F]{40}$")
PIN_NAMESPACE = "refs/ce-pins"
_FETCH_BRANCHES_AND_TAGS = ("+refs/heads/*:refs/heads/*", "+refs/tags/*:refs/tags/*")


def is_commit_ref(ref: str) -> bool:
    return bool(_COMMIT.match(ref or ""))


def _remove_tree(path: str) -> None:
    """Delete a git tree; pack files are read-only on Windows."""
    def make_writable_and_retry(function: Callable[..., object], entry: str, _: object) -> None:
        os.chmod(entry, stat.S_IWRITE)
        function(entry)

    if os.path.exists(path):
        shutil.rmtree(path, onerror=make_writable_and_retry)


class GitMirrorError(RuntimeError):
    pass


def find_mirror_git(toolchain_dir: Optional[str] = None) -> str:
    """Git for Windows, never the toolchain's MSYS2 git (see the module docstring)."""
    candidates = [shutil.which("git")]
    for base in (os.environ.get("ProgramW6432"), os.environ.get("ProgramFiles")):
        if base:
            candidates.append(os.path.join(base, "Git", "cmd", "git.exe"))
    toolchain = os.path.normcase(os.path.realpath(toolchain_dir)) if toolchain_dir else None
    for candidate in candidates:
        if not candidate or not os.path.isfile(candidate):
            continue
        if toolchain and os.path.normcase(os.path.realpath(candidate)).startswith(toolchain + os.sep):
            continue
        return candidate
    raise GitMirrorError("Git for Windows is required to maintain the source mirror (https://git-scm.com/)")


class GitMirror:
    """Keeps `<root>/<name>.git` able to serve a pinned ref without the network."""

    def __init__(
        self,
        mirror_git: str,
        checkout_git: str,
        env: Mapping[str, str],
        root: str,
        *,
        path_for_checkout_git: Callable[[str], str] = lambda path: path,
        logger: Optional[Logger] = None,
        lock_dir: Optional[str] = None,
    ) -> None:
        self.mirror_git = mirror_git
        self.checkout_git = checkout_git
        self.env = dict(env)
        # A public source never needs credentials; a prompt would hang a release.
        self.env.update({"GIT_TERMINAL_PROMPT": "0", "GCM_INTERACTIVE": "never"})
        self.root = root
        self._checkout_path = path_for_checkout_git
        self._log = logger or (lambda _message: None)
        self._lock_dir = lock_dir or os.path.join(root, ".locks")

    # -- helpers -------------------------------------------------------------
    def mirror_path(self, name: str) -> str:
        return os.path.join(self.root, f"{name}.git")

    def _git_run(self, arguments: Sequence[str], cwd: Optional[str] = None, *,
                 checkout: bool = False) -> subprocess.CompletedProcess:
        git = self.checkout_git if checkout else self.mirror_git
        return subprocess.run([git, *arguments], cwd=cwd, env=self.env, capture_output=True, text=True,
                              stdin=subprocess.DEVNULL, encoding="utf-8", errors="replace")

    def _checked(self, arguments: Sequence[str], cwd: Optional[str] = None, *, checkout: bool = False) -> str:
        result = self._git_run(arguments, cwd, checkout=checkout)
        if result.returncode != 0:
            detail = (result.stderr or result.stdout or "").strip().splitlines()
            raise GitMirrorError(f"git {' '.join(arguments[:3])} failed ({result.returncode}): "
                                 + (detail[-1] if detail else "no output"))
        return result.stdout

    def resolve(self, repository: str, ref: str, *, checkout: bool = False) -> Optional[str]:
        """The commit `ref` names in `repository`, or None. `^0` peels tags without braces,
        which the MSYS2 runtime would otherwise brace-expand (see build_ffmpeg)."""
        result = self._git_run(["rev-parse", "--verify", "--quiet", f"{ref}^0"], cwd=repository, checkout=checkout)
        commit = result.stdout.strip() if result.returncode == 0 else ""
        return commit or None

    @staticmethod
    def _host(url: str) -> str:
        return url.split("/")[2] if "//" in url else url

    # -- mirror maintenance ----------------------------------------------------
    def ensure(self, name: str, urls: Sequence[str], ref: str) -> str:
        """Return the mirror path once it holds `ref`, fetching only what is missing."""
        if not urls:
            raise ValueError(f"no source URLs for {name}")
        lock = MachineLock(f"git-mirror-{name}", self._lock_dir, logger=self._log)
        lock.acquire(description=["git-mirror", name, ref])
        try:
            return self._ensure_locked(name, list(urls), ref)
        finally:
            lock.release()

    def _ensure_locked(self, name: str, urls: List[str], ref: str) -> str:
        mirror = self.mirror_path(name)
        existed = os.path.isfile(os.path.join(mirror, "HEAD"))
        if not existed:
            self._create(name, urls, mirror)
        commit = self.resolve(mirror, ref)
        if commit is None:
            commit = self._fetch_ref(name, urls, mirror, ref)
        elif existed:
            self._log(f"[GitMirror] {name} already holds {ref[:12]}; no network access")
        if is_commit_ref(ref) and commit.lower() != ref.lower():
            raise GitMirrorError(f"{name}: {ref} resolved to {commit} in the mirror")
        self._checked(["update-ref", f"{PIN_NAMESPACE}/{commit}", commit], cwd=mirror)
        return mirror

    def _create(self, name: str, urls: List[str], mirror: str) -> None:
        os.makedirs(self.root, exist_ok=True)
        staging = f"{mirror}.partial-{os.getpid()}"
        errors = []
        for attempt in (1, 2):
            for url in urls:
                _remove_tree(staging)
                self._log(f"[GitMirror] Creating {name} mirror from {self._host(url)} (attempt {attempt})")
                result = self._git_run(["clone", "--bare", "--quiet", url, staging])
                if result.returncode == 0:
                    os.replace(staging, mirror)
                    return
                errors.append(f"{self._host(url)}: {(result.stderr or '').strip().splitlines()[-1:]}")
        _remove_tree(staging)
        raise GitMirrorError(f"could not create the {name} mirror from any source: {'; '.join(errors)}")

    def _fetch_ref(self, name: str, urls: List[str], mirror: str, ref: str) -> str:
        errors = []
        for attempt in (1, 2):
            for url in urls:
                host = self._host(url)
                self._log(f"[GitMirror] {name}: {ref[:12]} missing; fetching from {host} (attempt {attempt})")
                fetch = ["fetch", "--quiet", "--force", url]
                result = self._git_run([*fetch, *_FETCH_BRANCHES_AND_TAGS], cwd=mirror)
                if result.returncode == 0 and self.resolve(mirror, ref):
                    return str(self.resolve(mirror, ref))
                if is_commit_ref(ref):
                    # A pin on no advertised branch: hosts that allow fetching by id
                    # (GitHub) serve it directly; git.ffmpeg.org refuses and falls through.
                    by_id = self._git_run([*fetch, ref], cwd=mirror)
                    if by_id.returncode == 0 and self.resolve(mirror, ref):
                        return str(self.resolve(mirror, ref))
                    result = by_id
                errors.append(f"{host}: {(result.stderr or '').strip().splitlines()[-1:]}")
        raise GitMirrorError(f"no source serves {name} {ref}: {'; '.join(errors)}")

    # -- working checkouts ----------------------------------------------------
    def clone(self, name: str, urls: Sequence[str], ref: str, dest: str) -> None:
        """Create a working checkout of `ref` at `dest` from the mirror (no partial tree on failure)."""
        mirror = self.ensure(name, urls, ref)
        try:
            self._checked(["clone", "--quiet", "--no-checkout", self._checkout_path(mirror),
                           self._checkout_path(dest)], checkout=True)
            self._checked(["checkout", "--quiet", "--force", ref], cwd=dest, checkout=True)
            wanted = self.resolve(dest, ref, checkout=True)
            head = self.resolve(dest, "HEAD", checkout=True)
            if not wanted or wanted != head:
                raise GitMirrorError(f"checkout at {dest} is {head}, expected {ref}")
            # The hardlinked packs hold the pin even when no branch does; keep it reachable.
            self._checked(["update-ref", f"{PIN_NAMESPACE}/{wanted}", wanted], cwd=dest, checkout=True)
            self._checked(["remote", "set-url", "origin", list(urls)[0]], cwd=dest, checkout=True)
        except Exception:
            _remove_tree(dest)
            raise

    def replace_checkout(self, name: str, urls: Sequence[str], ref: str, dest: str) -> None:
        """Move an existing pristine checkout onto a pin it lacks by recloning from the mirror.

        A local reclone takes seconds and needs no transport, which MSYS2's git cannot
        drive (module docstring); the checkout holds nothing worth preserving, since
        packaging refuses a dirty FFmpeg tree anyway.
        """
        self.ensure(name, urls, ref)
        _remove_tree(dest)
        self.clone(name, urls, ref, dest)
