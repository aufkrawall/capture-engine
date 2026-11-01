"""Stable releases without wasted runs: one fail-fast preflight for the maintainer and the job.

Between 2026-10-23 and 2026-11-01 twelve `release-stable` runs failed or were cancelled
against four successes, each after 8-55 minutes plus a manual runner start. Almost every
failure was predictable from the commit and the machine before any compile started:

  * static gates the per-change loop never runs: flake8 (found 47 minutes in, run
    41318250050), the file-size ratchet, Python tool self-tests (privacy_paths,
    installer_native);
  * FFmpeg and dependency sources: git.ffmpeg.org / git.videolan.org / msys2 mirror
    outages, a pin off the default branch, a TLS EOF, SourceForge;
  * the shared MSYS2 toolchain changing under the release: a doxygen that could not
    load its DLLs, mingw-w64 headers that broke libvpl.

This tool runs exactly those checks, concurrently, in under a minute on a warm machine,
and the release job runs the same code first, so a local pass means the job's own
preflight passes too. It also fills the machine-wide source caches (tools/shared_cache.py)
so the job's FFmpeg closure build reads every source offline.

    python tools/stable_release.py preflight                 # local readiness check
    python tools/stable_release.py dispatch [--yes] [--watch]  # preflight, runner check, dispatch
    python tools/stable_release.py resolve-version ...       # release job
    python tools/stable_release.py preflight --ci ...        # release job
    python tools/stable_release.py summarize                 # release job, always

Nothing here weakens what a release proves: the job still builds the FFmpeg closure and
the product from source, runs the full `--verify --verify-clean` gate, and attests the
assets. The preflight only moves predictable failures to the front.
"""

from __future__ import annotations

import argparse
import base64
import datetime
import json
import os
import re
import subprocess
import sys
import tempfile
import threading
import time
from concurrent.futures import ThreadPoolExecutor
from dataclasses import asdict, dataclass, field
from typing import Any, Callable, Dict, Iterator, List, Mapping, Optional, Sequence

PROJECT_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, PROJECT_ROOT)

from tools.machine_lock import describe_holder  # noqa: E402
from tools.release_summary import summarize_command  # noqa: E402

WORKFLOW_FILE = "release-stable.yml"
RUNNER_NAME = "windows-release"
VERSION_PATTERN = re.compile(r"^0\.1\.(\d+)$")
REPORT_NAME = "release_preflight.json"
# Self-test suites that drive build outputs (the freshly linked setup stub). Before the
# release build they would test the previous release's binaries; the build reruns them.
BUILD_OUTPUT_SUITES = ("installer_native", "installer_ui")
DETAIL_LINES = 12


@dataclass
class CheckResult:
    name: str
    status: str  # pass | warn | fail
    seconds: float
    summary: str
    details: List[str] = field(default_factory=list)


class ReleaseError(RuntimeError):
    pass


# -- small helpers ----------------------------------------------------------------
def _redact(text: str) -> str:
    """Release-job output becomes a run log; keep the profile path and host name out of it."""
    if os.environ.get("CE_PRIVACY_SANITIZE_LOGS") != "1":
        return text
    for value, token in ((os.environ.get("USERPROFILE"), "%USERPROFILE%"), (os.environ.get("COMPUTERNAME"), "<host>")):
        if value:
            text = re.sub(re.escape(value).replace(r"\\", r"[\\/]"), token, text, flags=re.IGNORECASE)
    return text


def _say(message: str) -> None:
    print(_redact(message), flush=True)


def _tail(text: str, lines: int = DETAIL_LINES) -> List[str]:
    kept = [line.rstrip() for line in (text or "").splitlines() if line.strip()]
    return kept[-lines:]


