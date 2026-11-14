#!/usr/bin/env python3
"""Unit tests for scripts/kshell_run.py's `ktest` verdict, the death tests'
included.

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
        for command in ("ktest", "ktest sel=all", "ktest sel=list", "ktest sel=?",
                        "ktest sel=Ktest.*", "ktest sel=Sched.*:-Sched.Slow"):
            self.assertTrue(kshell_run.is_ktest(command), command)
        for command in ("lockup status", "samplerclaim.test", "ktestx", ""):
            self.assertFalse(kshell_run.is_ktest(command), command)

    def test_list_and_run_forms(self):
        for command in ("ktest sel=list", "ktest sel=?"):
            self.assertTrue(kshell_run.ktest_lists(command))
            self.assertFalse(kshell_run.ktest_runs_all(command))
        self.assertFalse(kshell_run.ktest_lists("ktest sel=all"))
        for command in ("ktest", "ktest sel=all", "ktest sel=all rounds=256"):
            self.assertTrue(kshell_run.ktest_runs_all(command))
        for command in ("ktest sel=Ktest.*", "ktest sel=Ktest.* rounds=256"):
            self.assertFalse(kshell_run.ktest_runs_all(command))

    def test_count_varies_for_repeat_and_shard(self):
        for command in ("ktest sel=all repeat=2", "ktest sel=all repeat", "ktest sel=all shard=1/2",
                        "ktest sel=all shard", "ktest sel=all shuffle repeat=2",
                        "ktest sel=all shuffle=5 shard=0/3"):
            self.assertTrue(kshell_run.ktest_count_varies(command), command)
        for command in ("ktest", "ktest sel=all", "ktest sel=list", "ktest sel=all shuffle",
                        "ktest sel=all shuffle=305419896", "ktest sel=all rounds=256",
                        "ktest sel=Ktest.* repeat=2"):
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


BANNER = "OURS KERNEL PANIC"
DEATH_NAME = "KtestDeath.PanicsOnDemand"


def death_command() -> str:
    return f"ktest sel=death filter={DEATH_NAME}"


def replay() -> str:
    """What a whole panic report looks like behind the banner and its message:
    the record ring between its two markers (kernel/top/panic.cpp)."""
    return ("--- recent log: the last 3 lines ---\n"
            + info(f"ktest: DEATH {DEATH_NAME}") + "\n"
            + "--- end of recent log ---\n")


class DeathClassify(unittest.TestCase):
    def test_death_selection_forms(self):
        for command in ("ktest sel=death", f"ktest sel=death filter={DEATH_NAME}",
                        "ktest sel=death filter=KtestDeath.*"):
            self.assertTrue(kshell_run.ktest_deaths(command), command)
        for command in ("ktest", "ktest sel=all", "ktest sel=list", "ktest sel=Ktest.*",
                        "ktest sel=Death.*"):
            self.assertFalse(kshell_run.ktest_deaths(command), command)

    def test_death_ends_the_machine(self):
        for command in ("ktest sel=death", f"ktest sel=death filter={DEATH_NAME}"):
            self.assertTrue(kshell_run.ends_machine(command), command)
        for command in ("ktest sel=list", "ktest sel=all", "ktest sel=Ktest.*"):
            self.assertFalse(kshell_run.ends_machine(command), command)


class DeathFilter(unittest.TestCase):
    def test_empty_filter_selects_everything(self):
        self.assertTrue(kshell_run.ktest_filter_matches("", DEATH_NAME))

    def test_globs(self):
        self.assertTrue(kshell_run.ktest_filter_matches("KtestDeath.*", DEATH_NAME))
        self.assertTrue(kshell_run.ktest_filter_matches("*Panics*", DEATH_NAME))
        self.assertTrue(kshell_run.ktest_filter_matches(DEATH_NAME, DEATH_NAME))
        self.assertTrue(kshell_run.ktest_filter_matches("KtestDeat?.PanicsOnDemand", DEATH_NAME))
        self.assertFalse(kshell_run.ktest_filter_matches("KtestDeath.*", "Ktest.AssertionsPass"))
        self.assertFalse(kshell_run.ktest_filter_matches("Ktest?", DEATH_NAME))

    def test_colon_tokens_and_exclusions(self):
        self.assertTrue(kshell_run.ktest_filter_matches("Ktest.AssertionsPass:KtestDeath.*",
                                                        DEATH_NAME))
        self.assertFalse(kshell_run.ktest_filter_matches("-KtestDeath.*", DEATH_NAME))
        self.assertTrue(kshell_run.ktest_filter_matches("-Ktest.AssertionsPass", DEATH_NAME))


class DeathList(unittest.TestCase):
    def test_names_from_the_list(self):
        lines = [info("ktest: TEST Ktest.AssertionsPass"),
                 info(f"ktest: DEATH {DEATH_NAME}")]
        self.assertEqual(kshell_run.ktest_death_names(lines), [DEATH_NAME])

    def test_no_death_line(self):
        self.assertEqual(kshell_run.ktest_death_names([info("ktest: TEST Ktest.AssertionsPass")]),
                         [])


class DeathEndPattern(unittest.TestCase):
    def test_done_matches_a_panic_report_or_the_runner_s_own_end(self):
        self.assertTrue(kshell_run.KTEST_DEATH_DONE.search(
            f"{BANNER}\n[1 cpu0 error] ktest: death test reached its panic\n"))
        self.assertTrue(kshell_run.KTEST_DEATH_DONE.search(
            "x ktest: SUMMARY ran=1 passed=0 failed=1 skipped=0\n"))
        self.assertTrue(kshell_run.KTEST_DEATH_DONE.search(
            "x ktest: ERROR no death tests are linked into this kernel\n"))
        self.assertFalse(kshell_run.KTEST_DEATH_DONE.search(f"x ktest: DEATH {DEATH_NAME}\n"))
        self.assertFalse(kshell_run.KTEST_DEATH_DONE.search("x ktest: RUN Ktest.AssertionsPass\n"))


class JudgeDeath(unittest.TestCase):
    def panic_lines(self, *extra: str) -> list[str]:
        return [info(f"ktest: DEATH {DEATH_NAME}"),
                BANNER,
                error("ktest: death test reached its panic"),
                *extra]

    def test_panic_report_after_the_marker_passes(self):
        lines = self.panic_lines()
        self.assertTrue(quiet(kshell_run.judge_ktest_death, death_command(), lines, replay(),
                              DEATH_NAME))

    def test_panic_without_the_marker_fails(self):
        lines = [BANNER, error("crash: deliberate panic from the kernel shell")]
        self.assertFalse(quiet(kshell_run.judge_ktest_death, death_command(), lines, replay(),
                               DEATH_NAME))

    def test_panic_before_the_marker_fails(self):
        lines = [BANNER, error("crash: deliberate panic from the kernel shell"),
                 info(f"ktest: DEATH {DEATH_NAME}")]
        self.assertFalse(quiet(kshell_run.judge_ktest_death, death_command(), lines, replay(),
                               DEATH_NAME))

    def test_prompt_between_marker_and_banner_fails(self):
        # The shell coming back means the body returned and something else
        # died later: not the death test's panic.
        lines = [info(f"ktest: DEATH {DEATH_NAME}"), "kshell> ", BANNER,
                 error("some unrelated panic")]
        self.assertFalse(quiet(kshell_run.judge_ktest_death, death_command(), lines, replay(),
                               DEATH_NAME))

    def test_survivor_fails(self):
        lines = [info(f"ktest: DEATH {DEATH_NAME}"),
                 error(f"ktest: FAILED {DEATH_NAME} (a death test must panic; it returned)"),
                 info("ktest: SUMMARY ran=1 passed=0 failed=1 skipped=0")]
        self.assertFalse(quiet(kshell_run.judge_ktest_death, death_command(), lines, "",
                               DEATH_NAME))

    def test_message_not_error_level_fails(self):
        lines = [info(f"ktest: DEATH {DEATH_NAME}"), BANNER, info("not the message")]
        self.assertFalse(quiet(kshell_run.judge_ktest_death, death_command(), lines, replay(),
                               DEATH_NAME))

    def test_two_banners_fail(self):
        lines = self.panic_lines(BANNER)
        self.assertFalse(quiet(kshell_run.judge_ktest_death, death_command(), lines, "",
                               DEATH_NAME))


if __name__ == "__main__":
    unittest.main()
