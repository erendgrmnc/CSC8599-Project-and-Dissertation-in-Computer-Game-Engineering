"""Unit tests for the analysis helpers in analyse.py.

Standard library only, for the same reason analyse.py is: the numbers have to be
checkable on the machine that produced the data with no environment setup.

Run: python tools/test_analyse.py
"""

import io
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


class CustodyReproducibilityNoteTests(unittest.TestCase):
    """custody_reproducibility_note - Task 6a Finding 3: a wall-clock-gated custody
    action must be reported as a reproducibility hazard, not just a raw counter."""

    def test_neither_counter_firing_returns_none(self):
        self.assertIsNone(analyse.custody_reproducibility_note(0, 0))

    def test_resend_alone_is_flagged(self):
        note = analyse.custody_reproducibility_note(3, 0)
        self.assertIsNotNone(note)
        self.assertIn("NOT", note)
        self.assertIn("bit-reproducible", note)

    def test_reclaim_alone_is_flagged(self):
        note = analyse.custody_reproducibility_note(0, 1)
        self.assertIsNotNone(note)
        self.assertIn("bit-reproducible", note)

    def test_message_reports_both_counts(self):
        note = analyse.custody_reproducibility_note(5, 2)
        self.assertIn("hoResent=5", note)
        self.assertIn("hoReclaimed=2", note)

    def test_string_counters_from_read_final_lines_are_accepted(self):
        # summarise_run's totals come from regex-captured strings ("0", "3", ...),
        # exactly like check_custody's hoCustody field - this must not misbehave
        # the same way an un-cast truthiness check would.
        self.assertIsNone(analyse.custody_reproducibility_note("0", "0"))
        self.assertIsNotNone(analyse.custody_reproducibility_note("0", "1"))


class ApFrameTimeTests(unittest.TestCase):
    """AP's metric: max frame time across servers per 5 s window.

    A 'frame' is a loop iteration in which physics advanced; its frame time is
    the wall-clock gap since the previous such iteration. Rows with substeps == 0
    did no physics and are not frames - but the time they consumed still belongs
    to the next frame's interval, which is why the gap is measured between
    consecutive physics rows rather than between adjacent CSV rows.

    Windows are SIMULATED seconds (cumulative substeps / substep rate), while the
    frame time inside them stays wall-clock milliseconds. AP's x-axis indexes the
    injected population - at t seconds their world holds 160*t objects - so a
    window only compares against theirs if it is placed by how far the schedule
    has advanced. A paced 60 s injection run that cannot hold the pace takes ~150 s
    of wall clock, and wall-clock windows would spread AP's 60 s benchmark across
    an axis two and a half times too long.
    """

    def test_gap_is_measured_between_physics_rows_not_adjacent_rows(self):
        rows = [
            {"time_us": "0", "substeps": "1"},
            {"time_us": "1000", "substeps": "0"},
            {"time_us": "2000", "substeps": "0"},
            {"time_us": "8000", "substeps": "1"},
        ]
        buckets = analyse.ap_frame_times(rows, bucket_seconds=5.0)
        # One frame interval: 8000 - 0 = 8 ms. The two idle rows are absorbed.
        self.assertEqual(buckets, {0: 8.0})

    def test_max_is_taken_within_a_bucket(self):
        rows = [
            {"time_us": "0", "substeps": "1"},
            {"time_us": "2000", "substeps": "1"},
            {"time_us": "9000", "substeps": "1"},
        ]
        buckets = analyse.ap_frame_times(rows, bucket_seconds=5.0)
        self.assertEqual(buckets, {0: 7.0})

    def test_windows_advance_with_simulated_time(self):
        # 600 substeps at 120 Hz is 5 s simulated, so the last frame lands in the
        # second window even though only 3 ms of wall clock elapsed.
        rows = [
            {"time_us": "0", "substeps": "1"},
            {"time_us": "1000", "substeps": "1"},
            {"time_us": "2000", "substeps": "598"},
        ]
        buckets = analyse.ap_frame_times(rows, bucket_seconds=5.0)
        self.assertEqual(sorted(buckets), [0, 1])

    def test_wall_clock_alone_does_not_advance_the_window(self):
        # Six seconds of wall clock, but only two substeps of simulated time: this
        # is one very slow frame at the START of the benchmark, not a later one.
        rows = [
            {"time_us": "0", "substeps": "1"},
            {"time_us": "6000000", "substeps": "1"},
        ]
        buckets = analyse.ap_frame_times(rows, bucket_seconds=5.0)
        self.assertEqual(buckets, {0: 6000.0})

    def test_no_physics_rows_yields_no_buckets(self):
        rows = [{"time_us": "0", "substeps": "0"}, {"time_us": "1000", "substeps": "0"}]
        self.assertEqual(analyse.ap_frame_times(rows, bucket_seconds=5.0), {})

    def test_a_single_physics_row_yields_no_interval(self):
        rows = [{"time_us": "5", "substeps": "1"}]
        self.assertEqual(analyse.ap_frame_times(rows, bucket_seconds=5.0), {})

    def test_missing_substeps_column_is_treated_as_no_frames(self):
        # Runs recorded before the substeps column existed must not silently
        # produce a frame-time series computed from every loop iteration.
        rows = [{"time_us": "0"}, {"time_us": "1000"}]
        self.assertEqual(analyse.ap_frame_times(rows, bucket_seconds=5.0), {})


