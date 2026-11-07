#!/usr/bin/env python3
"""Unit tests for scripts/kshell_run.py's `ktest` verdict.

The verdict is pure logic over the lines a command printed, so it runs here
rather than on a booted image; the booted runs themselves are exercised by
hand (and by the pilot migration's change record).
"""
from __future__ import annotations

import io
import sys
import unittest
from contextlib import redirect_stdout
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))

import kshell_run


def quiet(function, *args):
    with redirect_stdout(io.StringIO()):
        return function(*args)


def info(text: str) -> str:
    return f"   [6351527000 cpu0 info] {text}"


def error(text: str) -> str:
    return f"   [6351527000 cpu0 error] {text}"


class Classify(unittest.TestCase):
    def test_ktest_commands(self):
        for command in ("ktest", "ktest all", "ktest list", "ktest ?",
                        "ktest Ktest.*", "ktest Sched.*:-Sched.Slow"):
            self.assertTrue(kshell_run.is_ktest(command), command)
        for command in ("lockup status", "samplerclaim.test", "ktestx", ""):
            self.assertFalse(kshell_run.is_ktest(command), command)

    def test_list_and_run_forms(self):
        for command in ("ktest list", "ktest ?"):
            self.assertTrue(kshell_run.ktest_lists(command))
            self.assertFalse(kshell_run.ktest_runs_all(command))
        self.assertFalse(kshell_run.ktest_lists("ktest all"))
        for command in ("ktest", "ktest all", "ktest all rounds=256"):
            self.assertTrue(kshell_run.ktest_runs_all(command))
        for command in ("ktest Ktest.*", "ktest Ktest.* rounds=256"):
            self.assertFalse(kshell_run.ktest_runs_all(command))

    def test_count_varies_for_repeat_and_shard(self):
        for command in ("ktest all repeat=2", "ktest all repeat", "ktest all shard=1/2",
                        "ktest all shard", "ktest all shuffle repeat=2",
                        "ktest all shuffle=5 shard=0/3"):
            self.assertTrue(kshell_run.ktest_count_varies(command), command)
        for command in ("ktest", "ktest all", "ktest list", "ktest all shuffle",
                        "ktest all shuffle=305419896", "ktest all rounds=256",
                        "ktest Ktest.* repeat=2"):
            self.assertFalse(kshell_run.ktest_count_varies(command), command)


class EndPattern(unittest.TestCase):
    def test_a_run_ends_at_its_summary_or_its_error(self):
        self.assertTrue(kshell_run.KTEST_END.search("x ktest: SUMMARY ran=1 passed=1 failed=0 skipped=0\n"))
        self.assertTrue(kshell_run.KTEST_END.search("x ktest: ERROR no tests are linked into this kernel\n"))
        self.assertFalse(kshell_run.KTEST_END.search("x ktest: RUN Ktest.Foo\n"))


class Summaries(unittest.TestCase):
    def test_parses_one_summary(self):
        lines = [info("ktest: RUN Ktest.AssertionsPass"),
                 info("ktest: SUMMARY ran=3 passed=2 failed=0 skipped=1")]
        self.assertEqual(kshell_run.ktest_summaries(lines),
                         [{"ran": 3, "passed": 2, "failed": 0, "skipped": 1}])

    def test_no_summary(self):
        self.assertEqual(kshell_run.ktest_summaries([info("ktest: RUN Ktest.AssertionsPass")]), [])

    def test_counts_listed_tests(self):
        lines = [info("ktest: TEST Ktest.AssertionsPass"),
                 info("ktest: TEST Ktest.SkipIsReported"),
                 info("ktest: RUN Ktest.AssertionsPass")]
        self.assertEqual(kshell_run.ktest_listed(lines), 2)