def _git_env() -> Dict[str, str]:
    """In the release job, authorize git over HTTPS in memory only (never `gh auth setup-git`)."""
    env = dict(os.environ)
    token = env.get("GH_TOKEN")
    if token and env.get("GITHUB_ACTIONS") == "true":
        basic = base64.b64encode(f"x-access-token:{token}".encode("ascii")).decode("ascii")
        env.update({
            "GIT_CONFIG_COUNT": "1",
            "GIT_CONFIG_KEY_0": "http.https://github.com/.extraheader",
            "GIT_CONFIG_VALUE_0": f"AUTHORIZATION: basic {basic}",
        })
    env["GIT_TERMINAL_PROMPT"] = "0"
    return env


def _run(command: Sequence[str], *, timeout: float = 600, env: Optional[Mapping[str, str]] = None,
         cwd: str = PROJECT_ROOT) -> subprocess.CompletedProcess:
    return subprocess.run(list(command), cwd=cwd, env=dict(env) if env is not None else None, capture_output=True,
                          text=True, encoding="utf-8", errors="replace", timeout=timeout, stdin=subprocess.DEVNULL)


def _timed(name: str, function: Callable[[], CheckResult]) -> CheckResult:
    started = time.time()
    try:
        result = function()
    except Exception as error:  # noqa: BLE001 - a crashing check is a failed check, not a crashed preflight
        result = CheckResult(name, "fail", 0.0, f"{type(error).__name__}: {error}")
    result.seconds = round(time.time() - started, 1)
    return result


def _command_check(name: str, command: Sequence[str], ok_summary: str, timeout: float = 600) -> CheckResult:
    result = _run(command, timeout=timeout)
    if result.returncode == 0:
        return CheckResult(name, "pass", 0.0, ok_summary)
    output = "\n".join(part for part in (result.stdout, result.stderr) if part)
    return CheckResult(name, "fail", 0.0, f"exit {result.returncode}", _tail(output))


# -- version and tags -----------------------------------------------------------------
def remote_release_numbers(remote: str = "origin") -> List[int]:
    result = _run(["git", "ls-remote", "--tags", remote, "refs/tags/v0.1.*"], env=_git_env(), timeout=120)
    if result.returncode != 0:
        raise ReleaseError(f"git ls-remote failed: {' '.join(_tail(result.stderr, 2))}")
    numbers = []
    for line in result.stdout.splitlines():
        match = re.search(r"refs/tags/v0\.1\.(\d+)$", line.strip())
        if match:
            numbers.append(int(match.group(1)))
    return numbers


def choose_version(requested: str, existing: Sequence[int]) -> str:
    requested = (requested or "").strip()
    if requested:
        match = VERSION_PATTERN.match(requested)
        if not match:
            raise ReleaseError(f"version must match 0.1.N, got {requested!r}")
        if int(match.group(1)) < 1:
            raise ReleaseError("build number must be >= 1")
        if int(match.group(1)) in existing:
            raise ReleaseError(f"tag v{requested} already exists on origin; a release is never overwritten")
        return requested
    if not existing:
        raise ReleaseError("no v0.1.* release tags exist yet; pass --version (e.g. 0.1.5266)")
    return f"0.1.{max(existing) + 1}"


# -- the checks --------------------------------------------------------------------
class _BuildLogRouter:
    """Routes build.py's `log()` per thread, so in-process checks capture their own lines."""

    def __init__(self) -> None:
        self._buffers: Dict[int, List[str]] = {}
        self._lock = threading.Lock()

    def __call__(self, message: Any = "", *args: Any, **kwargs: Any) -> None:
        with self._lock:
            buffer = self._buffers.get(threading.get_ident())
        if buffer is not None:
            buffer.append(str(message))

    def capture(self) -> List[str]:
        buffer: List[str] = []
        with self._lock:
            self._buffers[threading.get_ident()] = buffer
        return buffer