class ApFrameFloorNoteTests(unittest.TestCase):
    """The frame-time floor has two different causes, and naming the wrong one
    turns a meaningful number into an apparent measurement artifact.

    In PACED mode the loop waits with sleep_until(tick * fixedDt), so a server
    keeping up reports a frame time equal to the physics step - that is the
    timestep itself, and a declared deviation from AP's 16 ms rather than an
    artifact. Only in REALTIME mode does the loop take the sleep_for(1ms) branch,
    which Windows rounds up to the timer granularity. The two floors are ~8.33 ms
    and ~8.3 ms, near enough to be mistaken for each other in the data.
    """

    def test_paced_note_names_the_timestep_not_the_sleep(self):
        note = " ".join(analyse.ap_frame_floor_note([True, True]))
        self.assertIn("8.33", note)
        self.assertNotIn("sleep_for", note)

    def test_realtime_note_names_the_sleep(self):
        note = " ".join(analyse.ap_frame_floor_note([False, False]))
        self.assertIn("sleep_for(1ms)", note)

    def test_mixed_modes_name_both(self):
        note = " ".join(analyse.ap_frame_floor_note([True, False]))
        self.assertIn("sleep_for(1ms)", note)
        self.assertIn("8.33", note)

    def test_no_runs_still_returns_lines(self):
        self.assertIsInstance(analyse.ap_frame_floor_note([]), list)


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


class SummariseRunWireBytesWiringTests(unittest.TestCase):
    """Task 3's deferred minor: WireBytesTests covers wire_bytes() itself, but
    nothing checks that summarise_run's net_cli_wire_bytes / net_peer_wire_bytes keys
    are wired to the RIGHT @@FINAL fields. E8's entire headline flows through those
    two keys, so a cli/peer swap - or either key reading the wrong field name -
    would silently swap which host family a reported figure describes.

    Uses clearly different magnitudes for the client-facing and peer-facing totals
    (roughly 2000x apart) so that a transposition changes the result by orders of
    magnitude rather than landing inside a tolerance and passing by accident.

    summarise_run reads @@FINAL lines from mid.log (and any cli*.log present) via
    read_final_lines(); it does not require ticks-server*.csv to be present at all
    for these two invariant keys - verified directly (an empty `servers` list, no
    exception) before relying on that here, so none is written.
    """

    def setUp(self):
        self.directory = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.directory, ignore_errors=True)

    def _write_mid_log(self, lines):
        with open(os.path.join(self.directory, "mid.log"), "w") as handle:
            handle.write("\n".join(lines) + "\n")

    def test_cli_and_peer_totals_are_not_transposed(self):
        # Client-facing: large. Peer-facing: small. A cli/peer swap - or either
        # key reading the other's field names - flips which figure is which by
        # roughly three orders of magnitude, so the assertions below fail hard
        # rather than by a rounding margin.
        self._write_mid_log([
            "@@FINAL role=server id=0 netCliBytes=1000000 netCliPkts=100 "
            "netPeerBytes=500 netPeerPkts=5",
            "@@FINAL role=server id=1 netCliBytes=2000000 netCliPkts=200 "
            "netPeerBytes=700 netPeerPkts=7",
        ])

        _servers, invariants, _custody = analyse.summarise_run(self.directory)

        # sum(bytes) + 28 * sum(packets), summed across both server lines.
        expected_cli = (1000000 + 2000000) + 28 * (100 + 200)
        expected_peer = (500 + 700) + 28 * (5 + 7)
        self.assertEqual(expected_cli, 3008400)
        self.assertEqual(expected_peer, 1536)

        self.assertEqual(invariants["net_cli_wire_bytes"], expected_cli)
        self.assertEqual(invariants["net_peer_wire_bytes"], expected_peer)
        # Belt and braces against a transposed wiring: the two keys must land on
        # the magnitude that matches their own field family, not each other's.
        self.assertNotEqual(
            invariants["net_cli_wire_bytes"], invariants["net_peer_wire_bytes"]
        )
        self.assertGreater(
            invariants["net_cli_wire_bytes"], invariants["net_peer_wire_bytes"]
        )


