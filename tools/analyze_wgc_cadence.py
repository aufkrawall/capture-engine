#!/usr/bin/env python3

import argparse
import re
import statistics
from pathlib import Path


CADENCE_DUP_RE = re.compile(r"TickDup=(\d+)")
CADENCE_THR_RE = re.compile(r"WgcThr=(\d+)")
CADENCE_SHORTFALL_RE = re.compile(r"Shortfall=(\d+)")
CADENCE_EMPTY_RE = re.compile(r"(?:Empty|FreshMiss)=(\d+)pm")
CADENCE_BUFAVG_RE = re.compile(r"BufAvg=(\d+)pm")
CADENCE_BUFMIN_RE = re.compile(r"BufMin=(\d+)")
CADENCE_STARVED_RE = re.compile(r"(?:Starved|NoFresh)=(\d+)")
CADENCE_SINGLE_RE = re.compile(r"(?:Single|NoReserve)=(\d+)")
CADENCE_HOLD_FRESH_RE = re.compile(r"HoldFresh=(\d+)")
CADENCE_SPEND_RE = re.compile(r"Spend=(\d+)")
CADENCE_CATCHUP_RE = re.compile(r"CatchUp=(\d+)")

PERF_INPUT_RE = re.compile(r"\bInput:\s*(\d+)")
PERF_DELIV_RE = re.compile(r"\bDeliv:\s*(\d+)")
PERF_MININ_RE = re.compile(r"\bMinIn250/500:\s*(\d+)/(\d+)")
PERF_MINDEL_RE = re.compile(r"\bMinDel250/500:\s*(\d+)/(\d+)")
PERF_DROPSTALE_RE = re.compile(r"\bDropStale:\s*(\d+)")
PERF_DROPSTALE_DETAIL_RE = re.compile(
    r"\bDropStale:\s*\d+\s*\(DupTs=(\d+)\s+OOO=(\d+)\)"
)
PERF_DROPPACE_RE = re.compile(r"\bDropPace:\s*(\d+)")
PERF_JITAVG_RE = re.compile(r"\bJitAvg:\s*(\d+)us")
PERF_THR_RE = re.compile(r"\bThrottle:\s*(\d+)")
PERF_DUP_RE = re.compile(r"\bDup:\s*(\d+)")


def extract(pattern, text, default=0):
    match = pattern.search(text)
    return int(match.group(1)) if match else default


def extract_pair(pattern, text):
    match = pattern.search(text)
    if not match:
        return (0, 0)
    return (int(match.group(1)), int(match.group(2)))


def safe_mean(values):
    return statistics.mean(values) if values else 0.0


def summarize_group(name, items):
    print(f"{name}:")
    print(f"  samples: {len(items)}")
    if not items:
        return
    print(
        f"  avg cadence TickDup: {safe_mean([item['tick_dup'] for item in items]):.2f}"
    )
    print(f"  avg perf Dup: {safe_mean([item['perf_dup'] for item in items]):.2f}")
    print(f"  avg delivered fps: {safe_mean([item['deliv'] for item in items]):.2f}")
    print(
        f"  avg min delivered 250 fps: {safe_mean([item['min_del_250'] for item in items]):.2f}"
    )
    print(
        f"  avg min delivered 500 fps: {safe_mean([item['min_del_500'] for item in items]):.2f}"
    )
    print(f"  avg input fps: {safe_mean([item['input'] for item in items]):.2f}")
    print(f"  avg stale drops: {safe_mean([item['drop_stale'] for item in items]):.2f}")
    print(
        f"  avg stale dup-ts: {safe_mean([item['drop_stale_dup_ts'] for item in items]):.2f}"
    )
    print(
        f"  avg stale ooo: {safe_mean([item['drop_stale_ooo'] for item in items]):.2f}"
    )
    print(
        f"  avg empty permille: {safe_mean([item['empty_pm'] for item in items]):.2f}"
    )
    print(
        f"  avg buffered permille: {safe_mean([item['buf_avg_pm'] for item in items]):.2f}"
    )
    print(f"  avg starved ticks: {safe_mean([item['starved'] for item in items]):.2f}")
    print(
        f"  avg hold-fresh ticks: {safe_mean([item['hold_fresh'] for item in items]):.2f}"
    )
    print(
        f"  avg reserve-spend ticks: {safe_mean([item['reserve_spend'] for item in items]):.2f}"
    )
    print(
        f"  avg catch-up ticks: {safe_mean([item['catch_up'] for item in items]):.2f}"
    )
    print(f"  avg shortfall: {safe_mean([item['shortfall'] for item in items]):.2f}")


def verify_sample(item):
    problems = []
    if item["deliv"] > item["input"] + item["drop_pace"] + item["drop_stale"] + 2:
        problems.append("delivered_exceeds_input")
    if item["min_del_250"] > item["min_in_250"] + 4:
        problems.append("min_del250_exceeds_min_in250")
    if item["min_del_500"] > item["min_in_500"] + 2:
        problems.append("min_del500_exceeds_min_in500")
    return problems


def sample_with_problems(item):
    return {
        **item,
        "problems": verify_sample(item),
    }