class Preflight:
    def __init__(self, *, ci: bool, version: str, prefetch: bool = True, clang_tidy: bool = False,
                 build_module: Any = None) -> None:
        self.ci = ci
        self.version = version
        self.prefetch = prefetch
        self.clang_tidy = clang_tidy
        self.build = build_module
        self.closure_drift: List[str] = []
        self.toolchain_sha256 = ""
        self.router = _BuildLogRouter()

    def _build(self) -> Any:
        if self.build is None:
            import build  # noqa: PLC0415 - heavy facade, only when a check needs it

            self.build = build
        return self.build

    def checks(self) -> List[tuple]:
        checks: List[tuple] = [
            ("python", self.check_python),
            ("changelog", self.check_changelog),
            ("size_ratchets", self.check_size_ratchets),
            ("flake8", self.check_flake8),
            ("pyright", self.check_pyright),
            ("module_boundaries", self.check_module_boundaries),
            ("python_self_tests", self.check_self_tests),
            ("toolchain", self.check_toolchain),
            ("closure_proofs", self.check_closure_proofs),
            ("build_lock", self.check_build_lock),
        ]
        if not self.ci:
            checks.insert(0, ("git_state", self.check_git_state))
        if self.prefetch:
            checks.append(("sources", self.check_sources))
        if self.clang_tidy:
            checks.append(("clang_tidy_ratchet", self.check_clang_tidy))
        return checks

    def run(self) -> List[CheckResult]:
        build = self._build()
        saved_log = build.log
        build.log = self.router
        results: List[CheckResult] = []
        try:
            with ThreadPoolExecutor(max_workers=8) as executor:
                futures = [executor.submit(_timed, name, function) for name, function in self.checks()]
                for future in futures:
                    result = future.result()
                    results.append(result)
                    _print_result(result)
        finally:
            build.log = saved_log
        return results

    # individual checks -------------------------------------------------------------
    def check_python(self) -> CheckResult:
        if sys.version_info < (3, 12):
            return CheckResult("python", "fail", 0.0, f"Python 3.12+ required, running {sys.version.split()[0]}")
        return CheckResult("python", "pass", 0.0, sys.version.split()[0])

    def check_git_state(self) -> CheckResult:
        branch = _run(["git", "rev-parse", "--abbrev-ref", "HEAD"]).stdout.strip()
        problems = []
        if branch != "main":
            problems.append(f"on branch {branch!r}; releases are dispatched from main")
        dirty = [line for line in _run(["git", "status", "--porcelain", "--untracked-files=no"]).stdout.splitlines()
                 if line.strip()]
        if dirty:
            problems.append(f"{len(dirty)} tracked file(s) modified; the release builds origin/main, not this tree")
        fetch = _run(["git", "fetch", "--quiet", "origin", "main"], env=_git_env(), timeout=120)
        if fetch.returncode != 0:
            problems.append("git fetch origin main failed: " + " ".join(_tail(fetch.stderr, 1)))
        else:
            head = _run(["git", "rev-parse", "HEAD"]).stdout.strip()
            remote = _run(["git", "rev-parse", "origin/main"]).stdout.strip()
            if head != remote:
                problems.append(f"HEAD {head[:10]} is not origin/main {remote[:10]}: push or merge (main requires "
                                "a PR) and pull first")
        if problems:
            return CheckResult("git_state", "fail", 0.0, problems[0], problems[1:])
        return CheckResult("git_state", "pass", 0.0, "main, clean, equal to origin/main")

    def check_changelog(self) -> CheckResult:
        script = os.path.join(PROJECT_ROOT, "tools", "manage_changelog.py")
        validate = _run([sys.executable, script, "--validate"])
        if validate.returncode != 0:
            return CheckResult("changelog", "fail", 0.0, "CHANGELOG.md validation failed",
                               _tail(validate.stdout + validate.stderr))
        head = _run(["git", "rev-parse", "HEAD"]).stdout.strip()
        with tempfile.TemporaryDirectory() as scratch:
            notes = os.path.join(scratch, "notes.md")
            generate = _run([sys.executable, script, "--generate-release-notes", "--version", self.version,
                             "--commit", head, "--output", notes])
            if generate.returncode != 0 or not os.path.isfile(notes):
                return CheckResult("changelog", "fail", 0.0, f"release notes for {self.version} cannot be generated",
                                   _tail(generate.stdout + generate.stderr))
        return CheckResult("changelog", "pass", 0.0, f"valid; release notes for {self.version} generate")

    def check_size_ratchets(self) -> CheckResult:
        build = self._build()
        lines = self.router.capture()
        details: Dict[str, Any] = {}
        try:
            build.evaluate_file_size_baseline(build.collect_source_file_sizes(), details, mutate_baseline=False)
            build.evaluate_source_line_length(details, mutate_baseline=False)
        except SystemExit:
            return CheckResult("size_ratchets", "fail", 0.0, "file-size or line-length ratchet regressed", lines[-12:])
        return CheckResult("size_ratchets", "pass", 0.0, "file size and line length within baselines")

    def check_flake8(self) -> CheckResult:
        return _command_check("flake8", [sys.executable, "-m", "flake8",
                                         f"--config={os.path.join(PROJECT_ROOT, 'tools', 'config', '.flake8')}",
                                         "build.py", "tools", "testapp"], "clean")

    def check_pyright(self) -> CheckResult:
        return _command_check("pyright", [sys.executable, "-m", "pyright", "-p",
                                          os.path.join(PROJECT_ROOT, "tools", "config", "pyrightconfig.json")],
                              "clean")

    def check_module_boundaries(self) -> CheckResult:
        return _command_check("module_boundaries",
                              [sys.executable, os.path.join(PROJECT_ROOT, "tools", "check_module_boundaries.py"),
                               "--json"], "no new dependency violations")

    def check_self_tests(self) -> CheckResult:
        from tools.python_tool_self_tests import run_tool_self_tests  # noqa: PLC0415

        results = run_tool_self_tests(project_root=PROJECT_ROOT, python_executable=sys.executable,
                                      jobs=min(8, os.cpu_count() or 4), exclude=BUILD_OUTPUT_SUITES)
        failed = [result for result in results if result.returncode != 0]
        if failed:
            details = []
            for result in failed[:3]:
                details.append(f"--- {result.name} (exit {result.returncode})")
                details.extend(_tail(result.stdout + "\n" + result.stderr, 8))
            return CheckResult("python_self_tests", "fail", 0.0,
                               f"{len(failed)} of {len(results)} suite(s) failed: "
                               + ", ".join(result.name for result in failed), details)
        return CheckResult("python_self_tests", "pass", 0.0,
                           f"{len(results)} suites (build-output suites {', '.join(BUILD_OUTPUT_SUITES)} run in the "
                           "build)")

    def check_toolchain(self) -> CheckResult:
        build = self._build()
        state = build.toolchain_state
        if state.pacman_busy(build.MSYS2_DIR):
            return CheckResult("toolchain", "fail", 0.0, "pacman holds its database lock: the toolchain is being "
                               "changed right now (or a crashed pacman left db.lck behind)")
        broken = [probe for probe in state.probe_tools(build.MSYS2_DIR) if not probe.ok]
        lines = self.router.capture()
        try:
            build.verify_msys2_ffmpeg_build_deps(build.MSYS2_DIR)
        except RuntimeError as error:
            return CheckResult("toolchain", "fail", 0.0, str(error), lines[-4:])
        if broken:
            return CheckResult("toolchain", "fail", 0.0, f"{len(broken)} tool(s) cannot run",
                               [f"{probe.tool}: {probe.detail}" for probe in broken])
        packages = state.toolchain_packages(build.MSYS2_DIR)
        self.toolchain_sha256 = state.fingerprint(packages)
        return CheckResult("toolchain", "pass", 0.0, f"{len(packages)} packages ({self.toolchain_sha256[:12]}), "
                           f"{len(state.TOOL_PROBES)} tools run")

    def check_closure_proofs(self) -> CheckResult:
        build = self._build()
        state = build.toolchain_state
        packages = state.toolchain_packages(build.MSYS2_DIR)
        toolchain_sha256 = state.fingerprint(packages)
        state_dir = build.toolchain_state_dir()
        inputs = build.closure_input_fingerprints()
        proven, unknown, details = [], [], []
        for kind in state.CLOSURE_KINDS:
            proof = state.find_proof(state_dir, kind, inputs[kind], toolchain_sha256)
            if proof:
                proven.append(f"{kind} ({proof.get('origin')}, {proof.get('recorded_at')})")
                continue
            last = state.latest_proof(state_dir, kind)
            previous = state.proof_packages(state_dir, last["toolchain_sha256"]) if last else None
            if previous is None:
                unknown.append(kind)
                continue
            changes = state.package_changes(previous, packages)
            if not changes:
                details.append(f"{kind}: inputs changed since the last proof ({last.get('recorded_at')}); "
                               "toolchain unchanged")
                continue
            self.closure_drift.append(kind)
            details.append(f"{kind}: {len(changes)} package change(s) since its last successful build "
                           f"({last.get('origin')}, {last.get('recorded_at')}):")
            details.extend(f"    {change}" for change in changes[:10])
            if len(changes) > 10:
                details.append(f"    ... {len(changes) - 10} more")
        if self.closure_drift:
            details.append("Prove it before dispatching: python tools/rehearse_dependency_closure.py (~11 min), "
                           "or a default `python build.py`, which rebuilds the closure and FFmpeg from source")
            return CheckResult("closure_proofs", "warn", 0.0,
                               f"{', '.join(self.closure_drift)} never built with this toolchain", details)
        if unknown:
            return CheckResult("closure_proofs", "warn", 0.0,
                               f"no successful build of {', '.join(unknown)} recorded on this machine yet", details)
        return CheckResult("closure_proofs", "pass" if not details else "warn", 0.0,
                           "proven: " + "; ".join(proven) if proven else "inputs changed", details)

    def check_build_lock(self) -> CheckResult:
        build = self._build()
        lock = build.MachineLock(build.MACHINE_BUILD_LOCK_NAME,
                                 build.shared_cache_path(build.PROJECT_ROOT, "locks", msys2_dir=build.MSYS2_DIR))
        if lock.try_acquire():
            lock.release()
            return CheckResult("build_lock", "pass", 0.0, "no other build.py running")
        return CheckResult("build_lock", "warn", 0.0, "another build.py is running; the release build will wait for "
                           "it: " + describe_holder(lock.holder()))

    def check_sources(self) -> CheckResult:
        build = self._build()
        lines: List[str] = []
        stats = build.prefetch_external_sources(lambda message: lines.append(str(message)))
        downloaded = [line for line in lines if "Downloading" in line or "Creating" in line or "missing;" in line]
        summary = (f"all sources cached and verified in {stats['seconds']}s "
                   f"({len(downloaded)} network fetch(es), {stats['recipes_verified']} recipe(s) re-verified)")
        return CheckResult("sources", "pass", 0.0, summary, downloaded[:DETAIL_LINES])

    def check_clang_tidy(self) -> CheckResult:
        build = self._build()
        lines = self.router.capture()
        env, _ = build.get_env()
        try:
            build.run_verify_preflight(env)
        except SystemExit:
            return CheckResult("clang_tidy_ratchet", "fail", 0.0, "clang-tidy or size ratchet regressed", lines[-12:])
        if any("deferred" in line for line in lines):
            return CheckResult("clang_tidy_ratchet", "warn", 0.0, "no compatible full compile database; run the "
                               "closing gate (a full product build) first, or the release job checks it after building")
        cache = next((line for line in lines if "cache:" in line), "")
        return CheckResult("clang_tidy_ratchet", "pass", 0.0, cache.strip() or "within baseline")