class I4VacuityTests(unittest.TestCase):
    """Invariant I4 reconciles the client's cmdSent against the servers'
    applied+rejected+dup. Until 2026-08-24 the harness force-killed clients before
    they printed @@FINAL, so `sent` was always 0. On a run that drove no commands
    the server terms were 0 too, cmd_delta came out 0 - 0, and I4 reported as a
    PASS while checking nothing. That held for all 60 client logs of Phase A.

    These tests pin the two keys that let a caller tell "checked and balanced"
    apart from "nothing to check". They deliberately assert on client_finals and
    cmd_server_side rather than on cmd_delta, because cmd_delta is exactly the
    quantity that cannot express the difference.
    """

    def setUp(self):
        self.directory = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.directory, ignore_errors=True)

    def _write(self, name, lines):
        with open(os.path.join(self.directory, name), "w") as handle:
            handle.write(chr(10).join(lines) + chr(10))

    def test_missing_client_final_is_visible_and_does_not_look_like_a_pass(self):
        self._write("mid.log", [
            "@@FINAL role=server id=0 cmdApplied=0 cmdRejected=0 cmdDup=0",
            "@@FINAL role=server id=1 cmdApplied=0 cmdRejected=0 cmdDup=0",
        ])

        _servers, invariants, _custody = analyse.summarise_run(self.directory)

        # The trap: the delta alone says "balanced".
        self.assertEqual(invariants["cmd_delta"], 0)
        # The signal that says it was never actually checked.
        self.assertEqual(invariants["client_finals"], 0)
        self.assertEqual(invariants["cmd_server_side"], 0)

    def test_client_final_present_is_counted_and_reconciles(self):
        self._write("mid.log", [
            "@@FINAL role=server id=0 cmdApplied=6 cmdRejected=1 cmdDup=0 cmdFanout=0",
            "@@FINAL role=server id=1 cmdApplied=3 cmdRejected=0 cmdDup=2 cmdFanout=0",
        ])
        self._write("cli-0.log", ["@@FINAL role=client cmdSent=12 resurrectAttempts=0"])

        _servers, invariants, _custody = analyse.summarise_run(self.directory)

        self.assertEqual(invariants["client_finals"], 1)
        self.assertEqual(invariants["cmd_sent"], 12)
        # 6+1+0 + 3+0+2 = 12, so the tally balances and the delta is a real zero
        # this time rather than an absence of evidence.
        self.assertEqual(invariants["cmd_server_side"], 12)
        self.assertEqual(invariants["cmd_delta"], 0)

    def test_server_command_activity_without_a_client_final_is_detectable(self):
        """The dangerous case: commands were processed but the client never
        reported. cmd_delta goes negative, which names the wrong cause; the caller
        needs cmd_server_side > 0 with client_finals == 0 to report it correctly."""
        self._write("mid.log", [
            "@@FINAL role=server id=0 cmdApplied=9 cmdRejected=0 cmdDup=0 cmdFanout=0",
        ])

        _servers, invariants, _custody = analyse.summarise_run(self.directory)

        self.assertEqual(invariants["client_finals"], 0)
        self.assertEqual(invariants["cmd_server_side"], 9)
        self.assertEqual(invariants["cmd_delta"], -9)


