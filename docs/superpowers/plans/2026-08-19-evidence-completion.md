# Evidence Completion Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Produce the four missing experiments (E5–E8) on a frozen server build, teach `analyse.py` to report a soundness knee, and retract the stale self-criticism in the docs.

**Architecture:** Almost none of this touches the servers. E5–E7 are new *run configurations* of the existing build; E8 is arithmetic over counters already emitted. The only compiled change is a size probe in the test target, which does not rebuild the deployed servers. The one real piece of software is a knee-detection and reporting section in `analyse.py`, developed test-first.

**Tech Stack:** PowerShell 5.1 (`tools/*.ps1`), Python 3 standard library only (`tools/analyse.py`), MSVC x64 C++20 (`tools/InteractionTests`).

**Spec:** `docs/superpowers/specs/2026-08-19-evidence-completion-design.md`

## Global Constraints

- **The server binaries are frozen for Tasks 2–7.** Do not edit anything under `DistributedGameServer/`, `DistributedPhysicsManager/`, `PhysicsServerMidware/`, `CSC8503CoreClasses/` or `CSC8503/`. Any change there invalidates every run already collected and the whole sequence restarts.
- **All measurement runs are Release.** `tools/build-deploy.ps1` defaults to `-Config Debug`, where MSVC disables inlining and enables checked iterators. Debug timings are unusable.
- **`analyse.py` depends only on the Python standard library.** No pandas, no matplotlib, no pip install. The numbers must be reproducible on the machine that produced the data with no environment setup.
- **`-Values` is a comma-separated STRING, not an array.** `powershell -File` does not parse array arguments: `-Values 1,2` arrives as the single value `12`.
- **Every measurement run needs `--fixed-step` and a seed.** Supplied by `run-experiments.ps1` defaults (`-Seed 42`); do not override.
- **Halo constants mirrored in Python must match the server.** `HALO_ASSUMED_MAX_SPEED = 60.0`, `HALO_ASSUMED_MAX_RADIUS = 2.0`, substep 120 Hz — declared at `DistributedGameServer/ServerWorldManager.cpp:656-668`.
- Commit messages: short, lower-case, conventional prefix, no co-author trailers.

---

### Task 1: Knee detection and the soundness report in `analyse.py`

The metric for E5 (`ho_sent`) is already collected and already lands in `summary.csv`. What does not exist is any notion of a *knee* — the width at which border crossings stop — or of the predicted floor to compare it against. Built first, so E5's output is readable the moment the runs finish.

**Files:**
- Modify: `tools/analyse.py` (add constants + 2 functions + 2 report sections + multi-directory `main`)
- Create: `tools/test_analyse.py`

**Interfaces:**
- Consumes: nothing from earlier tasks.
- Produces:
  - `predicted_halo_floor(lookahead_ticks: int, substep_hz: int = 120) -> float`
  - `find_knee(crossings_by_width: dict[float, float]) -> float | None`
  - `print_knee_report(manifest: dict, rows: list[dict]) -> dict | None` — returns `{"lookahead": int, "floor": float, "knee": float|None}` or `None` when the experiment is not a `haloWidth` sweep.
  - `main()` accepts one **or more** experiment directories via `sys.argv[1:]`.

- [ ] **Step 1: Write the failing test**

Create `tools/test_analyse.py`:

```python
"""Unit tests for the analysis helpers in analyse.py.

Standard library only, for the same reason analyse.py is: the numbers have to be
checkable on the machine that produced the data with no environment setup.

Run: python tools/test_analyse.py
"""

import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import analyse


class PredictedHaloFloorTests(unittest.TestCase):
    """Mirrors ServerWorldManager::MinimumSafeHaloWidth().

    w_min = 60 * lookahead / substep_hz + 2 * 2, which at 120 Hz is 0.5*L + 4.
    """

    def test_zero_lookahead_is_just_the_body_allowance(self):
        self.assertAlmostEqual(analyse.predicted_halo_floor(0), 4.0)

    def test_the_three_lookaheads_e5_sweeps(self):
        self.assertAlmostEqual(analyse.predicted_halo_floor(2), 5.0)
        self.assertAlmostEqual(analyse.predicted_halo_floor(16), 12.0)
        self.assertAlmostEqual(analyse.predicted_halo_floor(32), 20.0)

    def test_a_negative_lookahead_clamps_rather_than_shrinking_the_floor(self):
        self.assertAlmostEqual(analyse.predicted_halo_floor(-8), 4.0)

    def test_substep_rate_changes_the_floor(self):
        # Half the rate means twice the lag per tick, so twice the speed term.
        self.assertAlmostEqual(analyse.predicted_halo_floor(16, substep_hz=60), 20.0)

    def test_a_non_positive_substep_rate_is_rejected(self):
        with self.assertRaises(ValueError):
            analyse.predicted_halo_floor(4, substep_hz=0)


class FindKneeTests(unittest.TestCase):

    def test_clean_step_returns_the_first_zero(self):
        crossings = {2: 100, 3: 100, 4: 44, 5: 0, 6: 0, 7: 0}
        self.assertEqual(analyse.find_knee(crossings), 5)

    def test_an_isolated_zero_below_the_step_is_not_a_knee(self):
        # A single zero followed by a non-zero at a LARGER width is noise. Reporting
        # it would claim the condition holds at a width where it demonstrably does
        # not, which is the one error this whole experiment exists to avoid.
        crossings = {2: 100, 4: 0, 5: 100, 6: 0, 7: 0}
        self.assertEqual(analyse.find_knee(crossings), 6)

    def test_no_swept_width_suffices(self):
        self.assertIsNone(analyse.find_knee({2: 100, 3: 88, 4: 12}))

    def test_every_width_suffices(self):
        self.assertEqual(analyse.find_knee({9: 0, 10: 0, 11: 0}), 9)

    def test_empty_sweep(self):
        self.assertIsNone(analyse.find_knee({}))


class PrintKneeReportTests(unittest.TestCase):

    def _rows(self, crossings_by_width):
        # summarise_run duplicates run-level invariants onto EVERY server row, so a
        # two-server run contributes the same ho_sent twice per repeat. The report
        # must dedupe by (point, repeat) or a median over rows would be fine here by
        # luck and wrong the moment server counts differ.
        rows = []
        for width, value in crossings_by_width.items():
            for repeat in (1, 2, 3):
                for server in (0, 1):
                    rows.append({"point": width, "repeat": repeat,
                                 "server": server, "ho_sent": value})
        return rows

    def test_returns_none_for_a_non_halo_sweep(self):
        manifest = {"sweep": "servers", "fixed": {"haloLookahead": 4}}
        self.assertIsNone(analyse.print_knee_report(manifest, self._rows({1: 0})))

    def test_reports_floor_and_knee(self):
        manifest = {"sweep": "haloWidth", "fixed": {"haloLookahead": 16}}
        rows = self._rows({9: 100, 10: 100, 11: 30, 12: 0, 13: 0, 14: 0})
        result = analyse.print_knee_report(manifest, rows)
        self.assertEqual(result["lookahead"], 16)
        self.assertAlmostEqual(result["floor"], 12.0)
        self.assertEqual(result["knee"], 12)

    def test_a_knee_above_the_floor_is_reported_as_unsound(self):
        # The bound must never be optimistic. A knee ABOVE the predicted floor means
        # contacts were missed at a width the formula declared safe.
        manifest = {"sweep": "haloWidth", "fixed": {"haloLookahead": 2}}
        rows = self._rows({2: 100, 3: 100, 4: 100, 5: 60, 6: 0, 7: 0})
        result = analyse.print_knee_report(manifest, rows)
        self.assertAlmostEqual(result["floor"], 5.0)
        self.assertEqual(result["knee"], 6)
        self.assertFalse(result["sound"])

    def test_a_knee_at_or_below_the_floor_is_sound(self):
        manifest = {"sweep": "haloWidth", "fixed": {"haloLookahead": 2}}
        rows = self._rows({2: 100, 3: 0, 4: 0, 5: 0})
        result = analyse.print_knee_report(manifest, rows)
        self.assertEqual(result["knee"], 3)
        self.assertTrue(result["sound"])


if __name__ == "__main__":
    unittest.main()
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `python tools/test_analyse.py`
Expected: FAIL with `AttributeError: module 'analyse' has no attribute 'predicted_halo_floor'`

- [ ] **Step 3: Add the constants and the two functions**

Insert into `tools/analyse.py` immediately after the `WARMUP_TICKS = 200` line:

```python
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
```

- [ ] **Step 4: Add the report section**

Insert into `tools/analyse.py` immediately after `find_knee`:

```python
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
```

- [ ] **Step 5: Run the test to verify it passes**

Run: `python tools/test_analyse.py`
Expected: PASS, 14 tests

- [ ] **Step 6: Make `main()` take one or more experiment directories**

E5 is three experiments (one per lookahead), and the *tracking* half of the claim can only be seen across them. Restructure `main()` in `tools/analyse.py`:

Replace the opening of `main()`:

```python
def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2

    experiment_dir = sys.argv[1]
```

with:

```python
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


def analyse_experiment(experiment_dir):
    """Returns (exit_code, knee_dict_or_None), or None if the directory is unusable."""
