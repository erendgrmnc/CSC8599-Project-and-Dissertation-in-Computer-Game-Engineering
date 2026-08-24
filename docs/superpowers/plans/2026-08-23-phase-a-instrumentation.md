# Phase A — Instrumentation and Harness Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make E8's bandwidth comparison rest on measured datagrams and more than one client, and close E3's absolute-count caveat, without altering a single simulation result.

**Architecture:** ENet already counts, per host, the bytes and datagrams it writes to the socket after coalescing outgoing commands. The servers expose those raw counters through `NetworkBase` and print them on the `@@FINAL` line, split by host family — the client-facing sender host carries snapshots, the peer-link hosts carry halo and handoffs. `analyse.py` owns all header-overhead arithmetic, because charging IP/UDP headers is an analysis choice rather than a runtime fact. The harness gains an N-client mode so the client-count argument stops being analytical.

**Tech Stack:** C++20 / MSVC x64, ENet (vendored, `CSC8503CoreClasses/enet/`), PowerShell 5.1 harness scripts, Python 3 stdlib for `analyse.py`, and the dependency-free `tools/InteractionTests` assert harness.

**Spec:** `docs/superpowers/specs/2026-08-23-backlog-completion-design.md` — Phase A is §2; the baseline rule is §1.1 and the documentation rule is §1.2.

## Global Constraints

- **Phase A must not alter any simulation result.** Task 6 is the gate that proves it; no result may be claimed before it passes.
- **There is no stored baseline.** `runs/` is gitignored and empty (spec §1.1), so Task 1 does not start until Task 0 has produced one.
- **Every measurement run passes `--fixed-step` and `--seed`.** Without both, servers under different load integrate with different `dt` and figures are not comparable.
- **`--handoff-delay-ticks` must be 0 for any measurement run.** It is fault injection.
- **Measurement runs use a Release build, and `build-deploy.ps1` defaults to Debug** — always pass `-Config Release` explicitly. Debug duration figures are annotated "not quotable" throughout the results documents. Counts are build-independent; durations are not.
- **Repeats are 3, and the reported figure is the median across them** (`analyse.py` does this).
- **`-Values` is a quoted comma-separated string**, never a bare list: `powershell -File` parses `-Values 1,2` as the single value `12`.
- **New source files must be added to the owning `CMake*.cmake` or `CMakeLists.txt`**, not just to disk.
- **A phase is not done until the documents it invalidates are corrected** (spec §1.2), in the same change as the code.
- **`msbuild` is not on PATH on this machine.** Invoke it by full path, quoted:
  `& "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\MSBuild\Current\Bin\MSBuild.exe"` (verified 17.12.12).
  A bare `msbuild` fails with "command not found" and is not a build failure — do not diagnose it as one.
- **The build tree starts cold.** There is no `CMakeCache.txt` and no `deploy/`, so the first
  configure-and-build is a full build of every vendored library (Recast, Detour, imgui, OpenGL
  backend). Expect it to take a long time; that is not a hang.
- **`analyse.py` only accepts experiment directories**, never a single run directory. It requires
  `experiment.json` plus run subdirectories named `<sweep><value>-r<repeat>`, so every run in this
  plan goes through `run-experiments.ps1` rather than `measure.ps1` directly.

---

## File Structure

| File | Responsibility | Task |
|---|---|---|
| `CSC8503CoreClasses/NetworkBase.h` | Declares the three public host-counter accessors. Must not include `enet.h` — the header forward-declares `_ENetHost` deliberately. | 1 |
| `CSC8503CoreClasses/NetworkBase.cpp` | Defines them; already includes `enet.h`. | 1 |
| `tools/InteractionTests/NetworkCountersTests.cpp` | **New.** Unit-tests the null-host behaviour of the accessors. | 1 |
| `tools/InteractionTests/CMakeLists.txt` | Registers the new test file. | 1 |
| `DistributedGameServer/DistributedGameServerManager.h` | Declares `NetworkByteTotals` and its getter — the only public surface over the two private host families. | 2 |
| `DistributedGameServer/DistributedGameServerManager.cpp` | Sums the sender host and the peer links. | 2 |
| `DistributedGameServer/ServerStarter.cpp` | Emits the four new `@@FINAL` fields. | 2 |
| `tools/analyse.py` | Owns the header-overhead arithmetic (`wire_bytes`) and per-client log discovery. | 3, 4 |
| `tools/test_analyse.py` | Tests for both. | 3, 4 |
| `tools/measure.ps1` | `-Clients N`: manager argument, N client processes, per-client logs, cleanup. | 5 |
| `tools/run-experiments.ps1` | `-Clients N` passthrough and manifest field. | 5 |
| `docs/SPATIAL-PARTITIONING.md` | Two stale claims corrected. | 8 |
| `CLAUDE.md` | `--epoch-align-us` added to the game-server flag table. | 7 |
| `docs/superpowers/results/2026-08-23-A-instrumentation.md` | **New.** Phase A's results record. | 7, 9 |
| `docs/EVALUATION.md` | E3 and E8 figures and caveats; backlog items 10 and 11. | 9 |

---

### Task 0: Establish the step-0 baseline

Nothing in this plan may be gated against a baseline that does not exist. This task produces it.

**Files:**
- Create: `runs/exp-phaseA-baseline/` (gitignored output, not committed)
- Create: `docs/superpowers/results/2026-08-23-A-instrumentation.md`

**Interfaces:**
- Produces: `runs/exp-phaseA-baseline/FINAL-baseline.txt`, the `@@FINAL role=server` lines Task 6 compares against, and the commit SHA in `experiment.json`.

- [ ] **Step 1: Record the commit the baseline is taken at**

```powershell
git rev-parse --short HEAD
git status --porcelain
```

Expected: a clean working tree. If it is not clean, stop and commit or stash first — a baseline taken from a dirty tree cannot be reproduced.

- [ ] **Step 2: Build and stage all four roles in Release**

```powershell
powershell -ExecutionPolicy Bypass -File tools\build-deploy.ps1 -Config Release
```

Expected: `deploy/Manager`, `deploy/Midware`, `deploy/Client`, `deploy/DistributedPhysicsServer` each containing `EntryPoint.exe`.

- [ ] **Step 3: Take the baseline run**

Paced and reproducible, halo on, so the gate covers the halo path as well as the plain one.

Run through `run-experiments.ps1`, not `measure.ps1`: `analyse.py` only accepts an
experiment directory (it requires `experiment.json` and run subdirectories matching
`<sweep><value>-r<repeat>`), and the experiment manifest also records the commit SHA
and a dirty flag — which is exactly what makes a baseline reproducible.

```powershell
powershell -ExecutionPolicy Bypass -File tools\run-experiments.ps1 `
    -Name phaseA-baseline -Sweep ticks -Values "1800" -Repeats 1 `
    -Servers 2 -Objects 400 -Workload uniform `
    -Seed 42 -HaloWidth 8 -HaloReliable -DrainSeconds 5
```

Expected: `runs/exp-phaseA-baseline/experiment.json` and `runs/exp-phaseA-baseline/ticks1800-r1/` containing `ticks-server0.csv`, `ticks-server1.csv`, `mid.log`, `cli.log`, `manifest.json`.

- [ ] **Step 4: Verify the run is clean before trusting it as a baseline**

```powershell
python tools\analyse.py runs\exp-phaseA-baseline
```

Expected: exit code 0, two server CSVs found, no invariant failure. A baseline that already fails an invariant is not a baseline — investigate before continuing.

- [ ] **Step 5: Snapshot the exact values the gate will compare**

The filter must be **exactly** the one Task 6 uses. `mid.log` carries forwarded
server output, so capturing unfiltered `@@FINAL` here and filtering `role=server`
at the gate would let `Compare-Object` report a difference that is an artefact of
the filter rather than of the code — discrediting the only check that makes Phase
A's measurement-only claim verifiable.

```powershell
Select-String -Path runs\exp-phaseA-baseline\ticks1800-r1\mid.log -Pattern "@@FINAL role=server" | ForEach-Object { $_.Line } | Out-File -Encoding utf8 runs\exp-phaseA-baseline\FINAL-baseline.txt
Get-Content runs\exp-phaseA-baseline\FINAL-baseline.txt
```

Expected: one line per server. These lines are what Task 6 diffs against.

- [ ] **Step 6: Open the Phase A results document and record the baseline**

Create `docs/superpowers/results/2026-08-23-A-instrumentation.md`:

```markdown
# Phase A results: instrumentation and harness (2026-08-23)