class ConservationFieldTests(unittest.TestCase):
    """Conservation must be counted on objPool + hoCustody, never on objs.

    `objs` is Profiler::GetObjectsOnBorders(), written by ServerWorldManager::Update
    as the count of mTestObjects with physics. The drain phase runs the loop WITHOUT
    stepping the world, so `objs` freezes at the last stepped tick while the drain is
    still installing in-flight arrivals. On an E4 rebalancing run that made a
    fully-conserved run report a 2,244-object loss - the whole of backlog item 12.

    These tests pin the field choice, not the arithmetic: the failure mode is reading
    a stale counter, and only a fixture where objs and objPool DISAGREE can catch it.
    """

    def setUp(self):
        self.directory = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.directory, ignore_errors=True)

    def _write_mid_log(self, lines):
        with open(os.path.join(self.directory, "mid.log"), "w") as handle:
            handle.write(chr(10).join(lines) + chr(10))

    def test_stale_objs_does_not_read_as_a_loss(self):
        # The measured shape: objs frozen mid-drain at 1756, objPool complete at 4000,
        # custody emptied by the drain. Nothing was lost.
        self._write_mid_log([
            "@@FINAL role=server id=0 objs=732 objPreseed=4000 objPool=2976 "
            "hoCustody=0 objSpawned=0 objDestroyed=0",
            "@@FINAL role=server id=1 objs=1024 objPreseed=4000 objPool=1024 "
            "hoCustody=0 objSpawned=0 objDestroyed=0",
        ])

        _servers, invariants, _custody = analyse.summarise_run(self.directory)

        self.assertEqual(invariants["conservation_delta"], 0)
        # Reading objs instead would have produced this, which is the bug.
        self.assertEqual(invariants["drain_recovered"], 4000 - 1756)

    def test_in_flight_custody_counts_towards_conservation(self):
        # A transfer in custody has been released by the sender and not installed by
        # the receiver, so it sits in NEITHER objPool. Omitting it reads as a loss.
        self._write_mid_log([
            "@@FINAL role=server id=0 objs=963 objPreseed=4000 objPool=963 "
            "hoCustody=315 objSpawned=0 objDestroyed=0",
            "@@FINAL role=server id=1 objs=1024 objPreseed=4000 objPool=2722 "
            "hoCustody=0 objSpawned=0 objDestroyed=0",
        ])

        _servers, invariants, _custody = analyse.summarise_run(self.directory)

        # 963 + 2722 + 315 = 4000.
        self.assertEqual(invariants["conservation_delta"], 0)

    def test_a_genuine_loss_is_still_detected(self):
        """The check must not have been widened into uselessness."""
        self._write_mid_log([
            "@@FINAL role=server id=0 objs=500 objPreseed=4000 objPool=500 "
            "hoCustody=0 objSpawned=0 objDestroyed=0",
            "@@FINAL role=server id=1 objs=1000 objPreseed=4000 objPool=1000 "
            "hoCustody=0 objSpawned=0 objDestroyed=0",
        ])

        _servers, invariants, _custody = analyse.summarise_run(self.directory)

        self.assertEqual(invariants["conservation_delta"], -2500)

    def test_spawns_and_destroys_still_shift_the_expected_total(self):
        self._write_mid_log([
            "@@FINAL role=server id=0 objs=0 objPreseed=100 objPool=104 "
            "hoCustody=0 objSpawned=7 objDestroyed=3",
        ])

        _servers, invariants, _custody = analyse.summarise_run(self.directory)

        # 100 + 7 - 3 = 104.
        self.assertEqual(invariants["conservation_delta"], 0)



