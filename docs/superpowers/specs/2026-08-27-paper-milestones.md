# Paper milestones — arXiv preprint as a PhD-application lead

**Date:** 2026-08-27
**Goal:** an arXiv preprint in the first week of November 2026, usable as the lead artifact for
PhD applications in the December 2026 – January 2027 cycle.
**Context:** the MSc (CSC8599) was submitted two years ago and never published; everything the
paper leads with — invariants, gates, bounds, the lag ceiling, the ownership results, Phases A–D —
is 2026 work. The application story is *two years of sustained, self-directed research after
graduation*, and the materials should say so explicitly.

Companion documents: `2026-08-16-phd-paper-roadmap.md` (framing; §6.1b holds the prior-work
verdicts), `docs/EVALUATION.md` (the evidence), the dated results documents under
`docs/superpowers/results/`.

---

## 1. Claim order (settled by the 2026-08-27 prior-work check, roadmap §6.1b)

Lead with, in order:

1. **The total-lag ceiling** — width-independent, reached along two lag axes, generalises across
   workloads (`oblique` vs `headon`). *No prior art found.*
2. **Lookahead and latency are interchangeable poisons** — the ceiling is on the sum; raising the
   lookahead to absorb latency was tested and fails. *No prior art found; inverse structure of
   local lag.*
3. **The width/lookahead conflict** — past ~100 ms the lookahead the ownership guarantee needs
   exceeds what the halo bound permits at that width. *No prior art found; most mechanism-specific.*
4. **Ownership atomicity as measured characterisation** — conditional on the pacing budget;
   threshold behaviour (a too-small lookahead is worse than none). *Lineage: local lag (Mauve et
   al.), SpatialOS handover-timeout rule — cite, then claim the measurement.*

**The width bound itself is framing, not a contribution.** Aura Projection (Brown, Ushaw & Morgan,
ACM I3D 2019 — the same Newcastle group) publishes a closed-form aura radius with an inter-server
latency term (§3.3), validated empirically in Brown's 2021 thesis. Honest deltas that survive: the
explicit worst-case jitter term (measured 2026-08-27, binds at full weight), the scheduling
lookahead as a separate term, the pair form, and the mechanism — non-migratory dead-reckoned
shadows, where the bound then *fails* past the ceiling in a regime AP's evaluation (≤ 32 ms) never
entered. **AP must be cited prominently; a reviewer finds it in minutes.**

> Verification status: the novelty verdicts are "no prior art found in one focused literature
> pass" — absence of evidence, not proof. The AP formula and thesis validation were checked
> against the primary sources (I3D DOI 10.1145/3306131.3317021; Newcastle thesis repository).

---

## 2. Evidence gaps to close (the four gaps, as milestones)

### G1 — Baseline / ablation table  *(highest priority; the paper's evidentiary spine)*

Same binary, same seed, 6 repeats per cell, 2 servers, `uniform` (+ one `headon` column for the
contact signal):

| configuration | represents |
|---|---|
| `-Servers 1` | ground truth — no borders, correctness trivial |
| `--halo-width 0 --handoff-lookahead 0` | the naive distributed implementation |
| width 8, lookahead 0 | +cross-border collision |
| width 8, lookahead 8 | +atomic ownership |

Report per row: missed border contacts, `ownership_gap_ticks`, `hoDup`, conservation.
**Done when:** the table exists in `docs/EVALUATION.md` with every cell from one binary at one
commit. ~26 unattended runs, zero code. External baselines are explicitly out of scope; the AP
injection workload remains the single external anchor (declared-deviation form).

### G2 — Premises document

`docs/PREMISES.md`: every assumption stated as a premise **with its enforcement point and failure
signature** — `v_max = 60` as a constant of the bound (oblique guard, width-floor warning); no CCD
(the 30 u/s landing/tunnelling window); fixed 120 Hz paced dt, with the pacing budget as a stated
precondition of the ownership guarantee; POD/same-binary wire format; seeded determinism;
single-machine loopback with the claim-split (what survives it, what does not); the bound's AP
lineage. **Done when:** the file exists and EVALUATION references it. Writing only.

### G3 — Statistics discipline

1. `analyse.py`: min–max spread columns on the knee/soundness tables; a caution line when an
   ownership/duplication-sensitive experiment has < 6 repeats. Plus tests, house mutation style.
2. `docs/RUNNING-EXPERIMENTS.md`: repeats policy — 6 for gap/duplication-sensitive quantities
   (justification: measured 4-of-6 bimodality; item 15 at 1-in-3), 3 acceptable for knee sweeps,
   never a bare median for an invariant.
3. Audit remaining published 3-repeat tables of bimodal quantities; re-run any stragglers inside
   the G1 matrix.

**Done when:** the tooling change is merged with tests green and the policy is written.

