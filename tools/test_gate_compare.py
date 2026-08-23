"""Unit tests for tools/gate-compare.py.

Standard library only, same convention as test_analyse.py. The module under test
has a hyphen in its filename (matching the other tools/*.py CLI scripts), which is
not a legal Python identifier, so it is loaded via importlib rather than a plain
import statement.

Run: python tools/test_gate_compare.py
"""

import importlib.util
import os
import shutil
import sys
import tempfile
import unittest

_HERE = os.path.dirname(os.path.abspath(__file__))
_spec = importlib.util.spec_from_file_location(
    "gate_compare", os.path.join(_HERE, "gate-compare.py")
)
gate_compare = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(gate_compare)


def _final_line(server_id, **fields):
    """Build one @@FINAL role=server line. cmdApplied etc. default to 0 so a test
    only has to spell out the fields it cares about.
    """
    defaults = dict(
        objPreseed=400, objPool=100, objs=100, contacts=0,
        hoSent=0, hoRecv=0, hoFail=0, hoLate=0, hoResent=0, hoReclaimed=0,
        hoCustody=0, hoClamp=0, hoDup=0, hoPending=0, hoSched=0,
        cmdApplied=0, cmdRelayed=0, cmdDup=0, cmdRejected=0, cmdFanout=0,
        objSpawned=0, objDestroyed=0, manifestSent=0,
        haloSent=0, haloRecv=0, haloAhead=0,
    )
    defaults.update(fields)
    parts = " ".join(f"{k}={v}" for k, v in defaults.items())
    return f"@@FINAL role=server id={server_id} {parts}\n"


class GateCompareFixture(unittest.TestCase):
    """Builds a temp experiment directory of run<n>/mid.log files, mirroring the
    real layout gate-compare.py reads (<experiment>/<run tag>/mid.log).
    """

    def setUp(self):
        self.directory = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.directory, ignore_errors=True)

    def _write_run(self, tag, lines):
        run_dir = os.path.join(self.directory, tag)
        os.makedirs(run_dir, exist_ok=True)
        with open(os.path.join(run_dir, "mid.log"), "w") as handle:
            handle.writelines(lines)

    def _two_server_run(self, tag, objs0=100, objs1=100, **shared_fields):
        self._write_run(tag, [
            _final_line(0, objs=objs0, objPool=objs0, **shared_fields),
            _final_line(1, objs=objs1, objPool=objs1, **shared_fields),
        ])


class WellFormedTwoRunGateTests(GateCompareFixture):
    """A well-formed two-server, two-repeat experiment on each side should pass:
    same stable-field values, same conserved total (400) on every run.
    """

    def test_identical_pre_and_post_pass(self):
        self._two_server_run("r1", objs0=201, objs1=199)
        self._two_server_run("r2", objs0=200, objs1=200)

        pre_totals = gate_compare.conservation_totals([self.directory])
        self.assertEqual(len(pre_totals), 2)
        self.assertEqual({t["objs"] for t in pre_totals}, {400})

        rows = gate_compare.finals(self.directory)
        self.assertEqual(len(rows), 4)

    def test_main_reports_gate_passed_when_both_sides_match(self):
        pre_dir = self.directory
        post_dir = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, post_dir, ignore_errors=True)

        for target_dir, self_directory in ((pre_dir, pre_dir), (post_dir, post_dir)):
            for tag in ("r1", "r2"):
                run_dir = os.path.join(target_dir, tag)
                os.makedirs(run_dir, exist_ok=True)
                with open(os.path.join(run_dir, "mid.log"), "w") as handle:
                    handle.writelines([
                        _final_line(0, objs=200, objPool=200),
                        _final_line(1, objs=200, objPool=200),
                    ])

        exit_code = self._run_main([pre_dir, post_dir])
        self.assertEqual(exit_code, 0)

    def _run_main(self, dirs):
        old_argv = sys.argv
        sys.argv = ["gate-compare.py"] + dirs
        try:
            return gate_compare.main()
        finally:
            sys.argv = old_argv