def _print_result(result: CheckResult) -> None:
    label = {"pass": "PASS", "warn": "WARN", "fail": "FAIL"}[result.status]
    _say(f"[{label}] {result.name:<20} {result.seconds:6.1f}s  {result.summary}")
    if result.status != "pass" or result.name == "sources":
        for line in result.details[:DETAIL_LINES + 4]:
            _say(f"         {line}")


def write_report(results: Sequence[CheckResult], *, version: str, ci: bool, started: float, toolchain: str) -> str:
    report = {
        "version": version,
        "mode": "ci" if ci else "local",
        "started_at": datetime.datetime.fromtimestamp(started).isoformat(timespec="seconds"),
        "seconds": round(time.time() - started, 1),
        "toolchain_sha256": toolchain,
        "checks": [{key: value for key, value in asdict(result).items() if key != "details"} for result in results],
        "failed": [result.name for result in results if result.status == "fail"],
    }
    path = os.path.join(PROJECT_ROOT, "build", "verification", REPORT_NAME)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as target:
        json.dump(report, target, indent=2)
        target.write("\n")
    return path


def run_preflight(*, ci: bool, version: str, prefetch: bool, clang_tidy: bool) -> tuple:
    started = time.time()
    _say(f"=== Stable release preflight for {version} ({'release job' if ci else 'local'}) ===")
    preflight = Preflight(ci=ci, version=version, prefetch=prefetch, clang_tidy=clang_tidy)
    results = preflight.run()
    write_report(results, version=version, ci=ci, started=started, toolchain=preflight.toolchain_sha256)
    failed = [result.name for result in results if result.status == "fail"]
    warned = [result.name for result in results if result.status == "warn"]
    _say(f"=== Preflight {'FAILED' if failed else 'passed'} in {time.time() - started:.0f}s"
         + (f": {', '.join(failed)}" if failed else "") + (f" (warnings: {', '.join(warned)})" if warned else "")
         + " ===")
    return results, preflight


