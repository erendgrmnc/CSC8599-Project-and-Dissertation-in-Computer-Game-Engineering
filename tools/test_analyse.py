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