Plan: `docs/superpowers/plans/2026-08-23-phase-a-instrumentation.md`
Spec: `docs/superpowers/specs/2026-08-23-backlog-completion-design.md` §2

## Step 0 — the baseline

`runs/` was empty at the start of this phase (spec §1.1), so this baseline was
generated rather than cited. Every no-op gate below compares against it.

| | |
|---|---|
| Commit | `<SHA from Step 1>` |
| Configuration | 2 servers, 400 objects, `uniform`, 1,800 paced ticks, seed 42, `--halo-width 8`, halo reliable |
| Build | Release, via `tools/build-deploy.ps1` |
| Invariants | `<paste the analyse.py verdict>` |

`@@FINAL` lines are stored at `runs/exp-phaseA-baseline/FINAL-baseline.txt`.
```

- [ ] **Step 7: Commit**

```bash
git add docs/superpowers/results/2026-08-23-A-instrumentation.md
git commit -m "docs(A): record the Phase A step-0 baseline

runs/ was gitignored and empty, so no stored dataset exists to gate
against. This baseline is generated from a clean tree at a recorded
commit and is what every no-op gate in Phase A compares to."
```

---

### Task 1: Expose ENet's per-host byte and datagram counters

**Files:**
- Modify: `CSC8503CoreClasses/NetworkBase.h` (public section of `class NetworkBase`, around `:173-177`)
- Modify: `CSC8503CoreClasses/NetworkBase.cpp`
- Create: `tools/InteractionTests/NetworkCountersTests.cpp`
- Modify: `tools/InteractionTests/CMakeLists.txt`

**Interfaces:**
- Consumes: nothing from earlier tasks.
- Produces: `unsigned int NetworkBase::GetTotalSentData() const`, `unsigned int NetworkBase::GetTotalSentPackets() const`, `unsigned int NetworkBase::GetTotalReceivedData() const` — all public, all safe on a null host. Task 2 calls these on `GameServer` and `GameClient` instances.

- [ ] **Step 1: Write the failing test**

Create `tools/InteractionTests/NetworkCountersTests.cpp`:

```cpp
#include "TestHarness.h"

#include "NetworkBase.h"

// NetworkBase's constructor is protected, so reaching it needs a derived type.
// Nothing else is required here: the property under test is what the accessors do
// when no ENet host was ever created.
namespace {
	struct HostlessNetworkBase : public NetworkBase {
		HostlessNetworkBase() = default;
	};
}

// A game server that never received its start packet still prints its @@FINAL line -
// that failure mode is exactly what the line exists to make visible. If these
// accessors dereferenced netHandle they would crash on precisely those runs.
TEST(ByteCountersReadZeroWithoutAHost) {
	HostlessNetworkBase base;
	CHECK_EQ(base.GetTotalSentData(), 0u);
	CHECK_EQ(base.GetTotalSentPackets(), 0u);
	CHECK_EQ(base.GetTotalReceivedData(), 0u);
}
```

Register it in `tools/InteractionTests/CMakeLists.txt`, in the `add_executable(InteractionTests ...)` list, after `"InjectionScheduleTests.cpp"`:

```cmake
    "InjectionScheduleTests.cpp"
    "NetworkCountersTests.cpp"
)
```

- [ ] **Step 2: Run it to make sure it fails**

```powershell
cmake -G "Visual Studio 17 2022" -A x64 .
msbuild DistributedPhysicsSystem.sln /t:Tools\InteractionTests /p:Configuration=Debug /p:Platform=x64
```

Expected: **compile error**, `error C2039: 'GetTotalSentData': is not a member of 'NetworkBase'`. For C++ a failing compile is the failing test — do not proceed until you have seen it.

- [ ] **Step 3: Write the minimal implementation**

In `CSC8503CoreClasses/NetworkBase.h`, add to the **public** section of `class NetworkBase`, immediately after `void ClearPacketHandlers();`:

```cpp
	// ENet's own totals for this host: bytes and datagrams actually written to the
	// socket. totalSentData is incremented with the return value of enet_socket_send
	// AFTER outgoing commands are coalesced into one datagram (enet/protocol.c:1732),
	// so these two together measure real wire cost without modelling coalescing at
	// all - which is what E8's payload-vs-datagram ambiguity came down to.
	//
	// Zero when no host exists. A role that never created one still prints @@FINAL,
	// and that report must not dereference null.
	//
	// Both are enet_uint32 and wrap after 4 GB - about 15 minutes at E8's measured
	// rates. Safe for a 20 s run; a longer run needs these accumulated into 64 bits
	// during the run rather than read once at exit.
	unsigned int GetTotalSentData() const;
	unsigned int GetTotalSentPackets() const;
	unsigned int GetTotalReceivedData() const;
```

In `CSC8503CoreClasses/NetworkBase.cpp`, add after `void NetworkBase::ClearPacketHandlers() { ... }`:

```cpp
unsigned int NetworkBase::GetTotalSentData() const {
	return (netHandle != nullptr) ? netHandle->totalSentData : 0u;
}

unsigned int NetworkBase::GetTotalSentPackets() const {
	return (netHandle != nullptr) ? netHandle->totalSentPackets : 0u;
}

unsigned int NetworkBase::GetTotalReceivedData() const {
	return (netHandle != nullptr) ? netHandle->totalReceivedData : 0u;
}
```

- [ ] **Step 4: Run the tests and make sure they pass**

```powershell
msbuild DistributedPhysicsSystem.sln /t:Tools\InteractionTests /p:Configuration=Debug /p:Platform=x64
.\tools\InteractionTests\Debug\InteractionTests.exe
```

Expected: `[       OK ] ByteCountersReadZeroWithoutAHost`, and a final line reporting 0 failed. Exit code 0.

- [ ] **Step 5: Commit**

```bash
git add CSC8503CoreClasses/NetworkBase.h CSC8503CoreClasses/NetworkBase.cpp tools/InteractionTests/NetworkCountersTests.cpp tools/InteractionTests/CMakeLists.txt
git commit -m "feat(net): expose ENet's per-host sent-bytes and datagram counters

ENet increments totalSentData with the return of enet_socket_send, after
coalescing a peer's queued commands into one datagram, and counts the
datagram alongside it. Reading both removes the need for the per-packet
overhead model E8's verdict currently depends on.

Accessors return 0 rather than dereferencing a null host: a server that
never received its start packet still prints @@FINAL, which is exactly
the run whose failure that line reports."
```

---

### Task 2: Report the counters on the server's @@FINAL line

**Files:**
- Modify: `DistributedGameServer/DistributedGameServerManager.h` (public section, near `SetHaloReliable` at `:43`)
- Modify: `DistributedGameServer/DistributedGameServerManager.cpp`
- Modify: `DistributedGameServer/ServerStarter.cpp:250-277` (the capture block) and the `@@FINAL` stream

**Interfaces:**
- Consumes: `NetworkBase::GetTotalSentData()` / `GetTotalSentPackets()` from Task 1.
- Produces: four `@@FINAL` fields — `netCliBytes`, `netCliPkts`, `netPeerBytes`, `netPeerPkts` — which Task 3 parses. Field names are the contract; do not rename them without changing `analyse.py` in the same commit.

- [ ] **Step 1: Declare the aggregate**

In `DistributedGameServer/DistributedGameServerManager.h`, in the **public** section immediately after `void SetHaloReliable(bool reliable) { mHaloReliable = reliable; }`:

```cpp
			// Per-host ENet totals, split by what each host family carries. The
			// sender server talks to CLIENTS (snapshots, spawns, acks); the peer
			// links talk to other SERVERS (halo, handoffs). E8 needs exactly that
			// split, and here it falls out of the topology instead of being
			// attributed after the fact.
			struct NetworkByteTotals {
				unsigned int clientBytes = 0;
				unsigned int clientPackets = 0;
				unsigned int peerBytes = 0;
				unsigned int peerPackets = 0;
			};
			NetworkByteTotals GetNetworkByteTotals() const;
