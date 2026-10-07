#!/usr/bin/env python3
"""Reject broken publication in the production configuration owner.

Restores exact bytes and checks the restored source. Do not run alongside builds
or edits to configuration_state.h.
"""
import sys

from check_ecl_dispatch_mutations import ROOT, assertion_failure_recorded, run_tests

SOURCE = ROOT / "captureengine/app/configuration_state.h"
COMMAND = [sys.executable, "build.py", "--incremental", "--tests-only", "--run-tests",
           "--gtest-filter=ConfigurationStateTest.*:RuntimeConfigurationTest.*",
           "--skip-updates", "--concise"]
MUTATIONS = (
    ("first poll loses an edit after startup loading", "state_.initialized = true;",
     "state_.initialized = false;", "ChangeBetweenStartupLoadAndFirstPollIsNotMistakenForAppliedSettings"),
    ("unreadable candidate publishes defaults", "!ce::config_reload::IsCoherentLoad(evidence)",
     "false", "UnreadableCandidateCannotPublishOrCommitAndAStableRetrySucceeds"),
    ("commit before load prevents a failed read from retrying", "AppConfig candidate = current_;",
     "ce::config_reload::CommitReload(state_, identity);\n        AppConfig candidate = current_;",
     "UnreadableCandidateCannotPublishOrCommitAndAStableRetrySucceeds"),
    ("file replacement during a load is ignored", "const auto evidence = files.Load(candidate, identity);",
     "auto evidence = files.Load(candidate, identity);\n        evidence.identityAfterLoad = identity;",
     "ChangedFileDuringLoadDiscardsTheCompleteCandidate"),
    ("recursive polling publishes twice", "loading_ || ", "", "RecursivePollingCannotLoadOrPublishTwice"),
    ("polling ignores its owned deadline", "WaitMs(files.NowMs()) != 0", "false",
     "SchedulingIsOwnedAndTickWrapDoesNotSkipChanges"),
)


def main() -> int:
    original = SOURCE.read_bytes()
    source = original.decode("utf-8")
    output_dir = ROOT / "build/refactor/configuration-mutations"
    output_dir.mkdir(parents=True, exist_ok=True)
    try:
        for index, (name, before, after, test) in enumerate(MUTATIONS, 1):
            if source.count(before) != 1:
                raise RuntimeError(f"mutation anchor changed: {name}")
            SOURCE.write_bytes(source.replace(before, after).encode("utf-8"))
            print(f"[Configuration mutation] checking: {name}", flush=True)
            log = output_dir / f"mutation-{index}.log"
            result = run_tests(log, COMMAND)
            if result == 0 or not assertion_failure_recorded(log, "unit", f"ConfigurationStateTest.{test}"):
                raise RuntimeError(f"configuration assertions did not reject: {name}; evidence: {log.name}")
            SOURCE.write_bytes(original)
            print(f"[Configuration mutation] detected: {name}", flush=True)
    finally:
        SOURCE.write_bytes(original)
        print("[Configuration mutation] restored exact source; verifying production", flush=True)
        restored = run_tests(output_dir / "restored.log", COMMAND)
    if restored != 0:
        raise RuntimeError("restored configuration did not pass")
    print(f"[Configuration mutation] {len(MUTATIONS)} defects detected; original passes", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