```

Then indent the remaining original body of `main()` under `analyse_experiment`, and change its three `return` statements:

- `return 1` (no runs found) becomes `return None`
- `return 1` (no usable runs) becomes `return None`
- the final `return 1 if failures else 0` becomes:

```python
    knee = print_knee_report(manifest, rows)

    print(f"\nsummary -> {out_path}")
    return (1 if failures else 0), knee
```

Move the existing `print(f"\nsummary -> {out_path}")` line so it comes after the knee report rather than before.

- [ ] **Step 7: Verify nothing regressed on an existing experiment**

Run: `python tools/analyse.py runs/exp-locality`
Expected: the same E1 table as before, and no knee section (the sweep is `servers`, not `haloWidth`).

Then run it on two directories at once:

Run: `python tools/analyse.py runs/exp-locality runs/exp-halo`
Expected: two reports separated by a rule; a knee section under the halo one only; no tracking table (only one knee).

- [ ] **Step 8: Commit**

```bash
git add tools/analyse.py tools/test_analyse.py
git commit -m "analyse: report the halo soundness knee against its predicted floor"
```

---

### Task 2: Freeze the build — one Release deploy for every run in Tasks 3–7

**Files:**
- Modify: none. This task produces artifacts, not source changes.

**Interfaces:**
- Consumes: nothing.
- Produces: `deploy/{Manager,Midware,Client,DistributedPhysicsServer}/EntryPoint.exe` built Release from a clean tree, and a recorded commit hash that every experiment manifest in Tasks 3–7 must match.

- [ ] **Step 1: Confirm the tree is clean**

Run: `git status --porcelain`
Expected: empty output. A dirty tree makes every manifest in this sequence say `gitDirty: true`, which marks the whole dataset non-reproducible.

- [ ] **Step 2: Record the frozen commit**

Run: `git rev-parse HEAD`
Write the hash down. Every `experiment.json` produced in Tasks 3–7 must carry this same `gitCommit`.

- [ ] **Step 3: Build and deploy Release**

Run:
```powershell
powershell -ExecutionPolicy Bypass -File tools\build-deploy.ps1 -Config Release
```
Expected: four staged binaries under `deploy/`. This takes several minutes.

- [ ] **Step 4: Verify all four binaries exist and are Release-fresh**

Run:
```powershell
Get-ChildItem deploy\*\EntryPoint.exe | Select-Object FullName, Length, LastWriteTime
```
Expected: four rows, all with a `LastWriteTime` from this build.

- [ ] **Step 5: Smoke-test one short run before spending two hours**

Run:
```powershell
powershell -ExecutionPolicy Bypass -File tools\measure.ps1 -Servers 2 -Objects 100 -Ticks 600 -Workload headon -HaloWidth 8 -HaloLookahead 4 -HaloReliable -Tag smoke -OutDir runs\smoke
```
Expected: `runs\smoke\smoke\ticks-server0.csv` and `ticks-server1.csv` both exist. If fewer than two CSVs appear, stop — the deployment is broken and every subsequent task would silently collect nothing.

- [ ] **Step 6: No commit**

Nothing changed in the tree. `deploy/` and `runs/` are build outputs. Confirm with `git status --porcelain` (expected: empty, or only ignored paths).

---

### Task 3: E5 — the soundness sweep

54 runs across three experiments. This is the paper's headline and the only task here that needs unattended time (roughly 1½–2 hours).

**Files:**
- Create: `runs/exp-haloL2/`, `runs/exp-haloL16/`, `runs/exp-haloL32/` (run outputs)
- Create: `docs/superpowers/results/2026-08-19-E5-soundness.md`

**Interfaces:**
- Consumes: `predicted_halo_floor`, `find_knee`, `print_knee_report`, multi-directory `main()` from Task 1; the frozen Release deploy from Task 2.
- Produces: a results document recording the three knees, their floors, and the soundness/tracking verdicts.

- [ ] **Step 1: Run lookahead 2, floor 5.0**

```powershell
powershell -ExecutionPolicy Bypass -File tools\run-experiments.ps1 `
    -Name haloL2 -Sweep haloWidth -Values "2,3,4,5,6,7" -Repeats 3 `
    -Servers 2 -Objects 100 -Ticks 1800 -Workload headon `
    -HaloLookahead 2 -HaloReliable
```
Expected: 18 runs, each printing `ok: 2/2 servers`. Any `FAILED:` line means that point produced no metrics and must be re-run before the sweep is analysed.

- [ ] **Step 2: Run lookahead 16, floor 12.0**