```

- [ ] **Step 2: Implement the aggregate**

In `DistributedGameServer/DistributedGameServerManager.cpp`, add beside the other accessors (near `HasPeerLink`, around `:894`):

```cpp
NCL::DistributedGameServer::DistributedGameServerManager::NetworkByteTotals
DistributedGameServer::DistributedGameServerManager::GetNetworkByteTotals() const {
	NetworkByteTotals totals;

	if (mDistributedPacketSenderServer != nullptr) {
		totals.clientBytes = mDistributedPacketSenderServer->GetTotalSentData();
		totals.clientPackets = mDistributedPacketSenderServer->GetTotalSentPackets();
	}

	// Summed, not per-peer: the claim under test is this server's total
	// server-to-server cost, and a per-link breakdown would not change it.
	for (const auto* connection : mDistributedPhysicsClients) {
		if (connection == nullptr || connection->client == nullptr) {
			continue;
		}
		totals.peerBytes += connection->client->GetTotalSentData();
		totals.peerPackets += connection->client->GetTotalSentPackets();
	}

	return totals;
}
```

- [ ] **Step 3: Emit the fields**

In `DistributedGameServer/ServerStarter.cpp`, add after the `if (auto* worldManager = ...) { ... }` capture block that ends at `:272`:

```cpp
		// Read after the world-manager block for the same reason that block exists:
		// a server that never started still reaches this line, and must report
		// zeroes rather than crash.
		const auto netTotals = serverManager->GetNetworkByteTotals();
```

Then in the `@@FINAL` stream, immediately after the `snapSupp` field and before the closing `<< "\n";`:

```cpp
			// Real wire cost, not a model. ENet counts these after coalescing
			// commands into datagrams, so E8 no longer has to assume one datagram
			// per packet. Split by host family: client-facing carries snapshots,
			// peer-facing carries halo and handoffs. Header overhead is NOT added
			// here - analyse.py owns that arithmetic, because which headers to
			// charge is an analysis choice rather than a runtime fact.
			<< " netCliBytes=" << netTotals.clientBytes
			<< " netCliPkts=" << netTotals.clientPackets
			<< " netPeerBytes=" << netTotals.peerBytes
			<< " netPeerPkts=" << netTotals.peerPackets
```

- [ ] **Step 4: Build all three server roles**

```powershell
msbuild DistributedPhysicsSystem.sln /t:EntryPointManager;EntryPointMidware;EntryPointServer /p:Configuration=Debug /p:Platform=x64
```

Expected: all three link with no errors.

- [ ] **Step 5: Verify the fields appear and are non-zero**

There is no unit test for this — it needs live hosts. Verify by running.

```powershell
powershell -ExecutionPolicy Bypass -File tools\build-deploy.ps1 -Config Release
powershell -ExecutionPolicy Bypass -File tools\run-experiments.ps1 `
    -Name netcounters-smoke -Sweep ticks -Values "600" -Repeats 1 `
    -Servers 2 -Objects 400 -Workload uniform -Seed 42 -HaloWidth 8 -HaloReliable
Select-String -Path runs\exp-netcounters-smoke\ticks600-r1\mid.log -Pattern "@@FINAL role=server" | ForEach-Object { $_.Line }
```

Expected: each server line carries all four fields. `netCliBytes` and `netCliPkts` must be **non-zero** (snapshots went to a client), and with `--halo-width 8` on two servers `netPeerBytes` and `netPeerPkts` must be **non-zero** too. All-zero peer figures mean the peer links were never established — a real failure, not a reporting one.

- [ ] **Step 6: Sanity-check the ratio before trusting it**

```powershell
Select-String -Path runs\exp-netcounters-smoke\ticks600-r1\mid.log -Pattern "@@FINAL role=server" | ForEach-Object {
    if ($_.Line -match "netCliBytes=(\d+).*netCliPkts=(\d+)") {
        "mean datagram payload: {0:N1} bytes" -f ([double]$Matches[1] / [double]$Matches[2])
    }
}
```

Expected: a mean between roughly 20 B and the MTU (~1400 B). A value below ~20 B means the counters are being read from the wrong host; a value above the MTU means they are not per-datagram at all. Either invalidates the whole approach — stop and re-check `enet/protocol.c:1732` before continuing.

- [ ] **Step 7: Commit**

```bash
git add DistributedGameServer/DistributedGameServerManager.h DistributedGameServer/DistributedGameServerManager.cpp DistributedGameServer/ServerStarter.cpp
git commit -m "feat(server): report per-host wire bytes and datagrams in @@FINAL

Splits by host family, which is the split E8 needs: the sender server
carries snapshots to clients, the peer links carry halo and handoffs to
other servers. No header overhead is added here - analyse.py owns that,
because which headers to charge is an analysis choice, not a fact about
the run."
```

---

### Task 3: Teach analyse.py the wire-byte arithmetic

**Files:**
- Modify: `tools/analyse.py`
- Modify: `tools/test_analyse.py`

**Interfaces:**
- Consumes: the `netCliBytes` / `netCliPkts` / `netPeerBytes` / `netPeerPkts` fields from Task 2.
- Produces: `analyse.wire_bytes(sent_data, sent_packets, header_bytes=28) -> int`, and the run-summary keys `net_cli_wire_bytes` and `net_peer_wire_bytes`. Task 9 quotes both.

- [ ] **Step 1: Write the failing tests**

Add to `tools/test_analyse.py`, after the `PredictedHaloFloorTests` class:

```python
class WireBytesTests(unittest.TestCase):
    """ENet's counters are per DATAGRAM, taken after it coalesces a peer's queued
    commands. E8's published figures charged a flat 36 B per PACKET, which is only
    correct if every packet became its own datagram - and whether it did was exactly
    the open question. Charging per datagram removes the question.
    """

    def test_headers_are_charged_per_datagram(self):
        # 10 datagrams carrying 1,000 bytes of ENet-level payload between them.
        self.assertEqual(analyse.wire_bytes(1000, 10), 1000 + 280)

    def test_no_datagrams_costs_nothing(self):
        self.assertEqual(analyse.wire_bytes(0, 0), 0)

    def test_coalescing_shows_up_as_a_lower_header_charge(self):
        # Identical payload; coalesced 10:1. Only the header term moves, and it
        # moves by exactly the datagrams saved.
        uncoalesced = analyse.wire_bytes(1000, 100)
        coalesced = analyse.wire_bytes(1000, 10)
        self.assertEqual(uncoalesced - coalesced, 28 * 90)

    def test_the_enet_header_is_not_charged_twice(self):
        # totalSentData already includes ENet's own protocol header, so only IPv4
        # (20) and UDP (8) are added. If this becomes 36 someone has re-added it.
        self.assertEqual(analyse.wire_bytes(0, 1), 28)

    def test_a_negative_total_is_rejected_rather_than_summed(self):
        # Both counters are unsigned in ENet, so a negative here means the @@FINAL
        # line was misparsed - which must fail loudly, not produce a smaller number.
        with self.assertRaises(ValueError):
            analyse.wire_bytes(-1, 10)
        with self.assertRaises(ValueError):
            analyse.wire_bytes(10, -1)