### G4 — Multi-machine (hardware arrives 2026-08-28)

In priority order — if time is short, do 1 and 2 only:

1. **Injection validation.** Measure real LAN RTT; run the identical configuration on loopback
   with that value under `--link-latency-ms`; compare knees and gaps. Agreement retroactively
   validates the entire injected-latency methodology (Phases C, D2) — converts the paper's biggest
   stated weakness into a validation section. Disagreement must be known before writing.
2. **E7, 8,000 objects, one server per machine.** Decides whether the pacing-budget limit is
   contention on one oversubscribed box or load-fundamental. Either answer is publishable; today
   they are indistinguishable.
3. **Lookahead floor at real RTT** — does `L_min ≈ 4 + (T_L + T_J)/dt` predict on a real network.

Bring-up traps (from DEPLOY.md and this session): midware `--manager-ip`, port 1234 firewall,
`--server-exe` path ("midware connects but no servers appear"), `--halo-reliable` because real
networks genuinely drop packets, and the harness must **not** pass `--handoff-lookahead`
explicitly (sentinel `-1` = server default).

**Experiment freeze after G4.** Anything found later goes to future work, not the preprint. The
binding constraint from September onward is writing, not evidence.

---

## 3. Publication path

| # | milestone | when | notes |
|---|---|---|---|
| P1 | arXiv account registered (durable personal email) | now | needed for P2 regardless |
| P2 | Check endorser status of the Newcastle group | with P1 | see verification box below |
| P3 | Outreach email to Ushaw / Davison / Morgan with draft | Oct, draft near-final | **critical path** — see §4 |
| P4 | Primary category decision: `cs.DC` (cross-list `cs.NI`/`cs.MM`) | before endorsement code | codes are per-category |
| P5 | Draft complete | end Oct | claims per §1; system as apparatus |
| P6 | arXiv submission | **first week of Nov** | first-time moderation can take days — do not submit in the last week |
| P7 | Applications with preprint + two-year continuation story | Dec–Jan | target list informed by §4 |

### Endorsement facts and their verification status

**Verified (primary sources, 2026-08-27):**
- Ushaw and Davison each have exactly two arXiv papers, both 2026, both **cs.AI**, both as
  trailing co-authors: 2601.05899 (TowerMind), 2607.27967 (MARS-RA). Source: arXiv API.
- The per-paper "Which authors of this paper are endorsers?" page exists but requires an arXiv
  login — it is the definitive check and takes two clicks once P1 is done.

**Inferred — treat as working assumptions until the P2 check:**
- Endorsing *others* requires several recent papers in the target category's endorsement domain
  (arXiv keeps thresholds private; ~3+ is the folklore figure). Two cs.AI papers likely do not
  qualify Davison to endorse, and certainly not for cs.DC. → **Plan on fallback endorsers**: the
  Dyconits authors (TU Delft @Large) or the Kale & Kry authors, both active in-category on arXiv.
- Academic auto-endorsement: `ncl.ac.uk` accounts can very likely *submit* to cs.DC themselves
  even though they cannot *endorse* into it. → **Co-authorship bypasses endorsement entirely**
  (the paper goes up from their account). This is the cleanest path and one more reason P3 is on
  the critical path.

---

## 4. The outreach, and application targeting

The email to the AP group does four jobs at once: expert review from the best-placed readers, the
endorsement/co-authorship path, the Newcastle PhD conversation, and — even applying elsewhere — a
reference signal from the prior-art authors. The honest one-line framing: *"your §3.3 condition,
ported to non-migratory ghosts, gains a jitter term and a lookahead term, and then measurably runs
out past ~200 ms of total lag — a regime your evaluation didn't reach."*

Application targets made legible by this work: **Newcastle** (Ushaw/Morgan — direct continuation
of the AP line) and **TU Delft @Large** (Iosup — Dyconits, exactly this consistency-vs-cost
space). The validation apparatus (invariants, gates, four self-caught measurement artefacts this
phase alone) goes in the research statement and interviews, as a methodology section in the paper
— not as the paper's thesis.

---

## 5. Milestone checklist

- [ ] G1 ablation matrix run and tabled in EVALUATION
- [ ] G2 `docs/PREMISES.md` written and referenced
- [ ] G3 spread reporting in `analyse.py` + repeats policy documented
- [ ] G4.1 injection validation on real LAN
- [ ] G4.2 E7 one-server-per-machine
- [ ] G4.3 lookahead floor at real RTT (optional if time short)
- [ ] Experiment freeze declared
- [ ] P1 arXiv account
- [ ] P2 endorser status of Newcastle group checked (login-gated page)
- [ ] P3 outreach email sent with draft
- [ ] P4 category decided (cs.DC primary)
- [ ] P5 draft complete
- [ ] P6 arXiv submission, first week of November
- [ ] P7 applications out, December–January