class GeneralisedHaloFloorTests(unittest.TestCase):
    """The latency dimension of the floor, mirroring HaloBound.h.

    This is the Python half of Phase C gate 4.4.2. The C++ assert harness pins the
    server's own arithmetic; this pins the analyser's mirror of it. Both are needed:
    the two are separate transcriptions of one formula, and a drift between them
    would be invisible to either one alone - the analyser would judge every run
    against a line the server never used.
    """

    @staticmethod
    def _published_zero_latency_floor(lookahead, substep_hz=120):
        # Written longhand, exactly as HaloBoundTests.cpp does. Calling the
        # generalised function to compute the expected value would assert nothing.
        return 60.0 * (lookahead / float(substep_hz)) + 2.0 * 2.0

    def test_reduces_to_the_published_expression_at_zero_delay(self):
        for lookahead in (0, 2, 4, 8, 16, 24, 30):
            self.assertAlmostEqual(
                analyse.predicted_halo_floor(lookahead, link_latency_ms=0.0,
                                             link_jitter_ms=0.0),
                self._published_zero_latency_floor(lookahead),
                msg="lookahead {}".format(lookahead))

    def test_latency_adds_travel_distance_at_the_assumed_maximum_speed(self):
        # 60 units/s for 50 ms is exactly 3 units.
        base = analyse.predicted_halo_floor(4)
        self.assertAlmostEqual(
            analyse.predicted_halo_floor(4, link_latency_ms=50.0), base + 3.0)
        self.assertAlmostEqual(
            analyse.predicted_halo_floor(4, link_latency_ms=100.0), base + 6.0)

    def test_jitter_adds_at_its_maximum_not_its_mean(self):
        self.assertAlmostEqual(
            analyse.predicted_halo_floor(4, link_jitter_ms=50.0),
            analyse.predicted_halo_floor(4, link_latency_ms=50.0))
        self.assertAlmostEqual(
            analyse.predicted_halo_floor(4, link_latency_ms=25.0, link_jitter_ms=25.0),
            analyse.predicted_halo_floor(4, link_latency_ms=50.0))

    def test_negative_delays_cannot_shrink_the_floor(self):
        base = analyse.predicted_halo_floor(4)
        self.assertAlmostEqual(
            analyse.predicted_halo_floor(4, link_latency_ms=-50.0), base)
        self.assertAlmostEqual(
            analyse.predicted_halo_floor(4, link_jitter_ms=-50.0), base)

    def test_matches_the_cpp_constants_at_a_swept_latency_point(self):
        # L=8 at 100 ms: 60 * (8/120 + 0.1) + 4 = 60 * 0.16667 + 4 = 14.
        self.assertAlmostEqual(
            analyse.predicted_halo_floor(8, link_latency_ms=100.0), 14.0)