```powershell
powershell -ExecutionPolicy Bypass -File tools\run-experiments.ps1 `
    -Name haloL16 -Sweep haloWidth -Values "9,10,11,12,13,14" -Repeats 3 `
    -Servers 2 -Objects 100 -Ticks 1800 -Workload headon `
    -HaloLookahead 16 -HaloReliable
```
Expected: 18 runs, all `ok: 2/2 servers`.

- [ ] **Step 3: Run lookahead 32, floor 20.0**

```powershell
powershell -ExecutionPolicy Bypass -File tools\run-experiments.ps1 `
    -Name haloL32 -Sweep haloWidth -Values "17,18,19,20,21,22" -Repeats 3 `
    -Servers 2 -Objects 100 -Ticks 1800 -Workload headon `
    -HaloLookahead 32 -HaloReliable
```
Expected: 18 runs, all `ok: 2/2 servers`.

- [ ] **Step 4: Confirm the three manifests carry the frozen commit and are not dirty**

Run:
```powershell
Get-Content runs\exp-haloL2\experiment.json, runs\exp-haloL16\experiment.json, runs\exp-haloL32\experiment.json | Select-String "gitCommit|gitDirty|haloLookahead"
```
Expected: the same `gitCommit` as Task 2 Step 2 in all three, `gitDirty` false in all three, and `haloLookahead` of 2, 16 and 32 respectively. If any manifest is dirty the dataset is not reproducible and the runs must be repeated on a clean tree.

- [ ] **Step 5: Analyse all three together**

Run:
```powershell
python tools\analyse.py runs\exp-haloL2 runs\exp-haloL16 runs\exp-haloL32
```
Expected: three reports, each ending in a knee section, followed by one `halo soundness across lookaheads` table with three rows.

- [ ] **Step 6: Write the results document**

Create `docs/superpowers/results/2026-08-19-E5-soundness.md`. It must record, verbatim from Step 5's output:

1. The three per-lookahead tables (width, median crossings, repeats).
2. The tracking table (lookahead, floor, knee, slack, verdict).
3. Which of the two claims held:
   - **soundness** — every knee at or below its floor;
   - **tracking** — the knee moving right as the lookahead grows.
4. The envelope, stated explicitly: `HALO_ASSUMED_MAX_SPEED = 60` is a hardcoded global constant, `headon` actually runs at 30, so positive slack is *expected* and is the margin the conservative constant buys. An object faster than 60 falls outside the guarantee.
5. The two limitations from the spec, unchanged: one synthetic workload, and this validates the formula as coded rather than a derivation.

If tracking fails at lookahead 32 while soundness holds, record it as a **result**, not a defect: extrapolation error growing faster than the linear bound is the measured form of "halo lag is permanent, keep the lookahead small".

- [ ] **Step 7: Commit**

```bash
git add docs/superpowers/results/2026-08-19-E5-soundness.md
git commit -m "docs: E5 halo soundness sweep results"
```

---

### Task 4: Packet sizes, taken from the compiler rather than by hand

E8 converts snapshot and halo counters into bytes. Hand-adding struct members gets padding and alignment wrong, so the sizes come from `sizeof` in the same toolchain that built the servers. `InteractionTests` links the same headers, and building it does **not** rebuild the deployed servers, so the freeze holds.

**Files:**
- Create: `tools/InteractionTests/PacketSizeTests.cpp`
- Modify: `tools/InteractionTests/CMakeLists.txt` (add the new source to `add_executable`)

**Interfaces:**
- Consumes: nothing from earlier tasks.
- Produces: a printed line `PACKETSIZE full=<n> delta=<n> haloEntry=<n> haloHeader=<n>` in the test output, consumed by hand in Task 5.

The harness is the repo's own dependency-free one in `tools/InteractionTests/TestHarness.h`: tests are declared with `TEST(Name)`, **self-register** through a static `AutoRegister`, and assert with `CHECK` / `CHECK_EQ` / `CHECK_NEAR`. There is no `Run*Tests()` convention and `main.cpp` needs no edit — it calls `NCL::Testing::RunAllTests()` and nothing else.

- [ ] **Step 1: Write the failing test**

Create `tools/InteractionTests/PacketSizeTests.cpp`:

```cpp
#include "TestHarness.h"

#include "NetworkObject.h"

#include <iostream>

// E8 costs snapshot and halo traffic in BYTES, from counters that record COUNTS.
// The conversion factor is sizeof, taken here rather than hand-added from the struct
// declaration: padding and alignment are not visible in the source, and an error in
// this constant scales the entire bandwidth claim.
//
// The printed line is the deliverable. The checks exist so that a wire-format change
// altering a packet size fails a test rather than silently invalidating a published
// figure.

TEST(PacketSizesForBandwidthAccounting) {
	std::cout << "PACKETSIZE"
		<< " full=" << sizeof(NCL::CSC8503::FullPacket)
		<< " delta=" << sizeof(NCL::CSC8503::DeltaPacket)
		<< " haloEntry=" << sizeof(NCL::CSC8503::HaloObjectState)
		<< " haloHeader=" << (sizeof(NCL::CSC8503::HaloUpdatePacket)
			- sizeof(NCL::CSC8503::HaloObjectState)
				* NCL::CSC8503::HaloUpdatePacket::MAX_ENTRIES)
		<< "\n";

	// A delta must be smaller than a full snapshot, or the delta path costs more than
	// it saves and the 1 full : 5 delta cadence is a pessimisation.
	CHECK(sizeof(NCL::CSC8503::DeltaPacket) < sizeof(NCL::CSC8503::FullPacket));

	// The halo batch is sized to stay inside a typical 1400-byte MTU. Larger batches
	// are fragmented by ENet, which costs a retransmit of the whole packet if any one
	// fragment is lost.
	CHECK(sizeof(NCL::CSC8503::HaloUpdatePacket) <= 1400);
}
```

- [ ] **Step 2: Add the source to the test target**

In `tools/InteractionTests/CMakeLists.txt`, add `"PacketSizeTests.cpp"` to the `add_executable(InteractionTests ...)` source list, after `"HandoffPacketTests.cpp"`. No other file changes — registration is automatic.

- [ ] **Step 3: Build the test target**

Run:
```powershell
msbuild DistributedPhysicsSystem.sln /t:Tools\InteractionTests /p:Configuration=Debug /p:Platform=x64
```
Expected: builds clean. Note the `Tools\` solution-folder prefix — the target does not resolve without it.

- [ ] **Step 4: Run and capture the sizes**

Run:
```powershell
.\tools\InteractionTests\Debug\InteractionTests.exe
```
Expected: exit code 0, and a line beginning `PACKETSIZE full=`. Record the four numbers; Task 5 uses them.

- [ ] **Step 5: Verify the deployed servers were not rebuilt**

Run:
```powershell
Get-ChildItem deploy\*\EntryPoint.exe | Select-Object FullName, LastWriteTime
```
Expected: the same timestamps as Task 2 Step 4. If they changed, the freeze is broken and Task 3 must be re-run.

- [ ] **Step 6: Commit**

```bash
git add tools/InteractionTests/PacketSizeTests.cpp tools/InteractionTests/CMakeLists.txt
git commit -m "test: pin the wire-format packet sizes E8 converts counts with"
```

---

### Task 5: E8 — bytes, and the Dyconits composition claim

**Files:**
- Create: `runs/exp-bytes/` (run output)
- Create: `docs/superpowers/results/2026-08-19-E8-bandwidth.md`

**Interfaces:**
- Consumes: the packet sizes printed in Task 4; the frozen deploy from Task 2.
- Produces: a results document stating the halo's server↔server cost and interest management's server→client saving in bytes per second, with the comparison bounded rather than point-estimated.

- [ ] **Step 1: Run the sweep with the halo on**

Interest radius is the swept axis; the halo is on throughout, so the halo cost is constant across the sweep and the snapshot saving is what varies.

```powershell
powershell -ExecutionPolicy Bypass -File tools\run-experiments.ps1 `
    -Name bytes -Sweep interestRadius -Values "0,50" -Repeats 3 `
    -Servers 2 -Objects 4000 -Ticks 0 -Seconds 20 -Workload uniform `
    -HaloWidth 8 -HaloLookahead 4
```
Expected: 6 runs, all `ok: 2/2 servers`.

Note `-Ticks 0 -Seconds 20`: bandwidth is a **rate**, so it has to be measured against wall-clock time. A paced run would report whatever the pacing allowed. `-HaloReliable` is deliberately omitted for the same reason — a deployment does not use it, and this run is measuring deployment cost, not reproducing a trajectory.

- [ ] **Step 2: Analyse**

Run: `python tools\analyse.py runs\exp-bytes`
Expected: a report with `snap_sent`, `snap_suppressed` and the halo counters at both radii. No knee section (the sweep is `interestRadius`).

- [ ] **Step 3: Read the halo totals out of the run logs**

`haloObjSent` is not among the invariants `analyse.py` promotes, so take it directly:

```powershell
Select-String -Path runs\exp-bytes\*\mid.log -Pattern "@@FINAL role=server" | ForEach-Object { $_.Line }
```
Record `haloObjSent` and `haloSent` per server per run. `haloObjSent` counts *entries*; `haloSent` counts *packets*, and a packet carries up to 20 entries.

- [ ] **Step 4: Compute the comparison as a bounded range, not a point estimate**