```

- [ ] **Step 2: Run them to make sure they fail**

```powershell
python tools\test_analyse.py
```

Expected: FAIL — `AttributeError: module 'analyse' has no attribute 'wire_bytes'`.

- [ ] **Step 3: Write the minimal implementation**

In `tools/analyse.py`, add near `predicted_halo_floor` (around `:50`):

```python
# IPv4 (20) + UDP (8). ENet's own protocol header is already inside totalSentData,
# which counts what enet_socket_send actually wrote, so charging 36 here would
# double-count it.
IP_UDP_HEADER_BYTES = 28


def wire_bytes(sent_data, sent_packets, header_bytes=IP_UDP_HEADER_BYTES):
    """Real bytes on the wire, from ENet's post-coalescing per-host counters.

    E8's published figures modelled this as payload + 36 B per PACKET, which holds
    only if every packet became its own datagram. These counters are per DATAGRAM,
    measured after ENet coalesces a peer's queued commands, so no model is needed -
    and the difference between the two is the size of the coalescing effect.
    """
    if sent_data < 0 or sent_packets < 0:
        raise ValueError(
            "ENet totals are unsigned; a negative means the @@FINAL line misparsed"
        )
    return sent_data + header_bytes * sent_packets
```

- [ ] **Step 4: Run the tests and make sure they pass**

```powershell
python tools\test_analyse.py
```

Expected: OK, all tests passing.

- [ ] **Step 5: Surface the totals in the run summary**

In `tools/analyse.py`, inside `summarise_run`, in the `if server_finals:` block after the `invariants["conservation_delta"] = ...` line:

```python
        # Measured wire cost, summed across servers. Deliberately a total rather
        # than a rate: the rate depends on which seconds of the run you count, and
        # E8's own figures are quoted against its configured 20 s window, so the
        # division belongs in the write-up next to that window, not here.
        invariants["net_cli_wire_bytes"] = wire_bytes(
            total(server_finals, "netCliBytes"), total(server_finals, "netCliPkts")
        )
        invariants["net_peer_wire_bytes"] = wire_bytes(
            total(server_finals, "netPeerBytes"), total(server_finals, "netPeerPkts")
        )
```

- [ ] **Step 6: Verify against the Task 2 smoke run**

```powershell
python tools\analyse.py runs\exp-netcounters-smoke
```

Expected: the summary reports `net_cli_wire_bytes` and `net_peer_wire_bytes`, both non-zero, and each strictly greater than the corresponding raw `netCliBytes` / `netPeerBytes` sum (because headers were added). Exit code unchanged from before the edit.

- [ ] **Step 7: Commit**

```bash
git add tools/analyse.py tools/test_analyse.py
git commit -m "feat(tools): compute wire bytes from ENet's per-datagram counters

Charges IPv4+UDP per datagram rather than 36 B per packet. ENet's own
header is already inside totalSentData, so adding 36 would double-count
it - a test pins that at 28.

Reported as totals, not rates: the rate depends on which window you
count, and E8 quotes its figures against a configured 20 s window."
```

---

### Task 4: Read every client's @@FINAL, and only the real ones

**Files:**
- Modify: `tools/analyse.py:271-289` (`read_final_lines`)
- Modify: `tools/test_analyse.py`

**Interfaces:**
- Consumes: nothing from earlier tasks.
- Produces: `read_final_lines` reading `cli.log` and `cli-<N>.log` for integer N, and never `cli-late.log`. Task 5 writes files matching that contract.

- [ ] **Step 1: Write the failing tests**

Add to `tools/test_analyse.py`:

```python
import shutil
import tempfile


class ClientLogDiscoveryTests(unittest.TestCase):
    """Multi-client runs write one log per client. The late joiner must stay out.

    measure.ps1's -LateClientAfter client exists to exercise the join path, not to
    carry load: it runs for 10 s regardless of the run length. Counting its @@FINAL
    line would inflate the client-side total that invariant I4 balances against what
    the servers applied.
    """

    def setUp(self):
        self.directory = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.directory, ignore_errors=True)

    def _write(self, name, text):
        with open(os.path.join(self.directory, name), "w") as handle:
            handle.write(text)

    def _clients(self):
        return [
            f for f in analyse.read_final_lines(self.directory)
            if f["role"] == "client"
        ]

    def test_every_numbered_client_log_is_read(self):
        self._write("cli-0.log", "@@FINAL role=client cmdSent=10\n")
        self._write("cli-1.log", "@@FINAL role=client cmdSent=7\n")
        clients = self._clients()
        self.assertEqual(len(clients), 2)
        self.assertEqual(sum(int(f["cmdSent"]) for f in clients), 17)

    def test_the_late_joiner_is_not_counted(self):
        self._write("cli-0.log", "@@FINAL role=client cmdSent=10\n")
        self._write("cli-late.log", "@@FINAL role=client cmdSent=99\n")
        clients = self._clients()
        self.assertEqual(len(clients), 1)
        self.assertEqual(int(clients[0]["cmdSent"]), 10)

    def test_the_single_client_name_is_still_read(self):
        self._write("cli.log", "@@FINAL role=client cmdSent=4\n")
        self.assertEqual(len(self._clients()), 1)

    def test_a_missing_run_directory_is_not_an_error(self):
        self.assertEqual(analyse.read_final_lines(os.path.join(self.directory, "nope")), [])
```

- [ ] **Step 2: Run them to make sure they fail**

```powershell
python tools\test_analyse.py
```

Expected: `test_every_numbered_client_log_is_read` FAILS (0 clients found — only `cli.log` is read today). `test_the_late_joiner_is_not_counted` passes already; that is fine, it is a regression guard for this change.

- [ ] **Step 3: Write the minimal implementation**

In `tools/analyse.py`, add above `read_final_lines`:

```python
# cli.log (single client) or cli-0.log, cli-1.log, ... (multi-client).
#
# Deliberately NOT a cli*.log glob: that would also match cli-late.log, the
# short-lived join-path client, whose totals must not enter the I4 tally.
CLIENT_LOG_PATTERN = re.compile(r"^cli(-\d+)?\.log$")
```

Then replace the `for name in ("mid.log", "cli.log"):` line with:

```python
    client_logs = []
    if os.path.isdir(run_dir):
        client_logs = sorted(
            entry for entry in os.listdir(run_dir) if CLIENT_LOG_PATTERN.match(entry)
        )

    for name in ["mid.log"] + client_logs:
```

- [ ] **Step 4: Run the tests and make sure they pass**

```powershell
python tools\test_analyse.py
```

Expected: OK, all tests passing, including the pre-existing ones.

- [ ] **Step 5: Verify no regression on a real run**

```powershell
python tools\analyse.py runs\exp-phaseA-baseline
```

Expected: identical output to Task 0 Step 4. The baseline has one `cli.log`, which the new pattern still matches.

- [ ] **Step 6: Commit**

```bash
git add tools/analyse.py tools/test_analyse.py
git commit -m "feat(tools): read one @@FINAL per client, excluding the late joiner

Matches cli.log and cli-<N>.log by pattern rather than globbing cli*.log,
because the glob would also pick up cli-late.log - the 10-second
join-path client, whose totals would inflate the client side of the I4
command tally."
```

---

### Task 5: Run more than one client

**Files:**
- Modify: `tools/measure.ps1` — param block (`:10-91`), manager arguments (`:148`), manifest (`:126`), client launch (`:182-184`), cleanup (`:231`)
- Modify: `tools/run-experiments.ps1` — param block, manifest (`:138`), the `measure.ps1` invocation (`:215`)

**Interfaces:**
- Consumes: the `cli-<N>.log` naming contract from Task 4.
- Produces: `-Clients N` on both scripts. Task 9 sweeps interest radius at `-Clients 1` and `-Clients 2`.

- [ ] **Step 1: Add the parameter**

In `tools/measure.ps1`, add to the `param(...)` block after `[int]$LateClientAfter = 0,`:

```powershell
    # How many clients to start. E8's client-count argument - that the interest
    # saving scales with clients while the halo cost does not - stays analytical
    # until this is greater than 1, because snapshots are counted per object PER
    # CLIENT and halo traffic is not.
    [int]$Clients = 1,