# -- dispatch ----------------------------------------------------------------------
def _gh_json(arguments: Sequence[str]) -> Any:
    result = _run(["gh", *arguments], timeout=120)
    if result.returncode != 0:
        raise ReleaseError(f"gh {' '.join(arguments[:2])} failed: {' '.join(_tail(result.stderr, 2))}")
    return json.loads(result.stdout or "null")


def runner_status(repository: str) -> Optional[Dict[str, Any]]:
    runners = _gh_json(["api", f"repos/{repository}/actions/runners"]).get("runners", [])
    return next((runner for runner in runners if runner.get("name") == RUNNER_NAME), None)


def find_dispatched_run(head: str, after: float, attempts: Iterator[int]) -> Optional[Dict[str, Any]]:
    """The run GitHub created for our dispatch; creation is asynchronous, so look a few times."""
    for _ in attempts:
        runs = _gh_json(["run", "list", "--workflow", WORKFLOW_FILE, "--branch", "main", "--event",
                         "workflow_dispatch", "--limit", "5", "--json", "databaseId,createdAt,headSha,url,status"])
        for run in runs or []:
            created = datetime.datetime.fromisoformat(run["createdAt"].replace("Z", "+00:00")).timestamp()
            if run.get("headSha") == head and created >= after - 5:
                return run
        time.sleep(3)
    return None


