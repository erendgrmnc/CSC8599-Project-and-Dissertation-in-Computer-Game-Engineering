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

# The halo safety floor implemented in ServerWorldManager::MinimumSafeHaloWidth()
# (DistributedGameServer/ServerWorldManager.cpp:656-668):
#
#   w_min = HALO_ASSUMED_MAX_SPEED * lookahead * dt + 2 * HALO_ASSUMED_MAX_RADIUS
#
# The constants are hardcoded in the server, so they are MIRRORED here rather than
# read out of a run. If they change there they must change here too, or this report
# compares a measurement against a floor the run never actually used.
HALO_ASSUMED_MAX_SPEED = 60.0
HALO_ASSUMED_MAX_RADIUS = 2.0
DEFAULT_SUBSTEP_HZ = 120


def predicted_halo_floor(lookahead_ticks, substep_hz=DEFAULT_SUBSTEP_HZ):
    """The narrowest halo band that can still catch every border contact.

    Deliberately conservative: it uses an ASSUMED maximum speed of 60 units/s, not
    the speed the workload actually runs at, so the value it returns is an upper
    bound on what is needed rather than an estimate of it. An object faster than
    the assumed maximum falls outside the guarantee.
    """
    if substep_hz <= 0:
        raise ValueError("substep_hz must be positive")
    lag = max(0, lookahead_ticks) / float(substep_hz)
    return HALO_ASSUMED_MAX_SPEED * lag + 2.0 * HALO_ASSUMED_MAX_RADIUS


def find_knee(crossings_by_width):
    """The smallest swept width from which NO wider swept width shows a crossing.

    Not simply the first zero. An isolated zero followed by a non-zero at a larger
    width is noise, and reporting it would claim the soundness condition holds at a
    width where it demonstrably does not - the exact error this experiment exists
    to avoid. Returns None when no swept width achieves it.
    """
    knee = None
    for width in sorted(crossings_by_width, reverse=True):
        if crossings_by_width[width] != 0:
            break
        knee = width
    return knee


def print_knee_report(manifest, rows):
    """Border crossings against halo width, with the predicted floor overlaid.

    Only meaningful for a haloWidth sweep on the `headon` workload, where every
    object is launched at a partner across x = 0: a pair that collides bounces and
    never crosses, a pair whose contact was MISSED passes through and is handed off.
    So ho_sent counts missed border contacts on a fixed, interpretable scale.

    Returns a dict describing the point, or None if this is not a haloWidth sweep.
    """
    if manifest.get("sweep") != "haloWidth":
        return None

    lookahead = int(manifest.get("fixed", {}).get("haloLookahead", 4))
    floor = predicted_halo_floor(lookahead)

    # Run-level invariants are duplicated onto every server row, so dedupe by
    # (point, repeat) before taking a median - otherwise the sample is weighted by
    # server count rather than by repeat.
    per_repeat = {}
    for row in rows:
        per_repeat.setdefault((row["point"], row["repeat"]), row["ho_sent"])

    by_width = {}
    for (width, _repeat), crossings in per_repeat.items():
        by_width.setdefault(width, []).append(crossings)
    medians = {w: statistics.median(v) for w, v in by_width.items()}

    knee = find_knee(medians)
    sound = (knee is not None) and (knee <= floor + 1e-9)

    print()
    print(f"halo soundness: lookahead {lookahead} ticks, "
          f"predicted floor w_min = {floor:g}")
    print(f"{'width':>8} {'crossings (median)':>19} {'repeats':>8} {'vs floor':>10}")
    print("-" * 49)
    for width in sorted(medians):
        marker = "AT FLOOR" if abs(width - floor) < 1e-9 else (
            "below" if width < floor else "above")
        print(f"{width:>8g} {medians[width]:>19.0f} "
              f"{len(by_width[width]):>8} {marker:>10}")

    if knee is None:
        print("empirical knee : NONE - no swept width caught every border contact")
    else:
        verdict = "SOUND" if sound else "UNSOUND - the bound was optimistic"
        print(f"empirical knee : {knee:g}  (predicted {floor:g})  -> {verdict}")

    return {"lookahead": lookahead, "floor": floor, "knee": knee, "sound": sound}


def percentile(values, pct):
    if not values:
        return float("nan")
    ordered = sorted(values)
    k = (len(ordered) - 1) * pct / 100.0
    low = math.floor(k)
    high = min(low + 1, len(ordered) - 1)
    return ordered[low] + (ordered[high] - ordered[low]) * (k - low)


