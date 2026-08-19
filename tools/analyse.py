"""Analysis for experiments produced by tools/run-experiments.ps1.

Reads the per-tick CSVs and the @@FINAL lines, checks the invariants, and emits a
tidy summary CSV plus a printed table.

Deliberately depends only on the standard library. pandas/matplotlib would be
nicer, but this has to run on the same machine that produces the data with no
environment setup, and the dissertation's numbers should not be gated on a pip
install.

Two things this does that a naive mean() would get wrong:

  * It reports percentiles, not just means. Tick cost is strongly bimodal in
    realtime mode (mean 22x the median), so a mean alone is misleading.
  * It never pools ticks across servers. Servers carry different loads and can tick
    at different rates, so pooling silently weights whichever server ticked more.
    Per-server statistics are reported, and any cross-server figure is a sum, never
    an average of averages.

Usage:
    python tools/analyse.py runs/exp-scaling
"""

import csv
import glob
import json
import math
import os
import re
import statistics
import sys

# Discard the opening ticks of every run: the world is still settling from its
# initial contacts and the numbers there describe construction, not the workload.
WARMUP_TICKS = 200


def percentile(values, pct):
    if not values:
        return float("nan")
    ordered = sorted(values)
    k = (len(ordered) - 1) * pct / 100.0
    low = math.floor(k)
    high = min(low + 1, len(ordered) - 1)
    return ordered[low] + (ordered[high] - ordered[low]) * (k - low)


def read_final_lines(run_dir):
    """@@FINAL totals are exact end-of-run values; @@STAT is a 2 Hz sample and is
    never used for a reported number."""
    finals = []
    for name in ("mid.log", "cli.log"):
        path = os.path.join(run_dir, name)
        if not os.path.exists(path):
            continue
        with open(path, errors="ignore") as handle:
            for line in handle:
                if "@@FINAL" not in line:
                    continue
                fields = dict(
                    re.findall(r"(\w+)=(-?\d+)", line[line.index("@@FINAL"):])
                )
                fields["role"] = "client" if "role=client" in line else "server"
                finals.append(fields)
    return finals


