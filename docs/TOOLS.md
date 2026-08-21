# Tools

Everything in `tools/`, what it is for, and how to run it properly.

Read the sections in order the first time. Each tool builds on the one before it.

| Tool | Use it when you want to… |
|---|---|
| [`build-deploy.ps1`](#1-build-deployps1--build-everything) | Build all four programs and stage them ready to run. |
| [`DistributedLauncher`](#2-distributedlauncher--watch-it-run) | Watch the system running, with a live dashboard. |
| [`measure.ps1`](#3-measureps1--one-measured-run) | Take **one** measurement and write numbers to disk. |
| [`run-experiments.ps1`](#4-run-experimentsps1--a-sweep-with-repeats) | Take **many** measurements while varying one setting. |
| [`analyse.py`](#5-analysepy--read-the-results) | Turn those numbers into a report, and check correctness. |

---

## 1. `build-deploy.ps1` — build everything

**What it does.** Builds all four programs and copies them into `deploy/`, each in its own
folder. This matters because the Midware launches game servers using a path *relative to its own
folder*, so the layout has to be right.

```powershell
powershell -ExecutionPolicy Bypass -File tools\build-deploy.ps1
```

Afterwards you have:

```
deploy/Manager/EntryPoint.exe
deploy/Midware/EntryPoint.exe
deploy/Client/EntryPoint.exe
deploy/DistributedPhysicsServer/EntryPoint.exe
```

**Why use the script rather than building by hand.** The Client is built with a different setting
than the three server programs, and the script flips that setting and puts it back afterwards.
Doing it by hand is easy to get wrong and the failure is silent — you get four binaries that look
fine and behave strangely. See [Building](BUILDING.md).

> **If the Midware connects but no game servers appear**, this is almost always the deploy layout.
> The Midware looks for `./DistributedPhysicsServer/EntryPoint.exe` next to itself.

---

## 2. `DistributedLauncher` — watch it run

**What it does.** Starts all four programs for you and shows a live dashboard: one row per
server, plus a log tab for each program.

```powershell
dotnet run --project tools\DistributedLauncher\DistributedLauncher.csproj
```

Use this when you want to **see** the system working — demos, sanity checks, or debugging a
bring-up problem. It is not how you take measurements.

**Headless tickbox.** Leave it ticked and everything runs in one window. Untick it and each
server opens its own profiler window — useful for screenshots, slower to run.

---

## 3. `measure.ps1` — one measured run

**What it does.** Starts a manager, a midware, the game servers and a client; runs them for a
fixed amount of work; and writes the results into a folder under `runs/`.

```powershell
powershell -ExecutionPolicy Bypass -File tools\measure.ps1 `
    -Servers 2 -Objects 400 -Ticks 7200 -Workload uniform -Tag my-run
```

Output lands in `runs\my-run\`:

| File | Contents |
|---|---|
| `ticks-server0.csv`, `ticks-server1.csv`, … | One row per physics step per server. This is where real numbers come from. |
| `mid.log` | Everything the servers printed, including the `@@FINAL` totals. |
| `mgr.log`, `cli.log` | Manager and client output. |
| `manifest.json` | Exactly what settings this run used. |

### The options you will actually use

| Option | Meaning |
|---|---|
| `-Servers N` | How many game servers to split the world across. |
| `-Objects N` | How many objects to create at the start. |
| `-Ticks N` | Run for N physics steps. **Use this for correctness work.** |
| `-Seconds N` | Run for N seconds of real time instead. Use with `-Ticks 0`. |
| `-Workload NAME` | Which test scenario. See [Workloads](WORKLOADS.md). |
| `-World "minX,maxX,minZ,maxZ"` | The size of the world. Default `-150,150,-150,150`. |
| `-Seed N` | Random seed. Same seed means the same starting world. |
| `-Tag NAME` | The folder name under `runs/`. |
| `-HaloWidth W` | Turn on cross-boundary collision, with a band W units wide. `0` (default) turns it off. |
| `-InterestRadius R` | Only send a client objects within R units of it. `0` sends everything. |
| `-PhysicsThreads N` | Worker threads per server for collision. `0` keeps it single-threaded. |
| `-DrainSeconds N` | After the run, keep the network alive N seconds so in-flight handovers land. |

### Two ways to bound a run, and it matters which you pick

This is the single most important thing to get right.

**`-Ticks N` — fixed amount of simulated work.** Every server does exactly N physics steps, each
advancing time by exactly the same amount, paced to a shared clock. Two runs with the same
settings give the same answer. **Use this for anything about correctness**: conservation,
ownership, handover counts.

**`-Seconds N` — fixed amount of real time.** The servers run as fast as the machine allows for N
seconds. This shows genuine behaviour under load, but it is **not repeatable** — the number of
steps completed varies with whatever else your machine is doing. **Use this for performance
claims, and always repeat it several times.**

> **Do not use `-Seconds` and then compare exact object counts between runs.** They will differ,
> and the difference means nothing.

---

## 4. `run-experiments.ps1` — a sweep with repeats

**What it does.** Runs `measure.ps1` many times, changing one setting each time, repeating each
setting several times, and writing a manifest describing the whole experiment.

```powershell
# 1, 2 and 4 servers at 400 objects, 3 repeats of each
powershell -ExecutionPolicy Bypass -File tools\run-experiments.ps1 `
    -Name scaling -Sweep servers -Values 1,2,4 -Repeats 3 -Objects 400 -Workload uniform
```

Results go to `runs\exp-scaling\`, one folder per run.

### What you can sweep

`servers`, `objects`, `ticks`, `seconds`, `haloWidth`, `interestRadius`, `physicsThreads`,
`rebalanceInterval`.

### Three rules that are easy to get wrong

**1. `-Values` is one string, not a list.**

```powershell
-Values 1,2,4      # correct
```

PowerShell does not parse list arguments for scripts run this way. If you write `-Values 1,2` it
can arrive as the single value `12` and silently run a 12-server experiment. Always check the
values echoed at the top of the output.

**2. Repeats are not optional.** Object counts and handover counts vary slightly between runs
even at the same seed. One run is an anecdote. `analyse.py` reports the **median** across
repeats, so give it at least 3.

**3. A run that produces fewer result files than servers has FAILED.** The script says so in red.
Do not average over it — find out why it failed.

### Growing the world with the server count

If you add servers but keep the world the same size, each region simply gets smaller. That
measures arithmetic, not distribution. To keep objects-per-region constant while the world grows,
add `-ScaleWorldWithServers`:

```powershell
powershell -ExecutionPolicy Bypass -File tools\run-experiments.ps1 `
    -Name locality -Sweep servers -Values 1,2,4 -Repeats 3 `
    -Objects 400 -Workload uniform -ScaleWorldWithServers
```

---

## 5. `analyse.py` — read the results

**What it does.** Reads an experiment folder, prints a report, and **checks correctness**. Needs
only a standard Python 3 install.

```powershell
python tools\analyse.py runs\exp-scaling
```

**The exit code is the important part.** Zero means every correctness check passed. Non-zero
means at least one failed — the run is not a valid result no matter how good the timings look.

### Reading the report

**Per-server timings.** `p50` is the typical step cost, `p95` and `p99` are the slow ones.
Percentiles are used rather than averages because the cost is uneven, and a mean hides spikes.

**Busiest server.** A distributed simulation is only as fast as its slowest participant, so the
report shows the busiest and lightest server and the ratio between them. A large ratio means the
world is badly divided.

**AP frame time.** Maximum time between physics steps, per 5-second window. This is the shape
used by the Aura Projection comparison. The report prints a note explaining what sets the floor,
which differs depending on how you bounded the run — read it before comparing numbers.

**Invariant failures.** Correctness problems. The important ones:

| Name | Meaning if non-zero |
|---|---|
| `conservation_delta` | Objects were lost or created from nothing. |
| `ho_parity_delta` | Handovers sent and received do not match. |
| `ownership_gap_ticks` | At some point an object was owned by nobody. |
| `ownership_double_ticks` | At some point two servers both owned one object. |
| `resurrections` | A destroyed object came back. |

**Reproducibility warning.** Printed separately from failures. It means the run used a recovery
mechanism that fired, so the run will not repeat bit-for-bit. That is the mechanism working as
designed, not a bug — but the run is no longer exactly reproducible and you should not claim it
is.

---

## Common problems

| Symptom | Cause |
|---|---|
| Midware connects, no servers appear | `deploy/` layout is wrong. Re-run `build-deploy.ps1`. |
| A run reports fewer servers than requested | A server crashed or never started. Read `mid.log`. |
| Object counts differ between identical runs | You used `-Seconds`. Use `-Ticks` for repeatability. |
| Objects pass through each other at a boundary | `-HaloWidth` is 0. That is the default. |
| `analyse.py` reports no runs | Wrong folder — point it at `runs\exp-NAME`, not `runs\`. |
| Numbers look wrong after a code change | You forgot to re-run `build-deploy.ps1`. The `deploy/` copies are stale. |

---

## The golden rules

1. **Re-run `build-deploy.ps1` after every code change.** The system runs the copies in
   `deploy/`, not what you just built.
2. **Use `-Ticks` for correctness, `-Seconds` for performance.**
3. **Always repeat.** Three runs minimum for anything you intend to quote.
4. **Check the exit code of `analyse.py`.** A fast run that fails an invariant is not a result.
5. **Read the manifest.** If it says the working tree was dirty, that dataset cannot be
   reproduced from the recorded commit.