def dispatch(arguments: argparse.Namespace) -> int:
    version = choose_version(arguments.version, remote_release_numbers())
    results, preflight = run_preflight(ci=False, version=version, prefetch=True,
                                       clang_tidy=not arguments.skip_clang_tidy)
    if any(result.status == "fail" for result in results):
        _say("Not dispatching: fix the failures above; each would have failed the release job.")
        return 1
    if preflight.closure_drift and not arguments.accept_unproven_toolchain:
        _say("Not dispatching: the FFmpeg closure was never built with this toolchain and it changed since the last "
             "success (2026-11-01 lost two runs to exactly this). Run the rehearsal named above, or pass "
             "--accept-unproven-toolchain to let the release job be the first build.")
        return 1
    repository = _gh_json(["repo", "view", "--json", "nameWithOwner"])["nameWithOwner"]
    runner = runner_status(repository)
    if not runner or runner.get("status") != "online":
        _say(f"Runner '{RUNNER_NAME}' is {runner.get('status') if runner else 'not registered'}: start "
             r"%USERPROFILE%\Programme\build\actions-runner\run.cmd and rerun.")
        return 1
    busy = " (busy with another job)" if runner.get("busy") else ""
    head = _run(["git", "rev-parse", "HEAD"]).stdout.strip()
    plan = f"release-stable on main @ {head[:10]}, version {version}, attest={arguments.attest}, runner online{busy}"
    if not arguments.yes:
        _say(f"Ready: {plan}.\nDry run - nothing was dispatched. Rerun with --yes to start the release.")
        return 0
    started = time.time()
    dispatched = _run(["gh", "workflow", "run", WORKFLOW_FILE, "--ref", "main", "-f", f"version={version}",
                       "-f", f"attest={arguments.attest}"], timeout=120)
    if dispatched.returncode != 0:
        raise ReleaseError("gh workflow run failed: " + " ".join(_tail(dispatched.stderr, 3)))
    run = find_dispatched_run(head, started, iter(range(10)))
    _say(f"Dispatched {plan}." + (f"\n{run['url']}" if run else "\n(run not listed yet; see the Actions tab)"))
    if run and arguments.watch:
        return subprocess.call(["gh", "run", "watch", str(run["databaseId"]), "--exit-status"])
    return 0


