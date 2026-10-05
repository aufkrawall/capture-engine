<!--
SPDX-License-Identifier: MIT
Copyright (c) 2026 aufkrawall
-->

# Secret Leak Prevention

Last cross-checked: 2026-10-05 (template baseline at `1fcfac5`; local Gitleaks 8.30.1 command help).

Primary sources:
- `AGENTS.md`
- `.gitignore`
- `llm-wiki/debug-tools.md`
- upstream `llm-wiki-agents.md-template/llm-wiki/secret-leak-prevention.md`
  in [llm-prompt-templates](https://github.com/aufkrawall/llm-prompt-templates/tree/1fcfac55c967f673af9902b481f0c38aacb8d00a)


This procedure is part of the normal agent commit workflow. It applies whenever an agent is authorized to create a Git commit, independently of whether a full security audit is being performed.

The goal is to prevent credentials, private keys, tokens, sensitive configuration, private data, local diagnostic artifacts, or accidentally copied secrets from entering Git history or commit metadata.

## Sensitive material to protect

Treat at least the following as sensitive unless repository policy explicitly establishes otherwise:

- passwords, API keys, bearer tokens, OAuth/client secrets, session cookies, refresh tokens, access tokens, cloud credentials, database credentials, and connection strings containing credentials
- private keys, signing keys, certificate private material, recovery codes, seed phrases, and authentication cookies
- real `.env`/local configuration values, credential stores, auth caches, package-manager tokens, and machine-specific secret files
- dumps, captures, traces, logs, exported requests, screenshots, databases, user data, or generated artifacts that may contain credentials or personal/private information
- credentials or sensitive values embedded in source, tests, fixtures, examples, generated files, documentation, changelog entries, commit messages, trailers, or other Git metadata

Public identifiers, intentionally public test fixtures, and documented placeholders are not automatically secrets, but verify that they are non-sensitive before committing them.

## Mandatory pre-commit check

Before every agent-created commit:

1. Inspect the complete staged-file inventory and relevant untracked files intended for staging.
2. Review the staged patch, not only the working-tree diff. Use Git's staged diff/check facilities or an equivalent repository workflow.
3. Check for unexpectedly staged local configuration, credential files, keys, dumps, logs, captures, databases, generated artifacts, or other sensitive files.
4. Run the repository-provided secret scan/check when one exists.
5. When a suitable local secrets scanner such as `gitleaks` or `trufflehog` is already available, use it on the staged/changed content or repository scope appropriate to the project.
6. If no scanner is available, the check is still mandatory: perform a targeted manual review/search for credential markers and suspicious secret-bearing files, and record that automated scanning was unavailable.
7. Review the planned commit message/body/trailers before committing. Do not paste raw secrets, sensitive log excerpts, private URLs containing credentials, tokens, or personal data into commit metadata.
8. If any suspected secret or sensitive artifact is found, stop the commit until it is removed, redacted, replaced with a safe fixture/placeholder, or explicitly established as safe to commit.

A clean scanner result does not replace staged-diff review. Secret scanners can miss custom formats, encoded values, private data, or sensitive artifacts that are not recognizable as credentials.

## Mandatory post-commit check

Immediately after every agent-created commit and before any push:

1. Inspect the exact commit that was created, including its complete patch, file list, commit message/body/trailers, and author/committer metadata. A command equivalent to `git show --format=fuller --stat --patch HEAD` is appropriate.
2. Confirm that the commit contains only intended task-owned files and no secret-bearing local/generated artifacts.
3. Re-run the repository-provided or available local secret scanner against the newly created commit or the relevant commit range when the tool supports Git-history/commit scanning.
4. If automated commit scanning is unavailable, manually re-check the committed patch and metadata for credential material and sensitive values.
5. Do not push, publish, open a release from, or otherwise share a commit that fails this check.

For a multi-commit outgoing branch, prefer an additional secrets scan/review over the complete outgoing range before pushing when practical.

## Remediation when a secret reaches a commit

If a real credential or other sensitive value is found after commit:

- stop before pushing or otherwise sharing the commit;
- remove the sensitive material from the working tree and rewrite/amend the affected local commit(s) as appropriate;
- do not quote the full secret in remediation notes, changelog entries, issue/PR text, or commit messages;
- treat a real credential as potentially exposed and rotate/revoke it according to repository/security policy, especially if the commit, patch, terminal output, logs, or scanner results may have left the local trusted environment;
- if the commit was already pushed/shared, follow the project's incident-response/history-rewrite procedure rather than assuming deletion of the latest branch tip removes the secret from all reachable history or caches.

## Reporting

When reporting secret-check results:

- state which staged/commit scope was checked and which scanner or manual fallback was used;
- report suspected secrets using a redacted fingerprint or location, never the complete value;
- distinguish "scanner unavailable" from "scan passed";
- do not claim that a repository is secret-free merely because one scan found no matches.

## CaptureEngine commands and boundaries

Run these commands from the repository root in PowerShell. Resolve the installed scanner with
`Get-Command gitleaks -ErrorAction Stop` or the verified path guidance in `debug-tools.md`.
The commands below match Gitleaks 8.30.1; check command help if the installed version differs.
Do not install a scanner or change Git hooks, PATH, or system settings merely to follow this procedure.

### Before each commit

Inventory task-owned paths with `git status --short --untracked-files=all` and stage only explicit paths.
Review `git diff --cached --name-status`, `git diff --cached --check`, and `git diff --cached`.
Review relevant untracked files before staging; ignored files are not proof of safe content.
Then scan the index, including newly staged files:

```powershell
gitleaks git --pre-commit --staged --no-banner --redact --timeout 60
if ($LASTEXITCODE -ne 0) { throw "Staged secret scan failed; do not commit." }
```

Review the planned title, body, and trailers separately, including private paths, credential-bearing
URLs, and copied log lines. When using a prepared commit-message file, scan that exact file too:

```powershell
gitleaks dir <commit-message-file> --no-banner --redact --timeout 60
if ($LASTEXITCODE -ne 0) { throw "Commit-message secret scan failed; do not commit." }
```

### After each commit

Inspect `git show --format=fuller --stat --patch HEAD` for the exact files, patch, message,
trailers, and author/committer metadata. Expected Git attribution is intentional metadata;
unexpected identities or private data still require review. Scan the committed patch:

```powershell
gitleaks git --log-opts="-1 HEAD" --no-banner --redact --timeout 60
if ($LASTEXITCODE -ne 0) { throw "Commit secret scan failed; do not share this commit." }
```

Git-history scanners primarily inspect patches. Manually review metadata and scan it separately
with Gitleaks stdin when available; do not assume the history scan covered the commit message.
Inspect the producer's result as well as the scanner's result so an empty failed Git command
cannot count as a successful check:

```powershell
$commitMetadata = git show -s --format=fuller HEAD
if ($LASTEXITCODE -ne 0 -or -not $commitMetadata) { throw "Cannot read commit metadata." }
$commitMetadata | gitleaks stdin --no-banner --redact --timeout 60
if ($LASTEXITCODE -ne 0) { throw "Commit-metadata secret scan failed; do not share this commit." }
```

Before an authorized push, additionally review/scan the full outgoing range against the actual
upstream base, e.g. `gitleaks git --log-opts="origin/main..HEAD" --no-banner --redact --timeout 60`.
Check that the base exists and the expected commits were scanned; no commits scanned is not
proof of a clean non-empty range. Secret checks never authorize a push or release by themselves.

### Manual fallback and sensitive artifacts

If no suitable scanner is available, review all added/changed staged content, relevant untracked
files, the planned message, and then the exact committed patch and metadata. Search these scopes
for credential assignments, bearer/auth headers, private-key blocks, connection strings, and
credential-bearing URLs; inspect long encoded/high-entropy values and confirm fixtures are safe
placeholders. Filename/keyword searches alone are insufficient. A scanner failure or timeout is
not a clean result: resolve it or report an explicit manual fallback and its coverage limits.

CaptureEngine logs, dumps, captures, screenshots, traces, private PDBs, local `tool-paths.env`,
authentication files, and generated manifests require manual artifact review even after a clean
credential scan. Keep them out of commits; `.gitignore` is only a guard against accidental staging.
Do not print suspected secret values while inspecting or searching. Review potentially sensitive
material locally and report only redacted locations/fingerprints. Scanner reports are sensitive
local artifacts and must not be committed.

TruffleHog is an optional secondary scanner, not a required second scan. For its Windows Git
transport, use the actual repository root as a `file://C:/...` URI (single slash after the drive)
and an explicit base SHA with `--since-commit`; an `origin/main` ref inside TruffleHog's fresh clone
can resolve to its own HEAD and silently scan nothing. Avoid network credential verification
unless authorized. The installed version and flags must be checked before use.

## Open questions / stale-risk

Scanner paths and versions are machine-local and can change; resolve availability on every task.
Scanners do not recognize all private user data or custom secret formats. Staged-patch and
metadata review remain mandatory, and passing these checks does not certify the whole repository.
