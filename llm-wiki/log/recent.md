# llm-wiki Log

### 2026-08-08 - Repo recreated to purge the scrubbed objects; runner workFolder is load-bearing

- The dangling pre-scrub commits could not be removed by a force-push (see the previous
  entry), so the repository was **renamed to `capture-engine-dev2` (kept private) and a
  fresh `capture-engine` created** with only the clean history. Chosen over delete+recreate
  because it is reversible and loses nothing: the archive keeps the old run history,
  creation date and the `v0.1.5294` release. Verified afterwards: all four leaked SHAs
  answer **HTTP 422** on the new repo and the contents endpoint 404s, i.e. the objects were
  never there rather than merely unreferenced - a stronger guarantee than a Support gc.
  `capture-engine-wip` (a separate older copy) does not contain them; no public repo does.
- Restored by hand after the recreate: 1897 commits + the `v0.1.5294` tag,
  `CE_TOOLCHAIN_ROOT`, `default_workflow_permissions=read`
  (`can_approve_pull_request_reviews=false`), and the runner registration.
- **`--work` is load-bearing when re-registering the runner.** The original registration
  used an explicit `workFolder` of `C:\...\build\runner-work`; re-registering with
  `--unattended` and no `--work` silently used config.cmd's default `_work`, which sits
  under the runner directory and put the workspace at **82** characters instead of **73**.
  Run 31226827240 then died seven minutes in, after a fully successful closure and FFmpeg
  build, while "Linking Hook DLL x64":
  `EXCEPTION: [WinError 206] The filename or extension is too long`. The link step passes
  hundreds of absolute object paths on one command line and nine characters were enough to
  exceed Windows' 32767-character command-line limit. Note this is the command-line limit,
  a *different* limit from the 260-char MAX_PATH that the doxygen man pages hit - the same
  path-depth theme, two distinct mechanisms.
- The runner's work folder is machine state, so no repository test can pin it.
  `release-stable.yml` now preflights the workspace length as its **first** step (limit 76;
  measured 73 passes, 82 fails) and names the fix. Seconds instead of seven minutes, with a
  message that mentions the runner rather than a filename.
- Gotcha inside that guard: a PowerShell here-string terminator must sit at column 0, which
  dedents out of a YAML block scalar and makes the workflow unparseable. Build the message
  with `-join` instead. The first attempt did exactly this and broke the file.
- Cleanup of the stray 4.4 GB `_work` tree followed the junction rule that once destroyed
  the dev toolchain: enumerate reparse points, `[System.IO.Directory]::Delete(path, $false)`
  each one, assert none remain, and only then recurse. Both junction targets
  (`external`, `build\msys64`) were verified unchanged afterwards.

### 2026-08-07 - v0.1.5294 released; Actions run logs were a second, larger leak surface

- `v0.1.5294` published by run 31218094687 (success, `step.external_preparation` 1111 s, so
  compiled in-job), log auto-deleted (404), published assets carry no user or host name.
  Replaces the deleted `v0.1.5293`. Next version: `0.1.5295`.
- **Checking "are the dangling SHAs the only leak?" found two further surfaces.** They were
  worse than the git objects, because no SHA was needed to reach them:
  - **Six retained `release-stable` failure logs.** `release-log-cleanup.yml` deliberately
    keeps a failed run's log (`if: conclusion == 'success'`) as the only diagnostic material,
    and flags that it "still needs a manual delete once investigated". That manual step had
    never been performed, so six had accumulated - each exposing the **machine name**, which
    is the entire reason the cleanup workflow exists, and one also the mangled user path.
  - **Two logs from a `debug-token` workflow that no longer exists**, both marked *success*.
    The cleanup matches `workflows: ["release-stable"]` by name, so any other workflow's logs
    are unprotected by construction, and with the workflow file deleted nothing would ever
    have cleaned them.
- All eight deleted, each verified `404` with the same fail-closed re-check the cleanup uses.
  A sweep of **all 36 runs** with a broad pattern (the name in any spelling, which also
  catches the host name) now reports **0 leaking logs**; 24 logs remain retained and clean.
  Zero workflow artifacts.
- **Design gaps this exposed, still open** (deliberately not changed unilaterally):
  the "manual delete once investigated" step will be forgotten again - it already was, six
  times - so failed logs want an age-based auto-delete or an issue opened by the flag step;
  and the cleanup's single hard-coded workflow name means a new or deleted workflow is
  silently outside its scope.
- Remaining exposure before the repo can go public is **only** the dangling git objects from
  the history scrub: still fetchable by SHA, and the SHA is advertised in the Actions run
  list, so it needs no guessing. Needs a GitHub Support garbage-collection request, or a
  repository delete-and-recreate (which also wipes the run history advertising the SHAs).

### 2026-08-07 - nv-codec-headers pinned and given a fallback source (git.videolan.org outage)

- Release run 31215691866 (0.1.5294) built the **entire** dependency closure, including aom
  with the fixed key and no import error, then died on
  `git clone https://git.videolan.org/git/ffmpeg/nv-codec-headers.git`:
  `Failed to connect to git.videolan.org:443 after 21273 ms`. The host was still down
  minutes later, so this was an outage, not a blip.
- **Two real gaps, both fixed together:**
  - `git_clone` had **no retry at all**, while `download_file` has had bounded retry since
    `0208d09b`. Same lesson - a run that has already compiled for minutes must not be lost
    to one unreachable host - applied to a path that had been missed.
  - `nv-codec-headers` was cloned from **master, unpinned** (the `llm-wiki`/hand-off
    follow-up), so a fresh build took whatever upstream had merged that day.