def ownership_anomalies(totals_in_tick_order, population_may_grow=False):
    """Invariant I1 per tick: (gap_ticks, double_ticks).

    The baseline is the RUNNING maximum, not the run's final population. Under a
    fixed population the two are the same from tick one, so this is unchanged for
    every workload that pre-seeds a grid. Under one that grows - injection spawns
    from an empty world - the final population is reached only at the end, so a
    final-population baseline marks every earlier tick as an ownership gap: 1,199
    of 1,200 ticks on a 10 s two-server run where nothing was lost.

    population_may_grow suppresses double-owner detection, because a rise is then
    ambiguous: two servers claiming one object and one server spawning a new one
    look identical in a per-tick owned total. Losses stay detectable either way,
    since nothing legitimately REMOVES an object unless it was destroyed - which
    is why a run with destroys needs conservation_delta read alongside this rather
    than this alone.

    With a fixed population a transient double raises the baseline for the rest of
    the run, so the tick after it also reads as a gap. That is the same weakness a
    final-population baseline has, and this at least reports the double: against
    [10, 11, 10] the old form reported two gaps and no double.
    """
    gap_ticks = 0
    double_ticks = 0
    running_max = None
    for total in totals_in_tick_order:
        if running_max is None or total > running_max:
            if running_max is not None and not population_may_grow:
                double_ticks += 1
            running_max = total
        elif total < running_max:
            gap_ticks += 1
    return gap_ticks, double_ticks


# AP aggregates over 5 s periods. Named because print_ap_frame_report labels its
# windows from it: a literal in one place and a default in the other would let the
# label drift from the width it describes.
AP_BUCKET_SECONDS = 5.0