class KneeReportLatencyWiringTests(unittest.TestCase):
    """The manifest's injected delay must reach the floor the knee is judged against.

    Reading the delay is the whole point: a latency run compared against the
    zero-latency line is being compared to a bound it never used. The failure is
    one-directional and quiet - the zero-latency floor is LOWER, so a knee the
    generalised bound comfortably covers would be reported as UNSOUND, and Phase C
    would publish a falsification of its own bound that is really a wiring bug.
    """

    def _rows(self, crossings_by_width):
        rows = []
        for width, value in crossings_by_width.items():
            for repeat in (1, 2, 3):
                for server in (0, 1):
                    rows.append({"point": width, "repeat": repeat,
                                 "server": server, "ho_sent": value})
        return rows

    def test_latency_in_the_manifest_raises_the_floor(self):
        manifest = {"sweep": "haloWidth",
                    "fixed": {"haloLookahead": 8, "linkLatencyMs": 100.0}}
        result = analyse.print_knee_report(manifest, self._rows({3: 100, 4: 0, 5: 0}))
        self.assertAlmostEqual(result["floor"], 14.0)
        self.assertEqual(result["latency"], 100.0)

    def test_a_knee_above_the_zero_latency_floor_is_sound_once_latency_is_read(self):
        # Knee 10 sits ABOVE the zero-latency floor for L=8 (8.0) and below the
        # 100 ms floor (14.0). Judged against the wrong line this reads UNSOUND.
        manifest = {"sweep": "haloWidth",
                    "fixed": {"haloLookahead": 8, "linkLatencyMs": 100.0}}
        rows = self._rows({8: 100, 9: 40, 10: 0, 11: 0})
        result = analyse.print_knee_report(manifest, rows)
        self.assertEqual(result["knee"], 10)
        self.assertTrue(result["sound"])

        no_latency = {"sweep": "haloWidth", "fixed": {"haloLookahead": 8}}
        self.assertFalse(analyse.print_knee_report(no_latency, rows)["sound"])

    def test_jitter_is_read_as_well_as_latency(self):
        manifest = {"sweep": "haloWidth",
                    "fixed": {"haloLookahead": 8, "linkJitterMs": 100.0}}
        result = analyse.print_knee_report(manifest, self._rows({3: 100, 4: 0}))
        self.assertAlmostEqual(result["floor"], 14.0)
        self.assertEqual(result["jitter"], 100.0)

    def test_a_manifest_without_delay_keys_still_reports_the_published_floor(self):
        # Every pre-Phase-C run has no such keys. They must read as zero rather than
        # raising, or the 120-run zero-latency baseline stops being analysable.
        manifest = {"sweep": "haloWidth", "fixed": {"haloLookahead": 16}}
        result = analyse.print_knee_report(manifest, self._rows({4: 100, 5: 0}))
        self.assertAlmostEqual(result["floor"], 12.0)
        self.assertEqual(result["latency"], 0.0)
        self.assertEqual(result["jitter"], 0.0)

    def test_a_null_delay_field_is_treated_as_zero(self):
        manifest = {"sweep": "haloWidth",
                    "fixed": {"haloLookahead": 16, "linkLatencyMs": None,
                              "linkJitterMs": None}}
        result = analyse.print_knee_report(manifest, self._rows({4: 100, 5: 0}))
        self.assertAlmostEqual(result["floor"], 12.0)


class TrackingReportShapeTests(unittest.TestCase):
    """The zero-latency table is quoted verbatim in E5. It must not gain columns."""

    @staticmethod
    def _capture(knees):
        buffer = io.StringIO()
        stdout = sys.stdout
        sys.stdout = buffer
        try:
            analyse.print_tracking_report(knees)
        finally:
            sys.stdout = stdout
        return buffer.getvalue()

    def _entry(self, lookahead, floor, knee, latency=0.0, jitter=0.0):
        return {"lookahead": lookahead, "floor": floor, "knee": knee,
                "sound": knee is not None and knee <= floor,
                "latency": latency, "jitter": jitter}

    def test_all_zero_delay_keeps_the_published_table_shape(self):
        text = self._capture([self._entry(2, 5.0, 1), self._entry(8, 8.0, 3)])
        self.assertIn("halo soundness across lookaheads", text)
        self.assertNotIn("latency", text)
        self.assertNotIn("jitter", text)

    def test_any_injected_delay_adds_the_delay_columns(self):
        text = self._capture([self._entry(8, 8.0, 3),
                              self._entry(8, 14.0, 4, latency=100.0)])
        self.assertIn("injected link delay", text)
        self.assertIn("latency", text)
        self.assertIn("jitter", text)

    def test_entries_sharing_a_lookahead_are_both_reported(self):
        # The summary used to key on lookahead alone. A latency sweep puts several
        # points at the SAME lookahead, and collapsing them would silently drop
        # every point but one - the sweep would look like it had run and reported
        # nothing.
        text = self._capture([self._entry(8, 8.0, 3),
                              self._entry(8, 11.0, 3, latency=50.0),
                              self._entry(8, 14.0, 4, latency=100.0)])
        rows = [line for line in text.splitlines() if line.strip().startswith("8 ")]
        self.assertEqual(len(rows), 3)

    def test_entries_missing_delay_keys_do_not_crash_the_summary(self):
        text = self._capture([{"lookahead": 4, "floor": 6.0, "knee": 2,
                               "sound": True}])
        self.assertIn("halo soundness across lookaheads", text)

if __name__ == "__main__":
    unittest.main()