class MisPairingIsRejectedTests(GateCompareFixture):
    """The bug this test exists to catch: a run that contributes a different
    number of @@FINAL lines than its own distinct server-id count must fail
    loudly, not silently shift every later pairing by one and sum rows from
    different runs together.
    """

    def test_a_run_with_only_one_server_line_raises_instead_of_mispairing(self):
        # A 2-server experiment where one run's second server died before
        # printing @@FINAL - exactly the real failure mode named in the review.
        self._two_server_run("r1", objs0=201, objs1=199)
        self._write_run("r2", [_final_line(0, objs=200, objPool=200)])  # only 1 line

        with self.assertRaises(gate_compare.GateError) as ctx:
            gate_compare.conservation_totals([self.directory])

        message = str(ctx.exception)
        self.assertIn("r2", message.replace("\\", "/"))
        self.assertIn("mid.log", message)
        self.assertIn("1", message)  # the count found

    def test_main_fails_loudly_rather_than_reporting_a_wrong_total(self):
        self._two_server_run("r1", objs0=201, objs1=199)
        self._write_run("r2", [_final_line(0, objs=200, objPool=200)])

        post_dir = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, post_dir, ignore_errors=True)
        post_run = os.path.join(post_dir, "r1")
        os.makedirs(post_run)
        with open(os.path.join(post_run, "mid.log"), "w") as handle:
            handle.writelines([
                _final_line(0, objs=200, objPool=200),
                _final_line(1, objs=200, objPool=200),
            ])

        old_argv = sys.argv
        sys.argv = ["gate-compare.py", self.directory, post_dir]
        try:
            exit_code = gate_compare.main()
        finally:
            sys.argv = old_argv

        self.assertEqual(exit_code, 1)

    def test_a_log_with_no_server_lines_at_all_also_raises(self):
        self._write_run("r1", ["some unrelated line\n"])
        with self.assertRaises(gate_compare.GateError):
            gate_compare.conservation_totals([self.directory])


class StableFieldDivergenceTests(GateCompareFixture):
    """A stable field that actually differs between pre and post must be
    reported by name, which is the signal the whole gate exists to produce.
    """

    def test_a_diverging_stable_field_is_named_in_the_failure(self):
        pre_dir = self.directory
        post_dir = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, post_dir, ignore_errors=True)

        pre_run = os.path.join(pre_dir, "r1")
        os.makedirs(pre_run)
        with open(os.path.join(pre_run, "mid.log"), "w") as handle:
            handle.writelines([
                _final_line(0, objs=200, objPool=200, hoFail=0),
                _final_line(1, objs=200, objPool=200, hoFail=0),
            ])

        post_run = os.path.join(post_dir, "r1")
        os.makedirs(post_run)
        with open(os.path.join(post_run, "mid.log"), "w") as handle:
            handle.writelines([
                # hoFail is in STABLE and expected to be 0 on every clean run.
                # A Phase A regression would move it off zero - simulate that.
                _final_line(0, objs=200, objPool=200, hoFail=3),
                _final_line(1, objs=200, objPool=200, hoFail=0),
            ])

        old_argv, old_stdout = sys.argv, sys.stdout
        sys.argv = ["gate-compare.py", pre_dir, post_dir]
        import io
        captured = io.StringIO()
        sys.stdout = captured
        try:
            exit_code = gate_compare.main()
        finally:
            sys.argv, sys.stdout = old_argv, old_stdout

        self.assertEqual(exit_code, 1)
        self.assertIn("hoFail", captured.getvalue())
        self.assertIn("GATE FAILED", captured.getvalue())


class NaturalSortTests(unittest.TestCase):
    """r10 must sort after r2, not before it - plain lexicographic order would
    interleave two-digit repeat numbers with one-digit ones.
    """

    def test_double_digit_repeat_sorts_after_single_digit(self):
        paths = ["exp/ticks1800-r10/mid.log", "exp/ticks1800-r2/mid.log"]
        self.assertEqual(
            sorted(paths, key=gate_compare._natural_key),
            ["exp/ticks1800-r2/mid.log", "exp/ticks1800-r10/mid.log"],
        )


if __name__ == "__main__":
    unittest.main()