def ap_frame_times(rows, bucket_seconds=AP_BUCKET_SECONDS):
    """Max frame time (ms) per elapsed-time bucket, AP's metric.

    AP aggregates "the maximum frame time of any server" over each 5 s period.
    This computes one server's per-bucket maximum; the caller takes the max
    across servers and then the mean across repeats.

    A frame is a loop iteration in which physics advanced (substeps > 0). Its
    frame time is the wall-clock gap since the previous such iteration, so the
    idle spin between them is charged to the frame that follows it - which is
    what AP's update period actually contains.
    """
    stamps = []
    for row in rows:
        raw = row.get("substeps")
        if raw is None:
            # Recorded before the column existed. Returning nothing is deliberate:
            # deriving frames from every loop iteration would report the loop's
            # spin rate as a frame rate.
            return {}
        try:
            if int(raw) <= 0:
                continue
            stamps.append(int(row["time_us"]))
        except (TypeError, ValueError):
            continue

    if len(stamps) < 2:
        return {}

    start = stamps[0]
    buckets = {}
    for previous, current in zip(stamps, stamps[1:]):
        frame_ms = (current - previous) / 1000.0
        bucket = int(((current - start) / 1e6) // bucket_seconds)
        if frame_ms > buckets.get(bucket, 0.0):
            buckets[bucket] = frame_ms
    return buckets


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


def custody_reproducibility_note(ho_resent, ho_reclaimed):
    """Custody firing means this run is NOT bit-reproducible, even under
    --run-ticks --fixed-step.

    The retry deadline (DistributedGameServer/ServerWorldManager.cpp,
    FlushPendingTransfers) is measured against real wall-clock time
    (NCL::MonotonicMicros), not the paced simulation tick counter, precisely because
    a tick count is not a valid proxy for elapsed real time (Task 6a). That fix
    makes custody correct, but it does not make custody's ACTIVATION deterministic:
    whether a resend or reclaim fires at all, and on which tick, depends on real
    elapsed time, machine load and network jitter - none of which replay identically
    between two runs of the same seed. A conservation delta of 0 on a run where
    custody fired does not mean the run reproduces bit-for-bit; it means custody's
    compensation worked THIS time.

    This is deliberately separate from check_custody() below: hoReclaimed > 0 is not
    an invariant FAILURE (the mechanism is working as designed), so it must not be
    reported as one - but it is not merely a counter either, and reporting only the
    number invites reading a clean end-of-run total as proof the run is reproducible
    when it is not.

    Returns None when neither counter fired, otherwise a human-readable message.
    """
    ho_resent = int(ho_resent)
    ho_reclaimed = int(ho_reclaimed)
    if not ho_resent and not ho_reclaimed:
        return None
    return (
        "custody fired (hoResent=%d, hoReclaimed=%d) - this run is NOT "
        "bit-reproducible, even under --run-ticks --fixed-step"
        % (ho_resent, ho_reclaimed)
    )


def check_custody(finals):
    """Custody invariant: reclaimed transfers must not lose objects.

    hoReclaimed > 0 is NOT a failure - it is the mechanism working. The failure is a
    non-zero conservation delta, which is checked separately. What is checked here is
    that nothing is left permanently in custody at the end of a run: a non-zero
    hoCustody means a transfer was still outstanding when the server exited, and that
    object is unaccounted for.
    """
    problems = []
    for row in finals:
        # read_final_lines regex-captures every field as a string, so hoCustody
        # arrives as e.g. "0" - truthy in Python regardless of its digits. Cast to
        # int before testing, or every run with the field present would "fail".
        stranded = int(row.get("hoCustody", 0))
        if stranded:
            problems.append(
                "server %s ended with %d transfer(s) still in custody"
                % (row.get("id", "?"), stranded))
    return problems


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
            # AP's frame-time series for this server. Kept out of summary.csv - it is
            # a dict per 5 s window, not a scalar - and popped before the row is
            # written. See ap_frame_times.
            "ap_frames": ap_frame_times(rows, AP_BUCKET_SECONDS),
        })

    # Only meaningful when the run was PACED. In realtime mode each server advances
    # its own tick counter as fast as it can, so server A's tick 500 and server B's
    # tick 500 are different simulated moments and summing across them compares
    # unrelated instants - which reads as thousands of ownership gaps that are not
    # there. The check below is therefore skipped, not weakened, for realtime runs.
    paced = True
    manifest_file = os.path.join(run_dir, "manifest.json")
    if os.path.exists(manifest_file):
        try:
            with open(manifest_file, encoding="utf-8-sig") as handle:
                paced = json.load(handle).get("mode") == "reproducible"
        except Exception:
            paced = True

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
    # Sorted by tick: the baseline below is a RUNNING maximum, so dict insertion
    # order (which follows the CSV rows, server by server) would make it nonsense.
    complete = [sum(per_tick[t]) for t in sorted(per_tick)
                if len(per_tick[t]) == server_count]
    ownership_gap_ticks = 0
    ownership_double_ticks = 0
    # Evaluated below, once objSpawned is known: whether the population may grow
    # decides whether a RISE is interpretable at all.

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

        # A workload that spawns at runtime grows its population, so a rising owned
        # total is expected rather than a double-owner. See ownership_anomalies.
        if complete and paced:
            ownership_gap_ticks, ownership_double_ticks = ownership_anomalies(
                complete, population_may_grow=(spawned > 0))
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
        # Not asserted here - see custody_reproducibility_note(). A non-zero value is
        # reported as an explicit REPRODUCIBILITY WARNING, not folded into
        # INVARIANT FAILURES: the mechanism firing is by design, not a bug.
        invariants["ho_resent"] = total(server_finals, "hoResent")
        invariants["ho_reclaimed"] = total(server_finals, "hoReclaimed")

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

    custody_problems = check_custody(server_finals)

    return servers, invariants, custody_problems


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2

    # More than one directory is how the halo soundness sweep is read: the knee
    # moving with the lookahead is a claim ACROSS experiments, not within one.
    exit_code = 0
    knees = []
    for index, experiment_dir in enumerate(sys.argv[1:]):
        if index > 0:
            print()
            print("=" * 72)
        result = analyse_experiment(experiment_dir)
        if result is None:
            exit_code = 1
            continue
        code, knee = result
        exit_code = exit_code or code
        if knee is not None:
            knees.append(knee)

    if len(knees) > 1:
        print_tracking_report(knees)
    return exit_code


def print_tracking_report(knees):
    """Does the knee move where the formula says it moves?

    One knee in the right place is a coincidence. Three knees that track the
    prediction across lookaheads is a validated condition - and a knee that stops
    tracking at long lookahead, while still never exceeding its floor, is the
    'halo lag is permanent, keep the lookahead small' argument, measured.
    """
    print()
    print("=" * 72)
    print("halo soundness across lookaheads")
    print(f"{'lookahead':>10} {'floor':>8} {'knee':>8} {'slack':>8} {'verdict':>10}")
    print("-" * 48)
    for entry in sorted(knees, key=lambda e: e["lookahead"]):
        knee = entry["knee"]
        knee_text = "none" if knee is None else f"{knee:g}"
        slack = "-" if knee is None else f"{entry['floor'] - knee:g}"
        verdict = "sound" if entry["sound"] else "UNSOUND"
        print(f"{entry['lookahead']:>10} {entry['floor']:>8g} {knee_text:>8} "
              f"{slack:>8} {verdict:>10}")