class JudgeList(unittest.TestCase):
    def test_list_with_tests(self):
        lines = [info("ktest: TEST Ktest.AssertionsPass"),
                 info("ktest: TEST Ktest.SkipIsReported")]
        self.assertTrue(quiet(kshell_run.judge_ktest_list, "ktest list", lines))

    def test_list_without_tests(self):
        self.assertFalse(quiet(kshell_run.judge_ktest_list, "ktest list", []))

    def test_list_with_error_line(self):
        lines = [info("ktest: TEST Ktest.AssertionsPass"),
                 error('ktest: ERROR the test name "A.B" is registered twice')]
        self.assertFalse(quiet(kshell_run.judge_ktest_list, "ktest list", lines))


class JudgeRun(unittest.TestCase):
    def run_lines(self, summary: str, *extra: str) -> list[str]:
        return [info("ktest: RUN Ktest.AssertionsPass"),
                info("ktest: OK Ktest.AssertionsPass (100 us)"),
                info(summary),
                *extra]

    def test_passing_run_of_everything(self):
        lines = self.run_lines("ktest: SUMMARY ran=3 passed=2 failed=0 skipped=1")
        self.assertTrue(quiet(kshell_run.judge_ktest_run, "ktest all", lines, 3))

    def test_passing_filtered_run_has_no_expected_count(self):
        lines = self.run_lines("ktest: SUMMARY ran=1 passed=1 failed=0 skipped=0")
        self.assertTrue(quiet(kshell_run.judge_ktest_run, "ktest Ktest.*", lines, None))

    def test_passing_repeat_run_has_no_expected_count(self):
        # repeat= and shard= runs are passed expected=None: their summary
        # counts executions, which is the option's point, so the list-count
        # equality is not required of them.
        lines = self.run_lines("ktest: SUMMARY ran=6 passed=6 failed=0 skipped=0")
        self.assertTrue(quiet(kshell_run.judge_ktest_run, "ktest all repeat=3", lines, None))
        lines = self.run_lines("ktest: SUMMARY ran=31 passed=30 failed=0 skipped=1")
        self.assertTrue(quiet(kshell_run.judge_ktest_run, "ktest all shard=1/2", lines, None))

    def test_failed_test(self):
        lines = self.run_lines("ktest: SUMMARY ran=1 passed=0 failed=1 skipped=0",
                               error("ktest: FAILED Ktest.AssertionsPass (1 of 2 checks failed)"))
        self.assertFalse(quiet(kshell_run.judge_ktest_run, "ktest all", lines, 1))

    def test_fewer_ran_than_listed(self):
        lines = self.run_lines("ktest: SUMMARY ran=2 passed=2 failed=0 skipped=0")
        self.assertFalse(quiet(kshell_run.judge_ktest_run, "ktest all", lines, 3))

    def test_no_test_ran(self):
        lines = self.run_lines("ktest: SUMMARY ran=0 passed=0 failed=0 skipped=0")
        self.assertFalse(quiet(kshell_run.judge_ktest_run, "ktest NoSuch.*", lines, None))

    def test_missing_summary(self):
        lines = [info("ktest: RUN Ktest.AssertionsPass"),
                 info("ktest: OK Ktest.AssertionsPass (100 us)")]
        self.assertFalse(quiet(kshell_run.judge_ktest_run, "ktest all", lines, 1))

    def test_error_before_a_summary(self):
        lines = [error("ktest: ERROR no tests are linked into this kernel")]
        self.assertFalse(quiet(kshell_run.judge_ktest_run, "ktest all", lines, None))

    def test_inconsistent_summary(self):
        lines = self.run_lines("ktest: SUMMARY ran=3 passed=3 failed=0 skipped=1")
        self.assertFalse(quiet(kshell_run.judge_ktest_run, "ktest all", lines, 3))

    def test_two_summaries(self):
        summary = "ktest: SUMMARY ran=1 passed=1 failed=0 skipped=0"
        lines = self.run_lines(summary, info(summary))
        self.assertFalse(quiet(kshell_run.judge_ktest_run, "ktest all", lines, 1))

    def test_error_line_beside_a_passing_summary(self):
        lines = self.run_lines("ktest: SUMMARY ran=1 passed=1 failed=0 skipped=0",
                               error("some kernel error"))
        self.assertFalse(quiet(kshell_run.judge_ktest_run, "ktest all", lines, 1))


if __name__ == "__main__":
    unittest.main()