The snapshot stream mixes full and delta packets on a 1 full : 5 delta cadence, and `snapSent` does not distinguish them. Rather than assume the mix holds exactly, bound it:

```
snapshot bytes, upper bound = snap_sent * (sizeof(FullPacket)  + TRANSPORT_OVERHEAD)
snapshot bytes, lower bound = snap_sent * (sizeof(DeltaPacket) + TRANSPORT_OVERHEAD)
halo bytes                  = halo_sent * (halo_header + halo_entry * entries_per_packet
                                           + TRANSPORT_OVERHEAD)
                              where entries_per_packet = halo_obj_sent / halo_sent
```

`TRANSPORT_OVERHEAD = 36 bytes` — roughly 8 bytes of ENet header plus 28 of UDP/IP. **This must be included.** A `DeltaPacket` is a couple of dozen bytes of payload, so costing snapshots at payload size alone understates the traffic by more than a factor of two, and it understates it *most* for the small packets interest management removes — that is, in the direction that flatters our own result. Halo batches carry up to 20 entries per packet and are barely affected, so omitting overhead would compare the two on different terms.

The composition claim holds **only if** the halo's cost is below the snapshot saving at its *least favourable* reading: halo bytes computed as above, against the saving computed with the **lower** snapshot bound. State it that way or not at all.

- [ ] **Step 5: Write the results document**

Create `docs/superpowers/results/2026-08-19-E8-bandwidth.md` recording:

1. The four packet sizes from Task 4, with the commit they were measured at.
2. `snap_sent` at radius 0 and radius 50, and the saving between them.
3. `haloSent`, `haloObjSent`, and the derived entries-per-packet.
4. Both snapshot bounds and the halo figure, all in bytes/second, with `TRANSPORT_OVERHEAD` stated.
5. The verdict on the composition claim, using the least favourable reading.
6. The stated caveat: `snapSent` does not separate full from delta, which is why the result is a bound rather than a figure. Removing the ambiguity needs a second counter, and that is a server change held for the build phase.

- [ ] **Step 6: Commit**

```bash
git add docs/superpowers/results/2026-08-19-E8-bandwidth.md
git commit -m "docs: E8 bandwidth bounds and the halo-vs-interest comparison"
```

---

### Task 6: E6 — density

**Files:**
- Create: `runs/exp-density/` (run output)
- Create: `docs/superpowers/results/2026-08-19-E6-density.md`

**Interfaces:**
- Consumes: the frozen deploy from Task 2.
- Produces: a results document reporting `physicsMs` percentiles against objects-per-broadphase-cell.

- [ ] **Step 1: Run the sweep at fixed world bounds**

One server, so the figure is per-server physics cost with no handoff or halo traffic mixed in. The world is held fixed while the object count grows, which is what makes this a density sweep rather than a repeat of the scale curve.

```powershell
powershell -ExecutionPolicy Bypass -File tools\run-experiments.ps1 `
    -Name density -Sweep objects -Values "500,1000,2000,4000,8000" -Repeats 3 `
    -Servers 1 -Ticks 1800 -Workload uniform -World "-150,150,-150,150"
```
Expected: 15 runs, all `ok: 1/1 servers`.

- [ ] **Step 2: Analyse**

Run: `python tools\analyse.py runs\exp-density`
Expected: p50/p95/p99 `physicsMs` per point.

- [ ] **Step 3: Convert the x-axis to density**

The broadphase cell size is `std::max(1.0f, maxExtent * 4.0f)` (`CSC8503CoreClasses/PhysicsSystem.cpp:651`), where `maxExtent` is the largest half-extent among the dynamic objects that tick. The workload's objects are unit cubes, so the expected cell size is 4.0 and a 300-unit world gives 75 cells per axis, 5,625 cells.

Confirm that by reading line 651 rather than assuming it, then express each swept point as objects-per-cell:

| objects | objects / 5,625 cells |
|---|---|
| 500 | 0.089 |
| 1,000 | 0.178 |
| 2,000 | 0.356 |
| 4,000 | 0.711 |
| 8,000 | 1.422 |

That conversion is the whole purpose of the experiment: the grid is linear in objects, but its constant is set by occupancy, and a real game world varies occupancy rather than count.

- [ ] **Step 4: Write the results document**

Create `docs/superpowers/results/2026-08-19-E6-density.md` with the objects/cell against p95 curve, the fitted exponent, and an explicit statement of whether cost stays linear in objects as density rises or degrades — the latter being the answer that matters, since it bounds how much clustering a region can absorb before the partition has to move.

- [ ] **Step 5: Commit**