# -- command line ------------------------------------------------------------------
def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=(__doc__ or "").splitlines()[0])
    commands = parser.add_subparsers(dest="command", required=True)

    preflight = commands.add_parser("preflight", help="fail-fast release readiness check")
    preflight.add_argument("--version", default="", help="release version 0.1.N (default: next after the newest tag)")
    preflight.add_argument("--ci", action="store_true", help="release-job mode (no git-state or clang-tidy checks)")
    preflight.add_argument("--no-prefetch", action="store_true", help="skip filling and verifying the source caches")
    preflight.add_argument("--clang-tidy", action="store_true", help="also run the clang-tidy ratchet (local mode)")

    resolve = commands.add_parser("resolve-version", help="pick and check the release version (release job)")
    resolve.add_argument("--version", default="")
    resolve.add_argument("--github-output", action="store_true", help="write version=... to $GITHUB_OUTPUT")

    release = commands.add_parser("dispatch", help="preflight, check the runner, then dispatch release-stable")
    release.add_argument("--version", default="")
    # The repository is public, so `always` attests; unlike `auto` it fails closed if that ever changes.
    release.add_argument("--attest", choices=("auto", "always", "never"), default="always")
    release.add_argument("--yes", action="store_true", help="actually dispatch (default is a dry run)")
    release.add_argument("--watch", action="store_true", help="follow the dispatched run with `gh run watch`")
    release.add_argument("--skip-clang-tidy", action="store_true", help="skip the local clang-tidy ratchet")
    release.add_argument("--accept-unproven-toolchain", action="store_true",
                         help="dispatch even though the FFmpeg closure was never built with this toolchain")

    commands.add_parser("summarize", help="write the release job summary (release job, always)")
    arguments = parser.parse_args(argv)

    try:
        if arguments.command == "resolve-version":
            version = choose_version(arguments.version, remote_release_numbers())
            _say(f"Release version {version} (tag v{version} is free)")
            if arguments.github_output and os.environ.get("GITHUB_OUTPUT"):
                with open(os.environ["GITHUB_OUTPUT"], "a", encoding="utf-8") as output:
                    output.write(f"version={version}\n")
            return 0
        if arguments.command == "preflight":
            version = arguments.version or choose_version("", remote_release_numbers())
            results, _ = run_preflight(ci=arguments.ci, version=version, prefetch=not arguments.no_prefetch,
                                       clang_tidy=arguments.clang_tidy and not arguments.ci)
            return 1 if any(result.status == "fail" for result in results) else 0
        if arguments.command == "dispatch":
            return dispatch(arguments)
        return summarize_command(PROJECT_ROOT)
    except ReleaseError as error:
        _say(f"ERROR: {error}")
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