```

- [ ] **Step 2: Tell the manager how many to expect**

In `tools/measure.ps1:148`, change `--clients 1` to `--clients $Clients`:

```powershell
    -ArgumentList "--servers $Servers --clients $Clients --objects $Objects --port 1234 --world $World --midwares 1 --autostart --headless --rebalance-alpha $RebalanceAlpha --rebalance-threshold $RebalanceThreshold $repartitionArgs" `
```

This is not cosmetic: the manager gates game-server startup on its expected client count, so a mismatch leaves servers waiting for a peer that never arrives.

- [ ] **Step 3: Record it in the manifest**

In `tools/measure.ps1`, in the manifest hashtable beside `objects = $Objects`:

```powershell
    clients      = $Clients
```

- [ ] **Step 4: Start N clients with per-client logs**

Replace the single client `Start-Process` at `tools/measure.ps1:182-184` with:

```powershell
# One log per client. analyse.py matches cli-<N>.log and deliberately does not
# match cli-late.log, so the late joiner below stays out of the I4 tally.
$cliProcs = @()
for ($i = 0; $i -lt $Clients; $i++) {
    $cliProcs += Start-Process -PassThru -FilePath (Join-Path $deploy "Client\EntryPoint.exe") `
        -ArgumentList "--manager-ip 127.0.0.1 --manager-port 1234 --headless --impulse-test $ImpulseTest --misroute-every $MisrouteEvery --blast-every $BlastEvery --spawn-every $SpawnEvery --blast-offset-x $BlastOffsetX --destroy-every $DestroyEvery --drive-every $DriveEvery --interest-radius $InterestRadius --run-seconds $clientSeconds" `
        -WorkingDirectory $deploy -RedirectStandardOutput "$runDir\cli-$i.log" -RedirectStandardError "$runDir\cli-$i.err" -WindowStyle Hidden
}
```

- [ ] **Step 5: Wait on all of them**

At `tools/measure.ps1:231`, replace `foreach ($p in @($cli, $mid, $mgr)) {` with:

```powershell
foreach ($p in @($cliProcs) + @($mid, $mgr)) {
```

- [ ] **Step 6: Pass it through the experiment runner**

In `tools/run-experiments.ps1`, add to the `param(...)` block beside `[double]$InterestRadius = 0,`:

```powershell
    # Clients per run. See measure.ps1 -Clients.
    [int]$Clients = 1,
```

Add to its manifest hashtable beside `interestRadius = $InterestRadius`:

```powershell
        clients = $Clients
```

And add `-Clients $Clients` to the `measure.ps1` invocation at `:215`, alongside `-HandoffLookahead $HandoffLookahead`.

- [ ] **Step 7: Verify one client still behaves exactly as before**

```powershell
powershell -ExecutionPolicy Bypass -File tools\run-experiments.ps1 `
    -Name clients-1 -Sweep ticks -Values "600" -Repeats 1 `
    -Servers 2 -Objects 400 -Workload uniform -Seed 42 -HaloWidth 8 -HaloReliable
python tools\analyse.py runs\exp-clients-1
```

Expected: `runs/exp-clients-1/ticks600-r1/cli-0.log` exists (not `cli.log`), analyse.py finds exactly one client, invariants pass.

- [ ] **Step 8: Verify two clients**

```powershell
powershell -ExecutionPolicy Bypass -File tools\run-experiments.ps1 `
    -Name clients-2 -Sweep ticks -Values "600" -Repeats 1 `
    -Servers 2 -Objects 400 -Workload uniform -Seed 42 -HaloWidth 8 -HaloReliable -Clients 2
python tools\analyse.py runs\exp-clients-2
```

Expected: `cli-0.log` and `cli-1.log` both present, both carrying an `@@FINAL role=client` line; invariants pass. Compare the two runs' `netCliBytes`: the two-client run must be substantially **higher**, since snapshots are sent per client. If it is not, the second client never actually received snapshots and the bootstrap is at fault — investigate before continuing, because this is the measurement Task 9 depends on.

- [ ] **Step 9: Commit**

```bash
git add tools/measure.ps1 tools/run-experiments.ps1
git commit -m "feat(tools): -Clients N in the measurement harness

The manager's expected client count is what gates game-server startup,
so it is passed through rather than left at 1. Each client writes
cli-<N>.log, which is the naming analyse.py matches.

E8's client-count argument was analytical because the harness could not
produce the measurement; this is what makes it measurable."
```

---

### Task 6: The Phase A gate — bound what could have changed

Everything above claims to be measurement-only. This task turns that claim into a
measurement — but not the one this plan originally specified, and the reason matters.

**The original gate does not work, and was replaced before Task 6 ran.** It required
the post-change `@@FINAL` lines to compare identical to the baseline's. Measured on
this machine with **four clean runs at the identical commit, seed and configuration**,
that never happens:

| behaviour across 4 identical clean runs | fields |
|---|---|
| **identical every time** | the 21 zero-valued or tick-locked counters: `cmdApplied` `cmdRelayed` `cmdDup` `cmdRejected` `cmdFanout` `hoFail` `hoLate` `hoResent` `hoReclaimed` `hoCustody` `hoDup` `hoPending` `hoSched` `haloLate` `haloAhead` `haloSent` `haloRecv` `manifestSent` `objPreseed` `objSpawned` `objDestroyed` |
| **per-server varies, TOTAL stable at 400** | `objs`, `objPool` — observed splits 201/199, 200/200, 200/200, 201/199 |
| **varies, total varies** | `contacts` (~1%), `snapSent` (~1.5%), `haloObjSent`/`haloObjRecv` (~10%), `hoSent`/`hoRecv` (±1), `objFwd`, `objHalo`, `objWorld`, `hoClamp` |

`ownership_gap_ticks` across those four runs: 84, 82, 91, 84. A fifth run degraded
badly — 1747 ms and 1049 ms frame times in its opening windows against an 8.33 ms
budget — which cascaded into custody firing, `haloLate` 9,530, `ownership_gap_ticks`
1,779 and two ticks of double ownership. `analyse.py` flagged it with a
REPRODUCIBILITY WARNING, so a degraded run is detectable; but its `@@FINAL` values
bore no resemblance to a clean run's.

So an exact comparison would have failed 100% of the time with zero code change. The
gate below is weaker than "identical" because nothing stronger is available on this
machine — not because a weaker check was more convenient.

**Files:**
- Modify: `docs/superpowers/results/2026-08-23-A-instrumentation.md`

**Interfaces:**
- Consumes: the Task 0 baseline experiment at `runs/exp-phaseA-baseline/`.
- Produces: a pass/fail verdict. **No Phase A result may be reported before this passes.**

- [ ] **Step 1: Take three post-change repeats of the baseline configuration**

Three, not one: the varying fields can only be compared as ranges, and a single run
cannot establish a range.

```powershell
powershell -ExecutionPolicy Bypass -File tools\build-deploy.ps1 -Config Release
powershell -ExecutionPolicy Bypass -File tools\run-experiments.ps1 `
    -Name phaseA-gate -Sweep ticks -Values "1800" -Repeats 3 `
    -Servers 2 -Objects 400 -Workload uniform `
    -Seed 42 -HaloWidth 8 -HaloReliable -DrainSeconds 5
