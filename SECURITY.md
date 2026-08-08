# Security Policy

## Reporting a vulnerability

Please do not disclose a suspected vulnerability in a public issue, discussion, pull request, log, or dump. Use
[GitHub's private vulnerability reporting form](https://github.com/aufkrawall/capture-engine/security/advisories/new).

Include the affected version or commit, expected security impact, a minimal reproduction, and any relevant platform
details. Share only the smallest diagnostic excerpt needed and remove credentials, tokens, personal paths, captures,
process data, and other private information. Do not test against systems, accounts, games, or services you do not own
or have explicit permission to assess.

Reports are evaluated privately. Because this is a free-time project, no response or remediation deadline is
guaranteed. Please coordinate disclosure through the private advisory until a fix and release are ready.

## Supported versions

Security fixes target the latest stable release and the current `main` branch. Older releases may be asked to upgrade
before a report is investigated or fixed.

## Release integrity

Stable releases include verification evidence and corresponding source for the redistributed LGPL components. Public
release assets carry GitHub artifact attestations. The Windows binaries are not currently Authenticode-signed; see
[Release verification](README.md#release-verification) for the verification command and trust boundary.