def main():
    parser = argparse.ArgumentParser(
        description="Analyze WGC cadence/perf correlations from media.log"
    )
    parser.add_argument("log", type=Path)
    args = parser.parse_args()

    lines = args.log.read_text(encoding="utf-8", errors="replace").splitlines()
    samples = []
    last_perf = None

    for lineno, line in enumerate(lines, start=1):
        if "[WGC Perf]" in line:
            min_in_250, min_in_500 = extract_pair(PERF_MININ_RE, line)
            min_del_250, min_del_500 = extract_pair(PERF_MINDEL_RE, line)
            drop_stale_dup_ts, drop_stale_ooo = extract_pair(
                PERF_DROPSTALE_DETAIL_RE, line
            )
            last_perf = {
                "line": lineno,
                "input": extract(PERF_INPUT_RE, line),
                "deliv": extract(PERF_DELIV_RE, line),
                "min_in_250": min_in_250,
                "min_in_500": min_in_500,
                "min_del_250": min_del_250,
                "min_del_500": min_del_500,
                "drop_stale": extract(PERF_DROPSTALE_RE, line),
                "drop_stale_dup_ts": drop_stale_dup_ts,
                "drop_stale_ooo": drop_stale_ooo,
                "drop_pace": extract(PERF_DROPPACE_RE, line),
                "jitter": extract(PERF_JITAVG_RE, line),
                "throttle": extract(PERF_THR_RE, line),
                "perf_dup": extract(PERF_DUP_RE, line),
            }
        elif "[Cadence Health]" in line and last_perf is not None:
            sample = dict(last_perf)
            sample.update(
                {
                    "cadence_line": lineno,
                    "tick_dup": extract(CADENCE_DUP_RE, line),
                    "cadence_thr": extract(CADENCE_THR_RE, line),
                    "shortfall": extract(CADENCE_SHORTFALL_RE, line),
                    "empty_pm": extract(CADENCE_EMPTY_RE, line),
                    "buf_avg_pm": extract(CADENCE_BUFAVG_RE, line),
                    "buf_min": extract(CADENCE_BUFMIN_RE, line),
                    "starved": extract(CADENCE_STARVED_RE, line),
                    "single": extract(CADENCE_SINGLE_RE, line),
                    "hold_fresh": extract(CADENCE_HOLD_FRESH_RE, line),
                    "reserve_spend": extract(CADENCE_SPEND_RE, line),
                    "catch_up": extract(CADENCE_CATCHUP_RE, line),
                }
            )
            samples.append(sample_with_problems(sample))

    print(f"samples: {len(samples)}")
    if not samples:
        return

    throttle_on = [
        sample
        for sample in samples
        if sample["cadence_thr"] > 0 or sample["throttle"] > 0
    ]
    throttle_off = [
        sample
        for sample in samples
        if sample["cadence_thr"] == 0 and sample["throttle"] == 0
    ]
    underfeed = [
        sample
        for sample in samples
        if sample["min_del_250"] < 120 or sample["min_in_250"] < 120
    ]
    healthy = [
        sample
        for sample in samples
        if sample["min_del_250"] >= 120 and sample["min_in_250"] >= 120
    ]
    worst = sorted(
        samples, key=lambda item: (item["tick_dup"], item["perf_dup"]), reverse=True
    )[:10]
    inconsistent = [sample for sample in samples if sample["problems"]]

    summarize_group("throttle_on", throttle_on)
    summarize_group("throttle_off", throttle_off)
    summarize_group("underfeed", underfeed)
    summarize_group("healthy", healthy)

    print("consistency_issues:")
    print(f"  samples: {len(inconsistent)}")
    for item in inconsistent[:10]:
        print(
            "  perf_line={perf} cadence_line={cad} problems={problems}".format(
                perf=item["line"],
                cad=item["cadence_line"],
                problems=",".join(item["problems"]),
            )
        )

    print("worst_windows:")
    for item in worst:
        print(
            "  perf_line={perf} cadence_line={cad} tick_dup={tick_dup} perf_dup={perf_dup} "
            "thr={thr} deliv={deliv} min_del250={min_del_250} min_in250={min_in_250} "
            "drop_stale={drop_stale} dup_ts={drop_stale_dup_ts} ooo={drop_stale_ooo} "
            "hold_fresh={hold_fresh} reserve_spend={reserve_spend} catch_up={catch_up} "
            "empty={empty_pm} buf_avg={buf_avg_pm} buf_min={buf_min} "
            "starved={starved} shortfall={shortfall}".format(
                perf=item["line"],
                cad=item["cadence_line"],
                tick_dup=item["tick_dup"],
                perf_dup=item["perf_dup"],
                thr=item["cadence_thr"],
                deliv=item["deliv"],
                min_del_250=item["min_del_250"],
                min_in_250=item["min_in_250"],
                drop_stale=item["drop_stale"],
                drop_stale_dup_ts=item["drop_stale_dup_ts"],
                drop_stale_ooo=item["drop_stale_ooo"],
                hold_fresh=item["hold_fresh"],
                reserve_spend=item["reserve_spend"],
                catch_up=item["catch_up"],
                empty_pm=item["empty_pm"],
                buf_avg_pm=item["buf_avg_pm"],
                buf_min=item["buf_min"],
                starved=item["starved"],
                shortfall=item["shortfall"],
            )
        )


if __name__ == "__main__":
    main()