def summarise_run(run_dir):
    """One row per server, plus the run-level invariant checks."""
    servers = []
    for path in sorted(glob.glob(os.path.join(run_dir, "ticks-server*.csv"))):
        with open(path) as handle:
            rows = list(csv.DictReader(handle))[WARMUP_TICKS:]
        if not rows:
            continue

        physics = [float(r["physics_ms"]) for r in rows]
        servers.append({
            "server": int(re.search(r"ticks-server(\d+)", path).group(1)),
            "ticks": len(rows),
            "phys_mean": statistics.mean(physics),
            "phys_p50": percentile(physics, 50),
            "phys_p95": percentile(physics, 95),
            "phys_p99": percentile(physics, 99),
            "phys_max": max(physics),
            "owned_final": int(rows[-1]["owned_objects"]),
            # integrated should track owned. If it tracks the world total instead,
            # the integrator is simulating objects outside this server's region and
            # the scaling curve is meaningless.
            "integ_mismatch": sum(
                1 for r in rows if r["owned_objects"] != r["integrated_objects"]
            ),
        })

    # Invariant I1, checked CONTINUOUSLY rather than only at the end.
    #
    # @@FINAL totals say what each server held when it stopped, which is silent about
    # everything in between. Handoff used to release an object the moment the packet
    # was sent while the receiver installed it lookahead ticks later, so a transferred
    # object was owned by NOBODY for that whole window - 78% of ticks on a 200-object
    # uniform run - and every end-of-run total still balanced perfectly.
    #
    # Summing owned_objects across servers per tick catches both directions: a dip
    # below the expected total is an ownership gap, a rise above it is two servers
    # simulating the same object.
    per_tick = {}
    for path in sorted(glob.glob(os.path.join(run_dir, "ticks-server*.csv"))):
        with open(path) as handle:
            for row in csv.DictReader(handle):
                tick = int(row["tick"])
                per_tick.setdefault(tick, []).append(int(row["owned_objects"]))

    server_count = len(glob.glob(os.path.join(run_dir, "ticks-server*.csv")))
    # Only ticks every server reported: a tick one server has not reached yet would
    # read as a gap.
    complete = [sum(v) for v in per_tick.values() if len(v) == server_count]
    ownership_gap_ticks = 0
    ownership_double_ticks = 0
    if complete:
        expected = max(complete)
        ownership_gap_ticks = sum(1 for v in complete if v < expected)
        ownership_double_ticks = sum(1 for v in complete if v > expected)

    finals = read_final_lines(run_dir)
    server_finals = [f for f in finals if f["role"] == "server"]
    client_finals = [f for f in finals if f["role"] == "client"]

    def total(fields, key):
        return sum(int(f.get(key, 0)) for f in fields)

    invariants = {}
    if server_finals:
        spawned = total(server_finals, "objSpawned")
        destroyed = total(server_finals, "objDestroyed")
        owned = total(server_finals, "objs")
        # Every server builds the identical pre-seeded set independently, so this is
        # ONE server's value - summing it would multiply the world by the server
        # count.
        preseed = max((int(f.get("objPreseed", 0)) for f in server_finals), default=0)
        invariants["conservation_delta"] = owned - (preseed + spawned - destroyed)
        invariants["ho_sent"] = total(server_finals, "hoSent")
        invariants["ho_recv"] = total(server_finals, "hoRecv")
        # Minus the transfers still in flight. Ownership changes on an agreed tick
        # rather than on send, so hoSent counts the START of a transfer and hoRecv
        # its COMPLETION: a run that ends mid-transfer is legitimately short by the
        # number still pending. The object is not lost - the sender still owns it,
        # which is what conservation and the per-tick ownership check confirm.
        invariants["ho_pending"] = total(server_finals, "hoPending")
        # Arrived but not yet installed, the receiver-side mirror of ho_pending.
        invariants["ho_scheduled"] = total(server_finals, "hoSched")
        invariants["ho_parity_delta"] = (invariants["ho_sent"] - invariants["ho_recv"]
                                         - invariants["ho_pending"]
                                         - invariants["ho_scheduled"])
        invariants["ho_fail"] = total(server_finals, "hoFail")
        invariants["ho_late"] = total(server_finals, "hoLate")

        applied = total(server_finals, "cmdApplied")
        rejected = total(server_finals, "cmdRejected")
        duplicate = total(server_finals, "cmdDup")
        fanout = total(server_finals, "cmdFanout")
        sent = total(client_finals, "cmdSent")
        # An area effect legitimately applies once per overlapped region, so the
        # fan-out hops have to come back out of the applied total.
        invariants["cmd_sent"] = sent
        invariants["cmd_delta"] = sent - (applied + rejected + duplicate - fanout)
        invariants["resurrections"] = total(client_finals, "resurrectAttempts")

        # Reported, not asserted. These are the quantities the increments are
        # ABOUT - how much snapshot traffic interest removed, how much work each
        # server did - so a summary without them cannot answer the question the
        # experiment was run to answer.
        invariants["snap_sent"] = total(server_finals, "snapSent")
        invariants["snap_suppressed"] = total(server_finals, "snapSupp")
        invariants["contacts"] = total(server_finals, "contacts")
        # Non-zero means a server could not hold pace with its peers, so halo
        # updates missed their tick. Not a failure on its own - it is the
        # symptom a balanced partition is supposed to remove - but a run with
        # any is not bit-reproducible.
        invariants["halo_late"] = total(server_finals, "haloLate")

    invariants["ownership_gap_ticks"] = ownership_gap_ticks
    invariants["ownership_double_ticks"] = ownership_double_ticks

    return servers, invariants


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2

    experiment_dir = sys.argv[1]
    manifest_path = os.path.join(experiment_dir, "experiment.json")
    manifest = {}
    if os.path.exists(manifest_path):
        with open(manifest_path, encoding="utf-8-sig") as handle:
            manifest = json.load(handle)
        print(f"experiment : {manifest.get('name')}")
        print(f"sweep      : {manifest.get('sweep')} over {manifest.get('values')}")
        print(f"commit     : {manifest.get('gitCommit', '?')[:12]}"
              f"{'  (DIRTY - not reproducible)' if manifest.get('gitDirty') else ''}")
        print()

    run_dirs = sorted(
        d for d in glob.glob(os.path.join(experiment_dir, "*"))
        if os.path.isdir(d)
    )
    if not run_dirs:
        print(f"No runs found under {experiment_dir}")
        return 1

    rows = []
    failures = []
    for run_dir in run_dirs:
        tag = os.path.basename(run_dir)
        # Sweep names are camelCase now (interestRadius, physicsThreads), and values
        # can be fractional or negative - a halo width or an interest radius is a
        # distance. run-experiments encodes '.' as 'p' and '-' as 'm', because a dot in
        # a directory name is awkward to glob and a leading dash reads as a flag.
        match = re.match(r"([A-Za-z]+)([0-9pm]+)-r(\d+)$", tag)
        if not match:
            continue
        point = float(match.group(2).replace("p", ".").replace("m", "-"))
        repeat = int(match.group(3))

        servers, invariants = summarise_run(run_dir)
        if not servers:
            failures.append(f"{tag}: no per-tick metrics (run failed)")
            continue

        for server in servers:
            rows.append({"point": point, "repeat": repeat, **server, **invariants})

        for name, value in invariants.items():
            if name.endswith("_delta") and value != 0:
                failures.append(f"{tag}: {name} = {value} (expected 0)")
            if name in ("ho_fail", "resurrections",
                        "ownership_gap_ticks", "ownership_double_ticks") and value != 0:
                failures.append(f"{tag}: {name} = {value} (expected 0)")

    if not rows:
        print("No usable runs.")
        return 1

    out_path = os.path.join(experiment_dir, "summary.csv")
    with open(out_path, "w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=list(rows[0].keys()))
        writer.writeheader()
        writer.writerows(rows)

    sweep_name = manifest.get("sweep", "point")
    print(f"{sweep_name:>8} {'srv':>4} {'n':>4} {'p50 ms':>9} {'p95 ms':>9} "
          f"{'p99 ms':>9} {'owned':>7} {'mismatch':>9}")
    print("-" * 68)
    for point in sorted({r["point"] for r in rows}):
        for server_id in sorted({r["server"] for r in rows if r["point"] == point}):
            group = [r for r in rows if r["point"] == point and r["server"] == server_id]
            # Median across repeats, not mean: with a handful of repeats one slow
            # run should not drag the reported figure.
            point_label = f"{point:g}"
            print(f"{point_label:>8} {server_id:>4} {len(group):>4} "
                  f"{statistics.median(r['phys_p50'] for r in group):>9.4f} "
                  f"{statistics.median(r['phys_p95'] for r in group):>9.4f} "
                  f"{statistics.median(r['phys_p99'] for r in group):>9.4f} "
                  f"{statistics.median(r['owned_final'] for r in group):>7.0f} "
                  f"{sum(r['integ_mismatch'] for r in group):>9}")

    # Busiest server per point. The headline for anything about load: an average over
    # servers hides exactly the imbalance the partitioning exists to fix, and the
    # simulation runs no faster than its slowest participant.
    print()
    print(f"{'point':>8} {'busiest p95 ms':>15} {'lightest p95 ms':>16} {'ratio':>7} {'owned max':>10} {'owned min':>10}")
    print("-" * 72)
    for point in sorted({r["point"] for r in rows}):
        at_point = [r for r in rows if r["point"] == point]
        by_server = {}
        for r in at_point:
            by_server.setdefault(r["server"], []).append(r)
        medians = {
            sid: statistics.median(x["phys_p95"] for x in group)
            for sid, group in by_server.items()
        }
        owned = {
            sid: statistics.median(x["owned_final"] for x in group)
            for sid, group in by_server.items()
        }
        busiest = max(medians.values())
        lightest = min(medians.values())
        ratio = (busiest / lightest) if lightest > 0 else float("inf")
        print(f"{point:>8g} {busiest:>15.4f} {lightest:>16.4f} {ratio:>7.2f} "
              f"{max(owned.values()):>10.0f} {min(owned.values()):>10.0f}")

    print()
    if failures:
        print(f"INVARIANT FAILURES ({len(failures)}):")
        for failure in failures:
            print(f"  {failure}")
    else:
        print("All invariants hold on every run (conservation, handoff parity, "
              "command accounting, no failures, no resurrections).")

    print(f"\nsummary -> {out_path}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
