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

# Identical on every clean run measured. A regression would almost certainly move one
# of these off its value - they are the failure counters plus the halo VOLUME counts.
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
#
# haloAhead was moved OUT on 2026-08-26, for the same reason and with the same
# evidence, because Phase A fixed only one face of a two-faced problem: haloAhead and
# haloLate are the SAME clock skew seen from the two ends of a link. Reclassifying one
# and pinning the other at 0 could not hold. Measured on Phase D's Gate A: two runs of
# one binary at one configuration, 4 repeats each, spiked haloAhead in a DIFFERENT
# repeat each time - r3 at 1086, then r1 at 161 - each paired with a haloLate spike on
# the other server (1989/250, then 328/43). hoResent was 0 on every server of every
# repeat, so the change under test could not execute at all. Intermittent, load-driven,
# and nothing to do with the code. See
# docs/superpowers/results/2026-08-26-D-ownership.md.
#
# HAZARD, 4 servers (measured 2026-08-26): `haloSent` and `haloRecv` are NOT stable
# there. Two experiments taken BEFORE the change under test, differing only by a
# provably halo-neutral edit, already disagree - [3745, 3797, 3841, 3867, 4258, 4415,
# 4462] against [3797, 3841, 4258, 4413, 4415]. This list was derived from Phase A's
# 2-server data and validated only there. If this gate is pointed at a 4-server
# experiment, drop haloSent and haloRecv and range-check them, exactly as haloLate and
# haloAhead are range-checked here.
STABLE = [
    "cmdApplied", "cmdRelayed", "cmdDup", "cmdRejected", "cmdFanout",
    "hoFail", "hoLate", "hoResent", "hoReclaimed", "hoCustody", "hoDup",
    "hoPending", "hoSched", "haloSent", "haloRecv",
    "manifestSent", "objPreseed", "objSpawned", "objDestroyed",
]

# Per-server assignment drifts, but the world is conserved.
#
# NOTE on `objs` (added 2026-08-24): unlike objPool it is NOT a count of what a
# server holds. It is Profiler::GetObjectsOnBorders(), written by
# ServerWorldManager::Update as the number of mTestObjects with physics, and the
# drain phase runs the loop WITHOUT stepping the world - so on any run that ends
# with transfers in flight it freezes at the last stepped tick while the drain
# keeps installing arrivals. It is kept here because this gate compares two runs
# of the SAME configuration against each other rather than against an expected
# world total, and on the gate's own healthy-path configuration nothing is
# outstanding at exit, so it is stable. Do not reuse it as a conservation
# quantity: analyse.py counts objPool + hoCustody for exactly this reason (see
# backlog item 12). If this gate is ever pointed at a rebalancing or
# high-lookahead configuration, drop `objs` first or it will fail on drain timing.
CONSERVED = ["objs", "objPool"]


class GateError(Exception):
    """A structural problem with the data itself, distinct from a gate FAILURE
    (which means the data is fine but the values disagree). Raised loudly rather
    than letting mismatched data silently produce a wrong or spuriously-passing
    total.
    """


def _natural_key(path):
    """Numeric-aware sort key so 'ticks1800-r10' sorts after 'ticks1800-r2', not
    before it (plain lexicographic sort puts '10' before '2'). Not triggered at
    the repeat counts used so far, but an experiment is not bounded to those.
    """
    return [int(chunk) if chunk.isdigit() else chunk
            for chunk in re.split(r"(\d+)", path)]


def finals_by_log(experiment_dir):
    """List of (log_path, [row dict, ...]) - one entry per mid.log under this
    experiment directory, each holding that log's own @@FINAL role=server rows.
    Keeping the per-log grouping (rather than flattening across logs) is what
    lets the conservation check pair rows from the SAME run instead of drifting
    across a run boundary if some log contributed an unexpected number of lines.
    """
    groups = []
    for log in sorted(glob.glob(os.path.join(experiment_dir, "*", "mid.log")),
                       key=_natural_key):
        rows = []
        for line in open(log, errors="ignore"):
            if "@@FINAL role=server" not in line:
                continue
            rows.append(dict(
                re.findall(r"(\w+)=(-?\d+)", line[line.index("@@FINAL"):])
            ))
        groups.append((log, rows))
    return groups


def finals(experiment_dir):
    """One dict per server per run under this experiment directory, flattened
    across runs. Safe for the STABLE-field checks below, which only ever build a
    set of values and do not care which run a value came from - the per-run
    boundary only matters for the conservation check, which uses
    finals_by_log directly instead of this.
    """
    return [row for _log, rows in finals_by_log(experiment_dir) for row in rows]


def conservation_totals(dirs):
    """Per-run sums of the CONSERVED fields, one dict per mid.log (= per run).

    The expected server count is DERIVED, not hardcoded: it is the number of
    distinct server ids seen anywhere across every log passed in (the union of
    every 'id=' value), which is correct whether this is a 1-server or a
    4-server experiment - no assumption baked in.

    Every individual log is then required to report exactly that many rows,
    each with a distinct id. A log that falls short (a server that died before
    printing @@FINAL - a real failure mode this project has hit) or reports a
    duplicate id (a stray matching line) raises GateError naming the log path
    and what was found, instead of letting the pairing silently drift and sum
    rows from different runs together.
    """
    groups = [group for d in dirs for group in finals_by_log(d)]

    all_ids = {row["id"] for _log, rows in groups for row in rows if "id" in row}
    expected = len(all_ids)
    if expected == 0:
        raise GateError(
            f"no @@FINAL role=server lines found in any of: {', '.join(dirs)}"
        )

    per_run = []
    for log, rows in groups:
        ids = [row.get("id") for row in rows]
        if len(rows) != expected or len(set(ids)) != len(rows):
            raise GateError(
                f"{log}: expected {expected} @@FINAL server line(s) with distinct "
                f"ids (server ids seen across this experiment: {sorted(all_ids)}), "
                f"found {len(rows)} line(s) with id(s) {ids} - refusing to pair, "
                f"since doing so would silently sum rows across different runs"
            )
        per_run.append({f: sum(int(r[f]) for r in rows) for f in CONSERVED})
    return per_run


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
    try:
        pre_run_totals = conservation_totals(pre_dirs)
        post_run_totals = conservation_totals(post_dirs)
    except GateError as error:
        print(f"GATE FAILED: {error}")
        return 1

    for field in CONSERVED:
        pre_totals = {t[field] for t in pre_run_totals}
        post_totals = {t[field] for t in post_run_totals}
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