```bash
git add docs/superpowers/results/2026-08-19-E6-density.md
git commit -m "docs: E6 density sweep results"
```

---

### Task 7: E7 — capacity with the halo on

**Files:**
- Create: `runs/exp-capacity-halo/`, `runs/exp-capacity-nohalo/` (run outputs)
- Create: `docs/superpowers/results/2026-08-19-E7-capacity.md`

**Interfaces:**
- Consumes: the frozen deploy from Task 2.
- Produces: a results document giving the per-server object budget at 120 Hz with cross-border collision enabled, against the halo-off baseline at the same points.

- [ ] **Step 1: Run the halo-on capacity sweep**

```powershell
powershell -ExecutionPolicy Bypass -File tools\run-experiments.ps1 `
    -Name capacity-halo -Sweep objects -Values "1000,2000,4000,8000,12000" -Repeats 3 `
    -Servers 2 -Ticks 1800 -Workload uniform -World "-300,300,-300,300" `
    -HaloWidth 8 -HaloLookahead 4 -HaloReliable
```
Expected: 15 runs, all `ok: 2/2 servers`.

- [ ] **Step 2: Run the matching halo-off baseline**

Identical in every respect but the halo, so the difference is attributable.

```powershell
powershell -ExecutionPolicy Bypass -File tools\run-experiments.ps1 `
    -Name capacity-nohalo -Sweep objects -Values "1000,2000,4000,8000,12000" -Repeats 3 `
    -Servers 2 -Ticks 1800 -Workload uniform -World "-300,300,-300,300" `
    -HaloWidth 0
```
Expected: 15 runs, all `ok: 2/2 servers`.

- [ ] **Step 3: Analyse both**

Run: `python tools\analyse.py runs\exp-capacity-halo runs\exp-capacity-nohalo`
Expected: two reports, no knee section on either (the sweep is `objects`).

- [ ] **Step 4: Locate the budget crossing**

The tick budget at 120 Hz is 8.33 ms. Using the **busiest-server p95** from each report — not an average across servers, since the simulation runs no faster than its slowest participant — find the object count at which each configuration crosses 8.33 ms, interpolating between swept points.

- [ ] **Step 5: Write the results document**

Create `docs/superpowers/results/2026-08-19-E7-capacity.md` recording both curves, both crossings, and the cost of the halo as a percentage of per-server budget. State plainly that every capacity figure published before this one (~6,000 serial, ~10,000 threaded, `2026-08-18-scale-ceiling.md` §5.3) was measured at `--halo-width 0`, which is not the configuration the work advocates, and give the corrected budget.

- [ ] **Step 6: Commit**

```bash
git add docs/superpowers/results/2026-08-19-E7-capacity.md
git commit -m "docs: E7 per-server capacity with cross-border collision on"
```

---

### Task 8: Retract the stale warnings

Two defects the docs still warn about have been fixed and the warnings never withdrawn. In an artifact repository a reviewer reads, stale self-criticism reads worse than the defect would have.

**Files:**
- Modify: `CLAUDE.md` (verified-state warnings block)
- Modify: `docs/superpowers/specs/2026-08-18-scale-ceiling.md` (§3.3 and the "Remaining, in order" list in §6)

**Interfaces:**
- Consumes: nothing.
- Produces: nothing consumed by later tasks.

- [ ] **Step 1: Confirm both fixes are real before claiming them**

Run:
```powershell
Select-String -Path DistributedGameServer\DistributedGameServerManager.cpp -Pattern "mServerSideLastFullID = minID"
Select-String -Path DistributedGameServer\ServerWorldManager.cpp -Pattern "PruneForwardingTable\(\)"
```
Expected: one hit at `DistributedGameServerManager.cpp:293`, and both a definition (`:258`) and a call site (`:1088`) for `PruneForwardingTable`. If either is absent, do not retract that warning.

- [ ] **Step 2: Retract the delta warning in `CLAUDE.md`**

Replace the bullet beginning **"Deltas never apply after the first full snapshot."** with:

```markdown
> - **Deltas apply.** `mServerSideLastFullID` is written from the minimum acknowledged
>   state across connected clients (`DistributedGameServerManager.cpp:293`), so the
>   delta baseline advances and deltas are applied rather than discarded. This
>   corrects an earlier warning here that said they never applied - true before the
>   snapshot-acknowledgement work, and not since.
```

- [ ] **Step 3: Retract the forwarding-table warning in the scale-ceiling doc**

In `docs/superpowers/specs/2026-08-18-scale-ceiling.md`, §3.3 currently says `mLastKnownOwner` "is never pruned" and "needs an eviction policy keyed on the same staleness argument the halo retirement uses". Append to that section:

```markdown
> **Closed.** `PruneForwardingTable()` (`ServerWorldManager.cpp:258`, called each tick
> from `:1088`) evicts entries after 1200 ticks, on exactly the staleness argument
> this section asked for. The bound is no longer monotonic.
```

Then remove item 2 (`mLastKnownOwner` never prunes) from the "Remaining, in order" list in §6, renumbering the item below it.

- [ ] **Step 4: Mark the Debug-era figures where a reader meets them**

`2026-08-18-scale-ceiling.md` §0 says every earlier absolute performance figure is a Debug build and should not be quoted, but the figures themselves sit in four other specs with no marking. Add a one-line note at the head of each performance table in:

- `docs/superpowers/specs/2026-08-18-halo-band-cross-border-collision.md`
- `docs/superpowers/specs/2026-08-18-dynamic-repartitioning.md`
- `docs/superpowers/specs/2026-08-18-region-local-world-state.md`
- `docs/superpowers/specs/2026-08-16-evaluation-pipeline-design.md`

Use this exact wording so the note is greppable:

```markdown
> **Debug build — not quotable.** See `2026-08-18-scale-ceiling.md` §0. Relative
> comparisons within this table hold; absolute values are pessimistic by an
> unknown factor.
```

Skip any table that is a count rather than a duration — conservation totals, handoff parity, contact counts and object counts are unaffected by build configuration. Only timings carry the note.

- [ ] **Step 5: Verify the notes landed and nothing else changed**

Run:
```powershell
Select-String -Path docs\superpowers\specs\*.md -Pattern "Debug build - not quotable" | Measure-Object
git diff --stat
```
Expected: the note appears in each spec that has a timing table; `git diff --stat` touches only the six documentation files named above.

- [ ] **Step 6: Commit**

```bash
git add CLAUDE.md docs/superpowers/specs/
git commit -m "docs: retract two fixed warnings, mark debug-era figures"
```

---

### Task 9: The consolidated evaluation document

The specs are a running log of discoveries in the order they were found. That is the right form for a working record and the wrong form for evidence: a reader currently has to reconcile eleven documents, several of which correct each other. This adds one document that presents E1–E8 as a single argument. The specs stay.

**Files:**
- Create: `docs/EVALUATION.md`
- Modify: `CLAUDE.md` (add the row to the reference-docs table)

**Interfaces:**
- Consumes: the results documents from Tasks 3, 5, 6 and 7, plus `2026-08-19-experiment-suite.md` for E1–E4.
- Produces: nothing consumed by later tasks.

- [ ] **Step 1: Write the document**

Create `docs/EVALUATION.md` with this structure, filled from the recorded results — no figure may be introduced here that is not traceable to a run directory:

1. **What is claimed.** Locality (I6), cross-border correctness, the soundness condition, the client-facing cost bound, dynamic balancing. One sentence each.
2. **How it is measured.** Paced vs realtime and why the choice is per-claim; repeats and the median-across-repeats rule; percentiles rather than means because tick cost is bimodal; never pooling ticks across servers.
3. **E1–E8**, one section each: claim, configuration, table, verdict.
4. **The soundness condition**, given its own section as the headline: the floor, the three knees, soundness and tracking, and the declared envelope.
5. **What this evidence cannot show.** Single-machine throughout, so wall-clock comparisons across server counts measure contention rather than distribution; no latency injection; no AP-comparable injection workload yet; `headon` is one synthetic workload.
6. **Known conditional guarantees.** Ownership atomicity holds only while both servers keep pace, which a rebalancing migration violates by definition (E4, ~2,538 gap ticks, nothing lost); the balancer equalises objects rather than contacts.

- [ ] **Step 2: Add it to the reference table in `CLAUDE.md`**

In the "Reference docs" table, add:

```markdown
| `docs/EVALUATION.md` | Every experiment and result as one argument. Read this before quoting any figure from a spec. |
```

- [ ] **Step 3: Verify every figure is traceable**

For each number in `docs/EVALUATION.md`, confirm a corresponding run directory exists under `runs/`. Any figure that cannot be traced to a run must be removed or labelled as an estimate. This is the check that keeps the consolidation a summary rather than a new source of unsourced claims.

- [ ] **Step 4: Commit**

```bash
git add docs/EVALUATION.md CLAUDE.md
git commit -m "docs: consolidated evaluation document"
```

---

## Not in this plan

Held for the build phase, in the spec's order, because each invalidates every run above: multi-machine measurement, the AP-comparable injection workload, latency injection, the handoff ack (`HandleTransitionHandshakeReceived` is an empty body), the ownership gap under rebalancing, the contact-weighted load profile, and wiring up `CalculateIncomingObjectOffsetPosition`.
