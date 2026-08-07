# llm-wiki Log

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

### 2026-08-07 - Fixed: aom's vendored PGP key had no user ID, so gpg never imported it

- Release run 31207385807 (the first with the MAX_PATH fix — opus built in **18 s**, so
  that fix held) died at aom: `Vendored PGP key b002f08b... did not import cleanly; trying
  keyservers` → dirmngr → fail closed.
- **The dirmngr message was a red herring.** Root cause: `tools/pgp-keys/B002F08B...asc`
  contained a key with **zero user IDs**, and gpg refuses such a key
  (`new key but contains no user ID - skipped`), so it never entered the keyring and
  `has_fingerprint()` stayed false. `keys.openpgp.org` strips UIDs it has not verified, and
  that is where the previous session fetched it. Re-fetched from `keyserver.ubuntu.com`
  (same pinned fingerprint, 1 UID, imports cleanly). All 8 keys now have exactly 1 UID.
- **Why local builds could never see it:** `_reset_outputs()` reset `prefix`, `recipes` and
  `staging` but **kept `gnupg/`**, so `has_fingerprint()` short-circuited on a key imported
  weeks earlier and the import path never ran. The runner deletes `ffmpeg_build` wholesale,
  so it always runs. The blob could have been broken indefinitely.
  Fixed: `dependency_pgp.reset_keyring()` deletes the keyring *files* (not the directory —
  gpg-agent and keyboxd sockets live there and a locked socket would fail removal), so any
  local rebuild re-imports exactly as the runner does.
- PGP trust moved to the new `tools/dependency_pgp.py` and its tests to
  `tools/tests/test_dependency_pgp.py`, because `ffmpeg_dependencies.py` hit 805 lines and
  the test file 805 — the ratchet caught both. The new test module is registered in the
  tool self-tests (now 20) **and** in `release-stable.yml`'s explicit preflight list; a
  module missing from that list never runs where it matters most.
- A vendored key that will not import is now a hard error carrying gpg's own reason,
  instead of falling through to a keyserver and dying with a misleading "No dirmngr".
- Test `test_every_vendored_key_actually_imports_into_an_empty_keyring` — the only check
  that catches this. The file existed, was armored, was fingerprint-named, and
  `gpg --show-keys` even reported the pinned fingerprint; every cheaper assertion passed.
  Verified it **fails on the old blob** before restoring the fixed one.
- **Two harness traps that produced false passes while diagnosing this** (worth more than
  the fix): `GNUPGHOME` must be given to the MSYS gpg in MSYS spelling — a `C:\...` path
  makes gpg join it onto its own cwd, create nothing, and a sloppy matcher then reports
  success; and GnuPG's daemon sockets live under `GNUPGHOME`, so a long path (the agent
  scratchpad) breaks `keyboxd` outright. Take the verdict from gpg's keyring listing, never
  from matching the key file.
- Added `tools/rehearse_dependency_closure.py`: builds the whole closure against a
  throwaway 89-character root with empty downloads and empty keyring, driving the real
  builder. See build.py.md "Rehearsing the release closure locally" for the table of what
  a local build reuses that the runner does not.

### 2026-08-07 - Fixed: opus doxygen man pages exceeded MAX_PATH on the release runner

- Fourth failure in the same chain (dirmngr -> cert chain -> dropped TCP -> this). Run
  31192891717 died in the opus build: ninja `FAILED: doc/html`, doxygen
  `error: Could not open file .../doc/man/man3/C__Users_..._src_opus-1.6.1_include_.3
  for writing`.