```

Every parameter except `-Repeats` must match Task 0 Step 3. `-Clients` is deliberately
omitted so it takes its default of 1, matching the baseline.

- [ ] **Step 2: Establish validity — a degraded run is not a comparand**

```powershell
python tools\analyse.py runs\exp-phaseA-gate
```

Read the output for a `REPRODUCIBILITY WARNING`. Any repeat that reports custody
firing is a **failed measurement, not a failed gate** — discard it and re-run that
repeat until you have three clean ones. `ownership_gap_ticks` in the 80–95 band is
expected (the documented `--handoff-lookahead 0` gap, see below); a value in the
thousands means the run degraded and must be discarded on the same grounds.

Only once you hold three clean repeats does the comparison below mean anything.

- [ ] **Step 3: Require exact equality on the stable set**

```powershell
python tools\gate-compare.py runs\exp-phaseA-baseline runs\exp-phaseA-repro3 runs\exp-phaseA-gate
```

Write `tools/gate-compare.py` as part of this task:

```python
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
STABLE = [
    "cmdApplied", "cmdRelayed", "cmdDup", "cmdRejected", "cmdFanout",
    "hoFail", "hoLate", "hoResent", "hoReclaimed", "hoCustody", "hoDup",
    "hoPending", "hoSched", "haloLate", "haloAhead", "haloSent", "haloRecv",
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
```

Expected: `GATE PASSED`. A failure here names the field that moved, which is the
actual signal — a Phase A change that altered simulation behaviour would show up as a
failure counter leaving zero, or as conservation breaking.

- [ ] **Step 4: Check the varying fields sit inside their pre-change spread**

The stable set cannot see a change that only perturbs the noisy fields, so bound them
by hand. Pre-change spread, from four clean runs (`runs/exp-phaseA-baseline` plus the
three in `runs/exp-phaseA-repro3`):

| field | pre-change range across clean runs |
|---|---|
| `contacts` (per server) | 171,536 – 173,921 |
| `snapSent` (per server) | 246,833 – 251,275 |
| `haloObjSent` (per server) | 16,149 – 20,186 |
| `hoSent` + `hoRecv` (both servers) | 78 – 80 |
| `objHalo` (per server) | 7 – 14 |
| `hoClamp` (server 0) | 7 – 8 |

Read the same fields from the three gate repeats. Each must fall inside — or
negligibly outside — its band. Record any that do not, with the actual value; a field
that moves an order of magnitude is a real signal even though a field that moves 2% is
not.

- [ ] **Step 5: State the structural argument the numbers cannot make**

The measurement above bounds the change; it cannot prove it is zero. Confirm by
inspection, and record the file and line:

- `DistributedGameServerManager::GetNetworkByteTotals()` is called **once**, from
  `ServerStarter.cpp`, after the run loop has exited and before the `@@FINAL` line.
- It performs only reads: the sender host's two counters plus a loop over peer links.
- Nothing on the tick path calls it, and no counter is reset.

If any of those three is false, the gate does not hold regardless of what the numbers
say.

- [ ] **Step 6: Record the gate result honestly**

Append to `docs/superpowers/results/2026-08-23-A-instrumentation.md`:

```markdown
## The no-op gate, and why it is not an equality check

Phase A claims to be measurement-only. The obvious verification — re-run the baseline
configuration and require every `@@FINAL` field to match — **is not available on this
machine.** Four clean runs at the identical commit, seed and configuration disagree on
`contacts` (~1%), `snapSent` (~1.5%), halo object counts (~10%), the `hoSent`/`hoRecv`
split (±1), and the per-server object split (201/199 vs 200/200). `ownership_gap_ticks`
across those four runs was 84, 82, 91, 84.

This contradicts `CLAUDE.md`'s claim that under `--run-ticks --fixed-step` "end state
and conservation then reproduce exactly". Conservation does reproduce — the object
total is 400 on every run. End state does not.

What the gate checks instead:

| check | result |
|---|---|
| 21 stable counters identical across all runs, both sides | `<pass/fail>` |
| Object conservation total unchanged (400) | `<pass/fail>` |
| Varying fields inside their pre-change spread | `<pass/fail, with any exceptions>` |
| Counter read is once-at-exit, off the tick path | `<confirmed at ServerStarter.cpp:NNN>` |

**What this does and does not establish.** It rules out a Phase A change that breaks
conservation, that trips any failure counter, or that shifts a noisy field beyond its
natural spread. It cannot rule out a change that perturbs those fields within that
spread. That is a weaker claim than the plan originally intended, and it is stated
here rather than papered over.
```

- [ ] **Step 7: Commit**

```bash
git add tools/gate-compare.py docs/superpowers/results/2026-08-23-A-instrumentation.md
git commit -m "test(A): bound Phase A's effect, since exact comparison is unavailable

Four clean runs at the same commit, seed and configuration disagree on
contacts, snapshot counts, halo object counts and the handoff split, so
the planned equality gate would have failed with zero code change.

Gates instead on what is actually stable: 21 counters that hold their
value on every clean run, and object conservation. The noisy fields are
bounded by their measured pre-change spread, and the once-at-exit
structure of the counter read is recorded as the part the numbers
cannot establish.

Also contradicts CLAUDE.md's 'end state and conservation then reproduce
exactly' - conservation does, end state does not."
```

---

### Task 7: Measure whether tick-epoch alignment tightens reproducibility

`--epoch-align-us` has existed since 2026-08-17, defaults to 0, and no experiment has ever set it. No code is needed — this is a run and a documentation fix.

**Files:**
- Modify: `CLAUDE.md` (the game-server row of the flags table)
- Modify: `docs/superpowers/results/2026-08-23-A-instrumentation.md`

**Interfaces:**
- Consumes: nothing.
- Produces: a decision on whether subsequent phases enable epoch alignment.

- [ ] **Step 1: Take three repeats without alignment**

```powershell
powershell -ExecutionPolicy Bypass -File tools\run-experiments.ps1 `
    -Name epoch-off -Sweep ticks -Values "1800" -Repeats 3 `
    -Servers 2 -Objects 400 -Workload uniform -Seed 42 -HaloWidth 8 -HaloReliable
```

- [ ] **Step 2: Take three repeats with alignment**

The alignment quantum is 100 ms — comfortably longer than the few-millisecond game-start spread it exists to absorb, and short enough not to stall the run.

```powershell
powershell -ExecutionPolicy Bypass -File tools\run-experiments.ps1 `
    -Name epoch-on -Sweep ticks -Values "1800" -Repeats 3 `
    -Servers 2 -Objects 400 -Workload uniform -Seed 42 -HaloWidth 8 -HaloReliable `
    -EpochAlignUs 100000
```

- [ ] **Step 3: Confirm alignment actually engaged**

```powershell
Select-String -Path runs\exp-epoch-on\*\mid.log -Pattern "Tick epoch aligned to"
```

Expected: one line per server per run. If absent, the flag did not reach the servers — check that the midware forwarded it (`PhysicsServerMidware/ProgramStart.cpp:86-87`), and do not interpret the comparison until it did.

- [ ] **Step 4: Compare handoff-event spread across repeats**

```powershell
python tools\analyse.py runs\exp-epoch-off
python tools\analyse.py runs\exp-epoch-on
```

Compare the spread of `hoSent` and `hoRecv` across the three repeats in each. The documented symptom is that these vary by ±1 at the same seed.

- [ ] **Step 5: Record the finding either way**

Append to the results document. **A null result is a result here** — it removes a suspected cause and is worth the same three runs.

```markdown
## Tick-epoch alignment

`--epoch-align-us` (`HeadlessRunner.cpp:88-97`) spins every server's tick 0 onto a
shared monotonic boundary. It has existed since 2026-08-17 — before the E1–E8
measurement pass — defaults to 0, and no experiment document sets it, so the
published runs were taken without it. Both handoff and halo scheduling are
expressed in the *sender's* tick numbers, which is why it was a candidate cause of
the ±1 handoff-event variance.

| configuration | hoSent across 3 repeats | hoRecv across 3 repeats |
|---|---|---|
| `--epoch-align-us 0` | `<values>` | `<values>` |
| `--epoch-align-us 100000` | `<values>` | `<values>` |

**Verdict:** `<tightens / does not tighten>`. `<If it tightens: enabled for every
reproducible run from Phase B onward. If not: the ±1 variance has another cause,
and the alignment flag is not it.>`

Note this addresses only the *start-of-run* offset. The drift half of tick-epoch
divergence — servers falling behind their pacing budget mid-run — is untouched and
remains as documented in `docs/EVALUATION.md` §5.
```

- [ ] **Step 6: Add the flag to CLAUDE.md**

In the flags table, the Game Server row currently ends `--rebalance-interval N`, `--drain-seconds N`. Add `--epoch-align-us N` to that list, and note beneath the table:

```markdown
> `--epoch-align-us N` makes every server spin at tick 0 to the next N-microsecond
> boundary on the monotonic clock, so all servers start their tick counters on the
> same instant. The game-start broadcast arrives with a few milliseconds of spread,
> which at 120 Hz is enough to shift epochs by a tick — and both handoff and halo
> scheduling are expressed in the sender's tick numbers. It defaults to 0 (off), and
> every E1–E8 measurement was taken that way.
```

- [ ] **Step 7: Record the standing decision in the experiment suite**

The results document records what was measured; the experiment suite is what the
next phase reads before configuring a run. Add to
`docs/superpowers/specs/2026-08-19-experiment-suite.md`, in the section describing
the shared run configuration:

```markdown
**Tick-epoch alignment.** Reproducible runs `<pass --epoch-align-us 100000 /
leave --epoch-align-us at 0>`, measured in
`docs/superpowers/results/2026-08-23-A-instrumentation.md`. Every E1–E8 figure was
taken with it off, so a run that enables it is not directly comparable to those
without re-measuring the baseline.
```

Fill the bracketed choice from Task 7 Step 5's verdict. The second sentence stands
either way and is the part that matters for the next phase.

- [ ] **Step 8: Commit**

```bash
git add CLAUDE.md docs/superpowers/specs/2026-08-19-experiment-suite.md docs/superpowers/results/2026-08-23-A-instrumentation.md
git commit -m "docs(A): measure tick-epoch alignment, and document the flag

--epoch-align-us has existed since before the E1-E8 pass, defaults to
off, and no experiment set it - so it was never in the flags table
either. Measured against the handoff-event variance it was a candidate
explanation for, and recorded either way."
```

---

### Task 8: Correct three stale claims, measured false during this phase

Two are stale independently of anything Phase A changes. The third was **measured false by
this phase's own runs**, which is why it lands here rather than in the spec's original list.

**Files:**
- Modify: `docs/SPATIAL-PARTITIONING.md:7`, `:50`, and the summary table row
- Modify: `CLAUDE.md:218` (the reproducibility claim — see Step 6 below)

**Interfaces:**
- Consumes: nothing. Produces: nothing. Purely documentation.

- [ ] **Step 1: Verify both claims are still stale before editing**

```powershell
Select-String -Path docs\SPATIAL-PARTITIONING.md -Pattern "150|scaffolded|TODO"
Select-String -Path tools\measure.ps1 -Pattern '\$World = '
```

Expected: the doc asserts a fixed ±150 world, while `measure.ps1` takes `-World` as a parameter. Confirm before changing — if someone has already fixed it, skip this task rather than editing blind.

- [ ] **Step 2: Correct the world-bounds claim**

Replace the fixed-square sentence at `docs/SPATIAL-PARTITIONING.md:7`:

```markdown
The simulated world is a square on the X/Z plane whose extent is set at runtime by the manager's `--world minX,maxX,minZ,maxZ` flag (default `-150,150,-150,150`, parsed in `DistributedPhysicsManager/ProgramStart.cpp`). It was formerly a fixed ±150 square, and experiments that grow the world with the server count — E1's locality sweep — depend on it no longer being fixed. The manager divides this square into one rectangular **region per server**.
```

And in the summary table, change the `World bounds (±150 X/Z)` row to:

```markdown
| World bounds (`--world`, default ±150 X/Z) | `DistributedPhysicsManager/ProgramStart.cpp` |
```

- [ ] **Step 3: Correct the acknowledgement claim**

Replace the "**4 — Acknowledgement (scaffolded).**" paragraph at `:50` with:

```markdown
**4 — Acknowledgement (live).** The receiver acknowledges on acceptance, and the sender holds the transfer packet in custody until that acknowledgement arrives, resending it on a wall-clock deadline (`CSC8503CoreClasses/DistributedSystemCommonFiles/HandoffCustody.h`, `ServerWorldManager::FlushPendingTransfers`). It reclaims the object only once the peer link itself is gone — never on a bare timeout, because a timeout cannot distinguish "the receiver never got it" from "the receiver got it and is slow". An outstanding transfer is visible as `hoCustody` rather than silently lost.

This paragraph previously described the path as scaffolded, with the ack a `TODO` and the receive handler commented out. That was true before the handoff-custody work; see `docs/superpowers/results/2026-08-20-B-custody.md`.

**Note this does not close the ownership gap.** Custody makes an individual transfer lossless; it does not make ownership transfer atomic. At `--handoff-lookahead 0`, the default, the sender still releases on send and nobody owns the object for one network round trip. See `docs/EVALUATION.md` §6.
```

- [ ] **Step 4: Check nothing else in the file repeats either claim**

```powershell
Select-String -Path docs\SPATIAL-PARTITIONING.md -Pattern "150|scaffold|stub|TODO"
```

Expected: only the corrected text and the summary-table row. Any other hit is another instance of the same staleness — fix it in this commit.

- [ ] **Step 5: Correct the reproducibility claim in CLAUDE.md**

`CLAUDE.md:218` currently reads, of `--run-ticks` with `--fixed-step`:

> End state and conservation then reproduce exactly; handoff *event* counts still vary by ±1,
> which would need a global tick barrier to remove.

Both halves are contradicted by this phase's measurements. Replace that sentence with:

```markdown
Conservation then reproduces exactly — the world total is identical on every run — but **end state does not**. Measured 2026-08-23 across four clean runs at an identical commit, seed and configuration: `contacts` varied ~1%, `snapSent` ~1.5%, halo object counts ~10%, and the per-server object split moved between 201/199 and 200/200. Handoff *event* counts vary by more than the ±1 previously claimed here: `hoSent` ranged 78–83 across six runs. A set of counters IS stable across clean runs and is what `tools/gate-compare.py` pins; see `docs/superpowers/specs/2026-08-23-backlog-completion-design.md` §1.3 for the full field-by-field breakdown and for what a verification gate can therefore check.
```

Keep the rest of that paragraph — the `--run-seconds` description and the closing
"use `--run-ticks` for correctness/conservation experiments" guidance — unchanged. The
guidance is still right; only the strength of the reproducibility claim was wrong.

- [ ] **Step 6: Commit**

```bash
git add docs/SPATIAL-PARTITIONING.md CLAUDE.md
git commit -m "docs: correct three stale claims, two long-standing and one measured false

World bounds have been a --world flag since E1's locality sweep needed
to grow the world with the server count; the doc still described a fixed
+/-150 square. The handoff acknowledgement was described as scaffolded
with the ack a TODO, which the custody work made false.

Landed in Phase A because it has no blast radius - the cheapest place to
put a prose fix."
```

---

### Task 9: Re-run E8 and E3, and update the evaluation

The deliverable of the phase. **Do not start until Task 6's gate has passed.**

**Files:**
- Modify: `docs/EVALUATION.md` — §3 E3, §3 E8, §7 items 10 and 11
- Modify: `docs/superpowers/results/2026-08-19-E8-bandwidth.md`
- Modify: `docs/superpowers/results/2026-08-23-A-instrumentation.md`

**Interfaces:**
- Consumes: `net_cli_wire_bytes` / `net_peer_wire_bytes` from Task 3, `-Clients` from Task 5.
- Produces: E8's settled verdict and E3's quotable absolute counts.

- [ ] **Step 1: Re-run E8 at one client**

E8's published configuration: 2 servers, 4,000 objects, `uniform`, 20 s realtime, 3 repeats, `--halo-width 8`, halo **unreliable** (deployment-realistic), drain 0.

```powershell
powershell -ExecutionPolicy Bypass -File tools\run-experiments.ps1 `
    -Name bytes-1client -Sweep interestRadius -Values "0,25,50,100" -Repeats 3 `
    -Servers 2 -Objects 4000 -Workload uniform -Seconds 20 `
    -HaloWidth 8 -DrainSeconds 0 -Clients 1
```

- [ ] **Step 2: Re-run E8 at two clients — with the world held constant**

**`--objects N` is per client, not per world.** `ServerWorldManager::CreatePlayerObjects`
creates N objects for *each* connected client; measured, `objPreseed` reads 400 with one
client and 800 with two. Every experiment in the suite to date used a single client, so
the distinction never mattered — here it decides whether the result means anything.

Passing `-Objects 4000 -Clients 2` would compare 4,000 objects at one client against
**8,000 objects at two**, conflating client scaling with world scaling. It would also
appear to confirm the hypothesis, because both effects push snapshot traffic the same
way. Halve the per-client count instead, so both runs simulate 4,000 objects:

```powershell
powershell -ExecutionPolicy Bypass -File tools\run-experiments.ps1 `
    -Name bytes-2client -Sweep interestRadius -Values "0,25,50,100" -Repeats 3 `
    -Servers 2 -Objects 2000 -Workload uniform -Seconds 20 `
    -HaloWidth 8 -DrainSeconds 0 -Clients 2
```

**Confirm before analysing** that both experiments report the same world size, or the
comparison is void:

```powershell
Select-String -Path runs\exp-bytes-1client\*\mid.log,runs\exp-bytes-2client\*\mid.log -Pattern "objPreseed=\d+" | ForEach-Object { ($_.Line -replace '.*(objPreseed=\d+).*','$1') } | Sort-Object -Unique
```

Expected: `objPreseed=4000` from both. Anything else means the world was not held
constant and the client-count result must not be reported.

- [ ] **Step 3: Analyse both**

```powershell
python tools\analyse.py runs\exp-bytes-1client
python tools\analyse.py runs\exp-bytes-2client
```

Expected: 12 runs each, none FAILED. A run producing fewer CSVs than servers is reported as failed and must not be averaged over.

- [ ] **Step 4: Compute the ratio that settles the claim**

For each radius, the claim under test is that the halo's server-to-server cost is paid for out of interest management's server-to-client saving:

```
saving(r)  = net_cli_wire_bytes(radius 0) - net_cli_wire_bytes(r)
ratio(r)   = net_peer_wire_bytes(r) / saving(r)
```

A ratio below 1.0 means the claim holds at that radius. Compute at 25, 50 and 100 for both client counts. The measured `net_peer_wire_bytes` must be roughly **flat across radius** — server-to-server traffic does not depend on what a client asked for — and a large variation there means something other than the halo moved.

- [ ] **Step 5: Re-run E3 with the drain artefact removed**

```powershell
powershell -ExecutionPolicy Bypass -File tools\run-experiments.ps1 `
    -Name interest-clean -Sweep interestRadius -Values "0,25,50,100" -Repeats 3 `
    -Servers 2 -Objects 4000 -Workload uniform -Seconds 20 -DrainSeconds 0
python tools\analyse.py runs\exp-interest-clean
```

Expected: `snapSent` reductions in the same direction as the published 55.3 / 71.5 / 78.9 %. The published *ratios* were expected to survive the drain artefact; the *absolute* counts were not quotable. Both are now clean.

- [ ] **Step 6: Record the results**

Append to `docs/superpowers/results/2026-08-23-A-instrumentation.md`, including — this is the point of Task 3's design — the **modelled figure alongside the measured one**:

```markdown
## E8 re-measured on counted datagrams

Columns are named for the **host** they were measured on, not for the traffic
assumed to dominate it (spec §2.1): the client-facing host also carries acks,
spawns and manifest entries, and the peer-facing host also carries handoffs. On
this configuration — `uniform`, halo on, no interaction drivers — snapshots and
halo dominate their respective hosts, and the non-dominant components are bounded
by `hoSent` and `manifestSent`, which are reported alongside.

| interest radius | client-facing wire B/s | peer-facing wire B/s | saving vs r=0 | peer / saving |
|---|---|---|---|---|
| 0 | | | — | — |
| 25 | | | | |
| 50 | | | | |
| 100 | | | | |

Measured against the published figures, which charged a flat 36 B per packet:

| radius | published (modelled) | measured (counted) | ratio |
|---|---|---|---|

That last column is the size of ENet's coalescing effect, and it is the quantity
the payload-vs-datagram ambiguity was standing in for.

## The client-count argument, measured

Snapshots are counted per object per client; halo traffic is not. The published
claim that the saving therefore scales with clients while the halo cost does not
was an analytical extrapolation. Measured:

| clients | saving at r=25 | peer-facing B/s | peer / saving |
|---|---|---|---|
| 1 | | | |
| 2 | | | |
```

- [ ] **Step 7: Update EVALUATION.md**

Four edits, all in the same commit as the results document:

1. §3 E8 — replace the bandwidth table with the measured figures, and **delete the "one honest caveat is the overhead model" paragraph**, which this task exists to close. If the measured verdict differs from the published one, say so plainly rather than adjusting the framing.
2. §3 E3 — replace the table with the clean absolute counts and remove the footnote marked "contains an unverified drain-phase artefact; absolute counts not yet clean to quote" together with the caveat paragraph beneath it.
3. §7 item 10 — mark fixed, in the established `~~**...**~~ **Fixed.**` style, naming the accessors and the 28 B per-datagram charge.
4. §7 item 11 — mark fixed, naming `-Clients N`, and state the measured client-count result.

- [ ] **Step 8: Update the E8 results document**

In `docs/superpowers/results/2026-08-19-E8-bandwidth.md`, add a header note pointing at the re-measurement rather than rewriting the original: it remains the record of what was known at the time, and the project's convention is that superseded findings are marked in place, not deleted.

- [ ] **Step 9: Verify the evaluation is self-consistent**

```powershell
Select-String -Path docs\EVALUATION.md -Pattern "36 B|payload-only|not yet clean to quote|analytical extrapolation"
```

Expected: no hits outside a historical note. Each is a phrase from a caveat this task closed; a live hit means one was missed.

- [ ] **Step 10: Commit**

```bash
git add docs/EVALUATION.md docs/superpowers/results/2026-08-19-E8-bandwidth.md docs/superpowers/results/2026-08-23-A-instrumentation.md
git commit -m "docs(A): E8 settled on counted datagrams, E3 absolute counts clean

E8's verdict no longer depends on whether traffic is costed at payload
or payload-plus-headers: ENet counts bytes and datagrams after
coalescing, so the overhead model is gone rather than chosen. The
client-count argument is measured at 1 and 2 clients instead of
extrapolated from how the counters increment.

E3 re-run with --drain-seconds 0, so its absolute counts are quotable
rather than carrying an unverified drain-phase artefact.

Closes backlog items 10 and 11."
```

---

## Phase A completion checklist

- [ ] Task 6's gate passed with no differing fields
- [ ] `InteractionTests.exe` exits 0
- [ ] `python tools/test_analyse.py` exits 0
- [ ] `docs/EVALUATION.md` §7 items 10 and 11 are marked fixed
- [ ] E3's "not yet clean to quote" footnote is gone
- [ ] E8's overhead-model caveat is gone
- [ ] The epoch-alignment finding is recorded, whichever way it went
- [ ] `docs/SPATIAL-PARTITIONING.md` no longer claims a fixed ±150 world or a scaffolded ack

Phase B is next: its §3.0 pre-check may close backlog item 12 in a single run.