def print_ap_frame_report(frame_series, sweep_name="point"):
    """AP's headline: the maximum frame time of ANY server, per 5 s window.

    Max across servers rather than mean, because a distributed simulation is only
    as responsive as its slowest participant - averaging over servers hides the
    one that fell behind, which is the whole thing the number is for. Mean across
    repeats, because a window is already a maximum and taking a median of maxima
    would discard the spikes AP's metric exists to expose.

    A measurement, not a check: it is printed outside INVARIANT FAILURES and never
    affects the exit code.
    """
    per_point = {}
    for point, repeat, _server, buckets in frame_series:
        if not buckets:
            continue
        by_repeat = per_point.setdefault(point, {}).setdefault(repeat, {})
        for bucket, frame_ms in buckets.items():
            # Max ACROSS SERVERS within one repeat.
            if frame_ms > by_repeat.get(bucket, 0.0):
                by_repeat[bucket] = frame_ms

    if not per_point:
        return

    print()
    print("AP FRAME TIME (max across servers per 5s window, mean over repeats)")
    print(f"{sweep_name:>10} {'window_s':>10} {'mean_max_frame_ms':>19} {'repeats':>8}")
    print("-" * 51)
    for point in sorted(per_point):
        repeats = per_point[point]
        buckets = sorted({b for r in repeats.values() for b in r})
        for bucket in buckets:
            values = [r[bucket] for r in repeats.values() if bucket in r]
            window = (f"{bucket * AP_BUCKET_SECONDS:g}-"
                      f"{(bucket + 1) * AP_BUCKET_SECONDS:g}")
            print(f"{point:>10g} {window:>10} "
                  f"{statistics.mean(values):>19.3f} {len(values):>8}")

    # Stated unconditionally, because it is a property of the loop rather than of any
    # particular run, and a reader comparing these figures against AP's would
    # otherwise credit the sleep to the physics.
    print("  note: frame time is the loop's UPDATE PERIOD, so it includes the")
    print("        headless loop's inter-iteration sleep_for(1ms) - which Windows")
    print("        rounds up to the timer granularity, measured at ~8.3 ms here.")
    print("        Values near that floor are loop-paced, not physics-limited;")
    print("        compare physics p95 above for the simulation cost itself.")


def analyse_experiment(experiment_dir):
    """Returns (exit_code, knee_dict_or_None), or None if the directory is unusable."""
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
        return None

    rows = []
    failures = []
    reproducibility_warnings = []
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

        servers, invariants, custody_problems = summarise_run(run_dir)
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

        for problem in custody_problems:
            failures.append(f"{tag}: {problem}")

        # Explicit, not just a counter in summary.csv - see custody_reproducibility_note().
        note = custody_reproducibility_note(
            invariants.get("ho_resent", 0), invariants.get("ho_reclaimed", 0))
        if note is not None:
            reproducibility_warnings.append(f"{tag}: {note}")

    if not rows:
        print("No usable runs.")
        return None

    # Frame buckets are per-window dicts, not scalars, so they are lifted out before
    # summary.csv is written rather than flattened into it.
    frame_series = [
        (r["point"], r["repeat"], r["server"], r.pop("ap_frames"))
        for r in rows
    ]

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

    print_ap_frame_report(frame_series, sweep_name)

    print()
    if failures:
        print(f"INVARIANT FAILURES ({len(failures)}):")
        for failure in failures:
            print(f"  {failure}")
    else:
        print("All invariants hold on every run (conservation, handoff parity, "
              "command accounting, no failures, no resurrections).")

    # Separate from failures on purpose: custody firing is the mechanism working as
    # designed (check_custody / custody_reproducibility_note), not a bug. But a clean
    # invariant report must not read as "this run reproduces bit-for-bit" when it does
    # not, so this is always printed on its own, explicitly, whenever it applies.
    if reproducibility_warnings:
        print()
        print(f"REPRODUCIBILITY WARNING ({len(reproducibility_warnings)}):")
        for warning in reproducibility_warnings:
            print(f"  {warning}")

    knee = print_knee_report(manifest, rows)

    print(f"\nsummary -> {out_path}")
    return (1 if failures else 0), knee


if __name__ == "__main__":
    sys.exit(main())
