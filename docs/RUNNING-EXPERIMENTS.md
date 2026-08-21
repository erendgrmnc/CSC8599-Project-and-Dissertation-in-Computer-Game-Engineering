# Running experiments

How to go from source code to a number you can defend.

If you only want to *watch* the system run, use the launcher instead — see
[Tools §2](TOOLS.md#2-distributedlauncher--watch-it-run). This page is about **measurement**.

---

## The short version

```powershell
# 1. Build and stage. Do this after EVERY code change.
powershell -ExecutionPolicy Bypass -File tools\build-deploy.ps1

# 2. Run the experiment.
powershell -ExecutionPolicy Bypass -File tools\run-experiments.ps1 `
    -Name scaling -Sweep servers -Values 1,2,4 -Repeats 3 `
    -Objects 400 -Workload uniform

# 3. Read the results. Non-zero exit means a correctness check failed.
python tools\analyse.py runs\exp-scaling
```

---

## Step 1 — Make sure you are measuring what you think you are

Three things go wrong here more often than anything else.

**Your code changes are not in the run.** The system executes the copies in `deploy/`. If you
built in Visual Studio but did not re-run `build-deploy.ps1`, you just measured the old binary.
There is no warning. Re-run it every time.

**The working tree is dirty.** `run-experiments.ps1` records the current git commit in
`experiment.json`, and prints a yellow warning if you have uncommitted changes. A dataset from a
dirty tree cannot be reproduced from the recorded commit, so it cannot be published. Commit
first.

**Something is still running from last time.** A leftover server process holds onto its port and
its files, and the next run behaves strangely. If a run fails oddly, check for stray processes.

---

## Step 2 — Choose how to bound the run

This is a methodological choice, not a convenience.

### Fixed work: `-Ticks N`

Every server takes exactly N physics steps, each advancing simulated time by the same fixed
amount, paced against a shared clock so all servers stay together.

- Same settings give the same result.
- Object counts and final positions match exactly between runs.
- **Use for:** conservation, ownership, handover counts, anything you want to be exactly
  repeatable.

7200 ticks is 60 seconds of simulated time.

### Fixed time: `-Seconds N` (with `-Ticks 0`)

Servers run flat out for N seconds of real time.

- Shows genuine responsiveness under load.
- **Not repeatable.** The number of steps completed varies with machine load.
- **Use for:** performance claims — always with repeats.

> A run bounded by time will complete a different number of steps every time. Comparing exact
> object counts between two such runs is meaningless.

---

## Step 3 — Pick a workload

See [Workloads](WORKLOADS.md) for the full guide. In short: `uniform` unless you have a specific
reason otherwise.

The workload has to match the question. Measuring load balancing on `uniform` shows nothing,
because `uniform` is already balanced. Measuring cross-boundary collision on a single server
shows nothing, because nothing crosses a boundary.

---

## Step 4 — Repeat

Even with a fixed seed, per-server object counts and handover counts vary by small amounts
between runs. That variation is real, and a single run cannot show you it exists.

- **3 repeats** is the minimum for anything you quote.
- **5 repeats** for a headline figure.
- `analyse.py` reports the **median** across repeats, so outliers do not drag the number.

---

## Step 5 — Check correctness before you look at the timings

`analyse.py` exits non-zero if any correctness check failed. **A fast run that fails an invariant
is not a result.**

The checks, in plain terms:

| Check | What it means if it fails |
|---|---|
| `conservation_delta` | Objects vanished, or appeared from nowhere. |
| `ho_parity_delta` | Handovers sent do not match handovers received. |
| `ownership_gap_ticks` | An object was owned by nobody at some point. |
| `ownership_double_ticks` | Two servers both simulated the same object. |
| `resurrections` | A destroyed object came back. |
| fewer result files than servers | A server crashed or never started. |

A **reproducibility warning** is different from a failure. It means a recovery mechanism fired —
working as designed — but the run will not repeat bit-for-bit. Do not claim reproducibility for
a run that prints one.

---

## Step 6 — Read the timings properly

**Use percentiles, not averages.** Step cost is uneven; a mean hides spikes. The report gives
p50 (typical), p95 and p99 (the slow ones).

**Look at the busiest server, not the average.** A distributed simulation runs no faster than
its slowest participant. Averaging across servers hides exactly the imbalance the partitioning
exists to fix.

**Do not pool steps across servers.** Servers under different loads take different numbers of
steps, so pooling silently weights whichever one ran more.

**Never quote the live `@@STAT` output.** Those lines are a snapshot taken twice a second, not an
average — they can land anywhere in the distribution by luck. Evaluation numbers come from the
CSV files.

---

## Understanding one important limitation

Every measurement in this repository puts **every program on one machine**. That has a
consequence you must state whenever you compare server counts.

Splitting the per-step cost in two makes it clear:

| servers | step time | physics | coordination | objects/server |
|---|---|---|---|---|
| 1 | 14.01 ms | 9.87 ms | 3.94 ms | 4,799 |
| 2 | 9.23 ms | 4.39 ms | 4.72 ms | 2,400 |
| 4 | 9.05 ms | 2.34 ms | 6.57 ms | 1,200 |

**The physics scales almost perfectly** — halving the objects per server roughly halves the
simulation cost. That is the claim the design exists to support, and it holds.

**Coordination does not scale.** Going from 2 servers to 4 saves 2.05 ms of physics and spends
1.85 ms more coordinating, so the total barely moves.

But on a single machine, four servers plus the manager, midware and client are **seven processes
on six cores**. Some of that coordination figure is genuine protocol cost and some is simply
processes fighting for the same CPU, and this setup cannot separate them. So:

> Do not quote the flat 2→4 segment as a scaling limit of the design. It is a measurement of this
> machine. The 1→2 segment and the physics column are the safe figures.

Separating the two properly needs one server per machine. Nothing in the code has to change for
that — only the run configuration. See [Deploy](DEPLOY.md).

---

## Worked example: does adding servers help?

```powershell
# Build first.
powershell -ExecutionPolicy Bypass -File tools\build-deploy.ps1

# Performance question, so bound by time and repeat.
powershell -ExecutionPolicy Bypass -File tools\run-experiments.ps1 `
    -Name scaling -Sweep servers -Values 1,2,4 -Repeats 5 `
    -Objects 4000 -Ticks 0 -Seconds 30 -Workload uniform

python tools\analyse.py runs\exp-scaling
```

Read the **busiest server p95** at each server count. If it falls as servers increase, the split
is helping. Then check the physics column separately from the total, for the reason above.

---

## Worked example: does the split lose objects?

```powershell
# Correctness question, so bound by work for exact repeatability.
powershell -ExecutionPolicy Bypass -File tools\run-experiments.ps1 `
    -Name conservation -Sweep servers -Values 2,4 -Repeats 3 `
    -Objects 400 -Ticks 7200 -Workload seam -DrainSeconds 10

python tools\analyse.py runs\exp-conservation
echo $LASTEXITCODE     # 0 means every check passed
```

`seam` puts objects exactly on the boundary lines, which is the hardest case for the ownership
rule. `-DrainSeconds 10` lets handovers still in flight finish, so they are not counted as lost
objects when the run stops.

---

## Where results go

```
runs/exp-NAME/
├── experiment.json          what was swept, the git commit, the machine
├── servers1-r1/             one folder per run
│   ├── ticks-server0.csv    one row per physics step
│   ├── mid.log              server output, including @@FINAL totals
│   └── manifest.json        this run's exact settings
└── summary.csv              written by analyse.py
```

`runs/` is not committed to git. If a dataset matters, record the experiment folder somewhere
durable along with the commit hash from `experiment.json`.
