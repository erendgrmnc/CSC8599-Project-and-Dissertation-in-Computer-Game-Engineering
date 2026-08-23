"""Phase A no-op gate.

Exact @@FINAL comparison is impossible: four clean runs at the same commit, seed
and configuration disagree on contacts, snapshot counts, halo object counts and the
handoff split. What IS stable is a specific set of counters, and object conservation.

Usage: gate-compare.py <pre-change experiment dir> ... -- <post-change experiment dir> ...
       (with no --, the LAST directory is the post-change one and the rest are pre.)

Exit code 0 = gate passes.
"""
import glob
import os
import re
import sys

# Identical on every clean run measured. A Phase A regression would almost certainly
# move one of these off its value - they are the failure counters plus the two
# tick-locked halo counts.
#
# haloLate was in this list originally (it read 0 on all 8 pre-change server-runs
# sampled for the Task 6 baseline) and was moved OUT after a counterfactual on the
# pre-change commit (d8aec0b) found a degraded run producing haloLate 526/66 with
# zero code change - an order of magnitude above anything seen post-change. It is a
# continuous measure of inter-server timing drift, not a threshold symptom like
# custody firing or an ownership gap in the thousands, so a mildly loaded run can
# clear the degraded-run thresholds while still posting a handful of late halo
# updates. It is range-checked in Step 4 instead of pinned here. See
# docs/superpowers/results/2026-08-23-A-instrumentation.md for the measurement.
STABLE = [
    "cmdApplied", "cmdRelayed", "cmdDup", "cmdRejected", "cmdFanout",
    "hoFail", "hoLate", "hoResent", "hoReclaimed", "hoCustody", "hoDup",
    "hoPending", "hoSched", "haloAhead", "haloSent", "haloRecv",
    "manifestSent", "objPreseed", "objSpawned", "objDestroyed",
]

# Per-server assignment drifts, but the world is conserved.
CONSERVED = ["objs", "objPool"]


def finals(experiment_dir):
    """One dict per server per run under this experiment directory."""
    out = []
    for log in sorted(glob.glob(os.path.join(experiment_dir, "*", "mid.log"))):
        for line in open(log, errors="ignore"):
            if "@@FINAL role=server" not in line:
                continue
            out.append(dict(
                re.findall(r"(\w+)=(-?\d+)", line[line.index("@@FINAL"):])
            ))
    return out


def main():
    args = sys.argv[1:]
    if "--" in args:
        cut = args.index("--")
        pre_dirs, post_dirs = args[:cut], args[cut + 1:]
    else:
        pre_dirs, post_dirs = args[:-1], args[-1:]
    if not pre_dirs or not post_dirs:
        print(__doc__)
        return 2

    pre = [f for d in pre_dirs for f in finals(d)]
    post = [f for d in post_dirs for f in finals(d)]
    if not pre or not post:
        print("FAIL: no @@FINAL server lines found on one side")
        return 1

    failures = []

    # 1. The stable set must hold its exact value on every server of every run.
    for field in STABLE:
        pre_values = {int(f[field]) for f in pre if field in f}
        post_values = {int(f[field]) for f in post if field in f}
        if not pre_values or not post_values:
            failures.append(f"{field}: absent on one side")
        elif pre_values != post_values:
            failures.append(
                f"{field}: was {sorted(pre_values)}, now {sorted(post_values)}"
            )

    # 2. Conservation: per-server assignment drifts, the world total does not.
    #    Grouped per run, because a total is only meaningful within one run.
    def totals(rows, dirs):
        per_run = []
        for d in dirs:
            rows_here = finals(d)
            by_run = {}
            for log_index in range(0, len(rows_here), 2):
                pair = rows_here[log_index:log_index + 2]
                if len(pair) == 2:
                    by_run.setdefault(log_index, pair)
            for pair in by_run.values():
                per_run.append({f: sum(int(r[f]) for r in pair) for f in CONSERVED})
        return per_run

    for field in CONSERVED:
        pre_totals = {t[field] for t in totals(pre, pre_dirs)}
        post_totals = {t[field] for t in totals(post, post_dirs)}
        if pre_totals != post_totals:
            failures.append(
                f"{field} total: was {sorted(pre_totals)}, now {sorted(post_totals)}"
            )

    if failures:
        print(f"GATE FAILED ({len(failures)}):")
        for failure in failures:
            print(f"  {failure}")
        return 1

    print(f"GATE PASSED: {len(STABLE)} stable fields unchanged, "
          f"{len(CONSERVED)} conserved totals unchanged "
          f"({len(pre)} pre-change server-runs vs {len(post)} post-change)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