- Fix: `FFNVCODEC_SOURCE_REF` pins the exact commit previous releases were already built
  against (`eddcea9e...`, "Bump for (in-dev) 13.1.15.1"), so pinning changes nothing about
  the product and only removes the non-determinism. `FFNVCODEC_URLS` adds the FFmpeg
  project's own GitHub mirror as a second source, and `git_clone` now tries each source
  twice, deleting any partial tree between attempts so a half-clone is never mistaken for
  a usable one.
- **The fallback is only sound because of the pin.** Both hosts were verified to serve that
  commit with the identical tree hash `2fd41cd5544091f6d0d27d0771a9cb7b838fd554`, so which
  host answers cannot change what is built. Without a pin, a second host would silently be
  a second source of truth.
- The ref feeds `ffmpeg_build_configuration_fingerprint()` for the same reason as
  `FFMPEG_SOURCE_REF`: `--skip-updates` builds return early when prebuilt DLLs exist, before
  the source is consulted, so a pin change outside the fingerprint would keep shipping
  FFmpeg built against the previous NVENC headers.
- Verified live while git.videolan.org was actually down: the real `git_clone` logged
  `Clone of ffnvcodec from git.videolan.org failed; trying the next source`, cloned from
  github.com, and checked out the pinned commit.

### 2026-08-07 - History scrubbed a third time; a force-push does NOT purge GitHub

- Documenting the log-scrub fix put the maintainer's real user name into four tracked files
  as `C__Users_<name>_Programme_...`, and it reached `origin/main` in three commits plus
  the `v0.1.5293` tag tree. Commit authorship is deliberately clean
  (`aufkrawall <...@users.noreply.github.com>`), so this would have been a genuinely new
  exposure rather than something already visible.
- **Why nothing caught it:** `test_no_developer_user_paths_in_tracked_files` matched only
  path-shaped occurrences, the same blind spot the binary scrub had (fixed `a9590837`) and
  the log scrub had (fixed `ebf962e0`). Third instance of one defect in three places.
  `MANGLED_USER_RE` now covers the underscore-mangled identifier form.
- Scrub performed with `git filter-repo --replace-text --replace-message` (the name was in
  a commit *message* too, which `--replace-text` alone would have missed), replacing
  `C__Users_<name>_` with `C__Users_TestUser_`. `TestUser` rather than `<developer>` so the
  rewritten historical blobs still satisfy the gate's allowlist and the detector's regex,
  which rejects `<` and `>`. Minimal rewrite: `18273781` and earlier kept their SHAs.
- **The important finding: a force-push does not remove anything from GitHub.** After
  force-pushing the rewritten `main` *and* deleting the `v0.1.5293` release and tag, all
  three old commits were **still fetchable by SHA**, and the leaked comment was still
  readable through the contents API at the old ref. Unreferenced objects stay served until
  GitHub garbage-collects, which is not automatic.
  - The remedy is to ask GitHub Support to garbage-collect the repository. This repo has
    **0 forks and network_count 0**, which is what makes that possible - objects in a fork
    network cannot be removed.
  - Consequence: **the repository must stay private until that purge is confirmed.** While
    private, the dangling objects need repo access to read, so they are contained; going
    public would expose them to anyone holding a SHA.
- Local hygiene: dropped a stale `refs/remotes/scrubbed/main` from the 2026-08-04 scrub
  (no configured remote, and verified clean) and the rewritten backup branch. A bundle of
  the pre-scrub state is kept outside the worktree in
  `build/ce-pre-scrub-backup/`, deliberately not in the repo.
- `v0.1.5293` was deleted and replaced by `0.1.5294` so the published release points at a
  commit that still exists.
- Runner-stop correction: `run.cmd` is a wrapper that **restarts** `Runner.Listener` when it
  dies, so killing the listener alone does not stop the runner - it silently comes back.
  Kill the `cmd.exe` running `run.cmd` first, then the listener, and confirm the GitHub-side
  status reports `offline`.

### 2026-08-07 - Stable release v0.1.5293 published, built entirely in-job

- Run 31210650635 succeeded in 24 min. First stable release that satisfies the original
  goal: **every shipped binary was compiled by the run that published it**, so a future
  artifact attestation would cover what it claims to. Author `github-actions[bot]`,
  4 assets, not draft.
- Evidence the closure was really built in-job rather than reused:
  `step.external_preparation` took **1037 s** (local from-scratch equivalent 956 s). If that
  step ever returns in seconds, the closure is being reused and the release is not
  attestable. The run log is auto-deleted, so read `latest_summary.txt` from the release
  assets instead of grepping the log.
- Log deletion verified: `GET /actions/runs/31210650635/logs` -> **404**.
- Privacy audited independently on the published assets: no user name, no host name. Paths
  read `C:\Users\<developer>\Programme\build\runner-work\...` - user component redacted,
  directory layout surviving, which is the deliberately deferred item (needs a project-root
  `-ffile-prefix-map`), not a regression.
- Artifact attestation step was **skipped**: the repository is private and GitHub
  Free/Pro/Team cannot attest private repositories. The property that makes attestation
  meaningful now holds, so it becomes real when the repo goes public.
- Five attempts failed before this one, each on a different runner-only fault. What made
  this one pass first time was rehearsing locally first - see build.py.md "Rehearsing the
  release closure locally".