- **Root cause:** doxygen emits one man page per *input directory*, named after the
  escaped absolute path. For opus that name is 152 characters; the containing
  `.../src/build-CLANG64/doc/man/man3/` path makes the total **259** from a dev checkout
  (one character under Windows' 260-char MAX_PATH - hence latent for years) and **313**
  from the runner workspace, which is 27 characters deeper. Nothing to do with the recent
  changes; purely path-depth dependent.
- **The obvious fix does not work.** Restricting the PKGBUILD's `pkgname` to
  `package_outputs` (so `opus-docs` is never packaged) does *not* prevent this: the
  `doc/html` target is a meson `custom_target` built by `meson compile` inside `build()`,
  which runs regardless of `pkgname`. Confirmed against the local tree - the man pages
  sit in `src/build-CLANG64/doc/man/man3/` while `src/dest/` (populated by
  `package_opus()`) is empty. `--auto-features=enabled` in the recipe is what forces the
  `docs` feature on, and it must not be relaxed: it also governs which optional codec
  features the library is built with.
- **Fix (generic, `tools/dependency_build_policy.py`):** the appended build policy forces
  `GENERATE_MAN/LATEX/RTF/XML/DOCBOOK = NO` into every `Doxyfile*` under `$srcdir`
  (including `.in` templates - meson generates the effective config during the build, so
  patching only the generated file is too late). Applied by wrapping `build()`, because
  the sources do not exist when the recipe is sourced.
- Which backends matter was **measured**, not assumed: running the opus Doxyfile with
  every backend on shows `man` is the only one whose names derive from the input path.
  LaTeX/RTF/XML/DocBook/HTML all use a content hash
  (`dir_fe80300f08587586fe06c8824e04b727.tex`, 40 chars). So only `GENERATE_MAN` is
  load-bearing; the other four are off because nothing consumes doxygen output and
  generating them is pure build cost. HTML stays *on* - it is the only doc output the
  targets declare and `package_opus()` moves `share/doc` and so requires it, which is
  what makes this lossless. (That experiment ran in a long scratch path and reproduced
  `Could not open file ... for writing` verbatim - an independent control that the
  mechanism is exactly as diagnosed.)
- Depth diagnosis for next time: a local path already **over** 260 is not a bug -
  MSYS/Cygwin tar, git and Python (`LongPathsEnabled=1` here) handle extended-length
  paths, and the extracted llvm-project tree hits 304 locally without trouble. The
  dangerous band is local **234..260** touched by a *native* Win32 tool with no
  `longPathAware` manifest; +27 puts it over on the runner.
- Also done, as the handoff intended: only the subpackages named in `package_outputs`
  are built (`opus-docs`, `iconv`, `winpthreads` were built, packaged and compressed for
  nothing), with a fail-closed check that every declared output is one the recipe
  actually declares. Dropping `iconv` also stops building a GPL-3.0-or-later subpackage
  that was never shipped.
- The rendered policy text is now hashed into `dependency_manifest_fingerprint`, so
  changing what the policy *does* invalidates a cached prefix. Without that, a policy
  edit would leave a closure built under the previous policy looking current - the same
  trap as `FFMPEG_SOURCE_REF`. It cascades into `ffmpeg_build_configuration_fingerprint`,
  which is correct: FFmpeg links the closure.
- **Verified on the real recipe, not just in tests:** built opus under an 89-character
  root (runner is 73, dev 46), where the man page would have been 345 characters. Result:
  build succeeds, no `man3` directory, HTML still generated and installed, only
  `mingw-w64-clang-x86_64-opus-1.6.1-1-any.pkg.tar.zst` produced, `libopus-0.dll`
  present.
- Tests: `DependencyBuildPolicyShellTest` executes the generated policy the way makepkg
  does (login shell, so the MSYS `find` the policy uses is on PATH - a plain `bash
  script` run silently exercises a different environment and *passed while the policy
  did nothing*, which is how that was caught). Covers pkgname reduction with the
  split-wrapper template intact, the upstream `build()` body still running after the
  policy step, `Doxyfile.in` restriction, fail-closed on an undeclared output, and
  `bash -n`. `DependencyBuildPolicyTest` covers the fingerprint coupling and the manifest
  invariant that no `-docs`/`-doc` subpackage is ever a declared output.
- **Privacy gap found in that same log:** the run redacted `C:/Users/<developer>/...`
  correctly but left `C__Users_<developer>_Programme_...` on the *next line* - the log scrub
  anchors on path separators, and doxygen's mangled name has none. Fixed in
  `sanitize_privacy_paths` with a rule anchored on the mangled `Users_` component, and
  the general rule's terminator allowlist replaced by the same negative lookahead
  `a9590837` gave the binary scrub (it still missed `;`, `)` and end-of-buffer here).
  Release logs are auto-deleted, so this was defence in depth, not an exposure.

### 2026-08-07 - Fixed: release job could not build the dependency closure (no dirmngr on the runner)

- First release after un-junctioning `ffmpeg_build` failed in `Build release product`:
  `gpg: failed to start dirmngr '/usr/bin/dirmngr': General error` ->
  `Could not retrieve and fingerprint-verify PGP key 5f944b02... for llvm-runtime`.
  The closure PGP-verifies every source tarball and **failed closed**, which is correct.
- Root cause is not runner-specific: `ffmpeg_build/dependencies/gnupg/` holds only
  sockets, no keyring, so **every** closure build re-fetched the keys from a keyserver.
  That needs dirmngr, which cannot start in the runner's non-interactive context. The
  path had never run there before because the tree used to be junctioned.
- **Fix:** the 8 pinned public keys are vendored as `tools/pgp-keys/<FINGERPRINT>.asc`
  and imported from file before any keyserver is tried (keyserver retained as fallback).
  Trust is unchanged - `has_fingerprint()` still proves the imported key carries the
  fingerprint pinned in the manifest, so the anchor is the manifest, not the keyserver.
  This removes a live network dependency from every release, which also matters for
  attestation.
- Keys were obtained over plain HTTPS (`pks/lookup?op=get`), bypassing dirmngr, and each
  was fingerprint-verified with `gpg --show-keys` before being written. `gpg --recv-keys`
  hangs even locally in a non-interactive shell - do not use it when scripting this.
- Tests: `FfmpegVendoredPgpKeyTest` - every pinned fingerprint has a vendored key, files
  are armored and fingerprint-named, and the vendored import is attempted *before* any
  keyserver.
- Proven in the job (run 31190976656): all 8 keys imported from `tools/pgp-keys`, no
  dirmngr, closure build proceeded through llvm-runtime and libiconv.
- **Follow-on failure, different cause:** that run then died on
  `<urlopen error [SSL: CERTIFICATE_VERIFY_FAILED] unable to get local issuer
  certificate>` for `https://downloads.xiph.org/...opus-1.6.1.tar.gz`, while
  `ftp.gnu.org` and `mirror.msys2.org` verified fine in the same run - the runner's
  Python store lacked an intermediate. Fix: `_download_file` now uses an SSL context
  built from the MSYS2 tree's own `etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem`.
  Verified directly: that bundle fetches the failing URL with HTTP 200. Verification is
  **not** disabled anywhere - a test asserts no `CERT_NONE`,
  `_create_unverified_context` or `check_hostname = False` appears - because the
  SHA256 and PGP checks are defence in depth, not a reason to drop TLS.
- **Third failure, transient:** run 31192282693 then died with `Remote end closed
  connection without response` fetching `llvm-project-22.1.8.src.tar.xz` from github.com
  - a dropped TCP connection, with no retry, losing a release that had already compiled
  for half an hour. (That run had **zero** certificate failures, so the CA-bundle fix
  held.)
- Fix: download concerns moved into the new `tools/source_download.py` unit (which also
  took `ffmpeg_dependencies.py` from 800 back to 788 - the ceiling was the reason the
  retry could not simply be added in place). `download_file` retries a bounded 4 attempts
  with linear backoff on transient faults only: `RemoteDisconnected`, `IncompleteRead`,
  `ConnectionError`, timeouts, and HTTP 408/425/429/5xx. `HTTPError` 404/403 is **not**
  retried - a wrong pinned URL is a bug, not weather. The body still lands in a `.tmp`
  file and is moved into place only when complete, so an interrupted attempt can never
  leave a truncated archive a later run would treat as cached.
- Tests: retry-then-succeed, 404-not-retried, bounded-attempts, and
  no-truncated-file-on-partial-read; plus the TLS assertions moved to the new module.

### 2026-08-07 - FFmpeg source pinned; release job now compiles the whole dependency closure

- **FFmpeg was never pinned.** `git_clone` did `clone --depth 1 https://git.ffmpeg.org/ffmpeg.git`,
  i.e. master HEAD, so two builds a week apart were not the same product. Now
  `FFMPEG_SOURCE_REF` (`build_linux_msys2.py`) pins it, enforced even under
  `--skip-updates`: skipping updates means "do not follow upstream", not "build
  whatever is checked out".
- The ref is part of `ffmpeg_build_configuration_fingerprint()`. This is load-bearing:
  `compile_custom_ffmpeg` returns early when prebuilt DLLs exist, *before* consulting
  the source, so a pin change outside the fingerprint would silently keep shipping the
  previous FFmpeg.
- **n9.0 was tried and rejected - it has no NMR AAC coder.** NMR landed on master after
  the 9.0 release branch was cut, so it is in *no* released FFmpeg (n9.0 is the newest
  tag). Building it drops `aac_coder` to twoloop and fails 11 unit tests with
  `Undefined constant or missing '(' in 'nmr'`; `mediaengine/audio_encoder.cpp:262`
  selects it explicitly. Pin is therefore master commit
  `86940d45aff7d59810794df3ab2b39b7b83b478c` - the last verified-green source.
  `FfmpegSourcePinTest.test_pin_keeps_the_nmr_aac_coder_available` fails if anyone
  switches to a tag pin without first checking `AAC_CODER_NMR` in `libavcodec/aacenc.c`.
- **git.ffmpeg.org refuses unadvertised objects** (`Server does not allow request for
  unadvertised object`), so a commit pin cannot be fetched shallowly. `git_clone`
  full-clones only for the commit form (tags keep `--depth 1 --branch`), and skips the
  fetch entirely when the commit is already in the local object store.
- **Release job builds the closure itself.** `ffmpeg_build` is no longer junctioned to
  the maintainer's dev tree, plus a reset step, so every release compiles FFmpeg and all
  8 source dependencies in the run that publishes them - the precondition for meaningful
  GitHub artifact attestation. Previously the release reused DLLs compiled locally days
  earlier: self-built, but not built by the job. The reset deletes a junction with
  `[System.IO.Directory]::Delete($path, $false)`, never `Remove-Item -Recurse`, which on
  Windows follows the link and would erase the real toolchain tree.
- `test_ffmpeg_dependencies` was added to the release workflow's own policy-test step; it
  was previously absent, so the pin/junction guards would not have run on the runner.
- Gate: `--verify --verify-clean` success=1 (build 0.1.5853, 820 s), FFmpeg rebuilt
  `d32b387f -> 86940d45`, privacy scan clean over 87 shipped binaries.
- Open: `nv-codec-headers` is still cloned unpinned from master. Headers only, nothing
  shipped, but the same class of non-determinism.

### 2026-08-07 - Fixed: the binary privacy scrub could neither redact nor report a user path with an unlisted terminator

- Found while re-reviewing `a65d1292..HEAD`. `_user_component_patterns` accepted
  only `[\\/= "\x00]` *after* the user name, so `C:\Users\<user>` followed by
  `;`, `)`, `'`, `>`, a newline/CRLF, a `.` (domain-profile `\<user>.DOMAIN`), or
  sitting at the **end of the buffer** did not match. The scrub and its
  verification pass share this pattern, so such an occurrence would have been
  neither rewritten nor reported - the stage would log "Privacy scan clean" while
  the path shipped. Latent, not live: an independent ground-truth scan (bare name,
  no delimiter assumptions, UTF-8 + UTF-16) over all 106 files of both shipped
  packages found zero occurrences before and after.
- **Fix:** the trailing side is now a negative lookahead on name-continuation
  characters, `(?![A-Za-z0-9_-])` (UTF-16: `(?!(?:[A-Za-z0-9_-]\x00))`). This
  still refuses a longer user name that merely starts with this one, treats `.`
  as a terminator, and matches at end of buffer.
- **Second bug, same function:** the UTF-16 spelling was built with
  `user.encode("utf-16le")` and never regex-escaped, so a user name containing
  `.` or `+` would have become a metacharacter and over-matched. Both spellings
  now go through `re.escape`.
- Lesson worth keeping: when a scrub and its verification share one pattern, the
  pattern's blind spots are invisible twice over. The independent ground-truth
  scan (different pattern, no delimiter assumptions) is what makes the gate
  trustworthy - keep using it when auditing, not the gate's own matcher.
- Regression tests: `test_count_profile_path_hits_covers_terminators_beyond_path_separators`
  (all ten terminators, each also asserted to scrub to zero),
  `test_count_profile_path_hits_still_refuses_longer_names`,
  `test_user_component_patterns_escape_regex_metacharacters`. Note the tracked-file
  gate rejects literal `C:\Users\<name>` fixtures, so those tests use approved
  placeholders or assemble the profile from parts.
- Gate: `--verify --verify-clean` success=1 (build 0.1.5850, 386 s); privacy scan
  clean over 87 shipped binaries/PDBs, 43 scrubbed.

### 2026-08-07 - Machine name and stray emails are now gated, not just inspected

- Audit finding, not a live leak: the shipped artifacts were verified clean of
  the hostname **by inspection**, while the only enforced artifact gate was
  `count_profile_path_hits`, which matches the *user name* alone. Nothing stopped
  host state from reappearing later. Verified first that enabling a gate could
  not break the build: 0 hostname hits across `captureengine.7z` (45 files) and
  `testapps.7z` (61 files).
- **Fix:** `count_machine_name_hits` (`build_privacy.py`) plus a fail-closed
  check in `build_project_finalize.py` over every shipped binary/PDB. Three
  deliberate design points:
  - **Verified, never scrubbed.** The user-name scrub exists because paths
    legitimately contain it; nothing in the toolchain should ever record the
    host, so a hit means a new leak source to identify. A source-policy test
    asserts no `scrub_machine_name` is introduced.
  - **`COMPUTERNAME` *and* `platform.node()`.** Presetting the variable does not
    change the real host name (the same fact that makes the runner's `Machine
    name` line unfixable in-job), so relying on the variable alone would let the
    gate be switched off by an env var.
  - **Names under `MACHINE_NAME_MIN_LENGTH` (8) are skipped with a logged
    reason.** A host called `PC` or `BUILD` occurs inside ordinary strings and
    mangled symbols; enforcing it would fail builds on coincidence. Skipping is
    reported, never silent.
- Tracked files also gained a machine-name scan and an email scan against
  `ALLOWED_EMAIL_DOMAINS` (allowlist, so no real address is written into the tree
  to detect it). The tree currently holds exactly two addresses, both allowed:
  `maintainer@example.com` and a `users.noreply.github.com` identity.
- Also audited and deliberately **not** changed: shipped PDBs and 7 of the 15
  FFmpeg-closure DLLs still contain `C:/Users/redact/Programme/build/captureproject/...`
  - the user component is redacted but the directory layout survives. Removing it
  needs a project-root `-ffile-prefix-map`, which rewrites source paths in every
  translation unit for a weak benefit (the residue carries layout, not identity).
  Open decision, not an oversight.
- Debug symbols re-verified against the shipped, scrubbed artifacts: PE RSDS
  GUID+age match the PDB, 2124 publics, 183/188 modules with debug info, 386
  source-file entries with MD5 checksums, and `cdb` resolves first-party symbols,
  types and `file:line:column`. Only source *file opening* needs `_NT_SOURCE_PATH`
  / `.srcpath`. Note `llvm-pdbutil`'s line-dump flag is `-l`, not `--lines`
  (the long spelling is silently rejected and looks like "no line info").
- Gate: `python build.py --verify --verify-clean --skip-updates --concise`
  success=1 (build 0.1.5849, 401 s); privacy scan clean over 87 shipped
  binaries/PDBs, 43 scrubbed.
