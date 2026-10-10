import json
import re
import shutil
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).parent))

import cov_host


def touch(path, text=""):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text)
    return path


def summary(covered, count):
    return {"covered": covered, "count": count, "percent": 0}


def file_entry(filename, lines=(0, 0), regions=(0, 0), branches=(0, 0), functions=(0, 0)):
    return {"filename": filename,
            "summary": {"lines": summary(*lines), "regions": summary(*regions),
                        "branches": summary(*branches), "functions": summary(*functions)}}


class CovHost(unittest.TestCase):
    def setUp(self):
        self.root = Path(tempfile.mkdtemp()).resolve()
        self.addCleanup(shutil.rmtree, self.root, ignore_errors=True)
        self.out = self.root / "out" / "cov"
        self.out.mkdir(parents=True)

    def test_a_source_belongs_to_the_nearest_directory_with_a_build_file(self):
        touch(self.root / "kernel/lib/sampler/BUILD.gn")
        touch(self.root / "kernel/lib/sampler/test/BUILD.gn")
        cache = {}
        self.assertEqual(
            cov_host.unit_of(self.root, "kernel/lib/sampler/src/dump.cpp", cache),
            "kernel/lib/sampler")
        self.assertEqual(
            cov_host.unit_of(self.root, "kernel/lib/sampler/include/sampler/sink.hpp", cache),
            "kernel/lib/sampler")
        self.assertEqual(
            cov_host.unit_of(self.root, "kernel/lib/sampler/test/a_test.cpp", cache),
            "kernel/lib/sampler/test")
        self.assertEqual(cov_host.unit_of(self.root, "stray.cpp", cache), ".")

    def test_counted_resolves_names_against_the_build_directory(self):
        self.assertEqual(
            cov_host.counted(self.root, self.out, "../../kernel/lib/x/src/a.cpp"),
            "kernel/lib/x/src/a.cpp")
        self.assertEqual(
            cov_host.counted(self.root, self.out, str(self.root / "kernel/a.cpp")),
            "kernel/a.cpp")
        self.assertIsNone(cov_host.counted(self.root, self.out, "/Library/SDK/usr/include/x.h"))
        self.assertIsNone(cov_host.counted(self.root, self.out, "gen/kernel/generated.cpp"))

    def test_summarize_adds_files_of_a_unit_and_skips_foreign_ones(self):
        touch(self.root / "kernel/lib/x/BUILD.gn")
        touch(self.root / "system/lib/y/BUILD.gn")
        export = {"data": [{"files": [
            file_entry("../../kernel/lib/x/src/a.cpp", lines=(8, 10), functions=(2, 2)),
            file_entry("../../kernel/lib/x/src/b.cpp", lines=(2, 10), functions=(0, 2)),
            file_entry("../../system/lib/y/src/c.cpp", lines=(5, 5), branches=(1, 4)),
            file_entry("/Library/SDK/x.h", lines=(99, 99)),
        ]}]}
        units, total = cov_host.summarize(self.root, self.out, export)
        self.assertEqual(sorted(units), ["kernel/lib/x", "system/lib/y"])
        self.assertEqual(units["kernel/lib/x"]["lines"], [10, 20])
        self.assertEqual(units["kernel/lib/x"]["functions"], [2, 4])
        self.assertEqual(total["lines"], [15, 25])
        self.assertEqual(total["branches"], [1, 4])

    def test_render_shows_percentages_a_dash_for_nothing_and_the_total(self):
        units = {"kernel/lib/x": {"lines": [10, 20], "functions": [2, 4],
                                  "regions": [0, 0], "branches": [0, 0]}}
        total = {"lines": [10, 20], "functions": [2, 4], "regions": [0, 0], "branches": [0, 0]}
        text = cov_host.render(units, total)
        self.assertIn("kernel/lib/x", text)
        self.assertIn("50.0%", text)
        self.assertIn("10/20", text)
        self.assertIn("TOTAL", text.splitlines()[-1])
        self.assertRegex(text.splitlines()[2], r"-\s+-")

    def test_what_is_left_out_of_the_numbers(self):
        ignored = re.compile(cov_host.ignore_regex(False))
        for name in ("../../vendor/googletest/x.cc", "../../prebuilt/clang/llvm-23/include/c++/v1/a",
                     "kernel/lib/x/test/a_test.cpp", "/Library/Developer/SDK/a.h",
                     "../../benches/b.cpp", "/x/out/cov/gen/g.cpp"):
            self.assertTrue(ignored.search(name), name)
        for name in ("../../kernel/lib/x/src/a.cpp", "kernel/lib/x/include/x/a.hpp",
                     "../../system/lib/ustl/include/ustl/vector.hpp"):
            self.assertFalse(ignored.search(name), name)
        with_tests = re.compile(cov_host.ignore_regex(True))
        self.assertFalse(with_tests.search("kernel/lib/x/test/a_test.cpp"))
        self.assertTrue(with_tests.search("../../vendor/googletest/x.cc"))

    def test_a_directory_without_the_host_selector_is_refused(self):
        touch(self.out / "build.ninja")
        touch(self.out / "args.gn", 'target_cpu = "x86"\n')
        with self.assertRaisesRegex(cov_host.CovError, "not configured for host coverage"):
            cov_host.ensure_configured(self.root, self.out)
        touch(self.out / "args.gn",
              'select_variant = [{variant="coverage" host=true}]\n')
        cov_host.ensure_configured(self.root, self.out)
        touch(self.out / "args.gn", 'select_variant = ["coverage"]\n')
        with self.assertRaises(cov_host.CovError):
            cov_host.ensure_configured(self.root, self.out)

    def fake_gn(self, host):
        """GN's answers for the old rule: t1 and t2 are tests //:default builds."""
        answers = {
            "ls": [f"//a:t1{host}", f"//a:t2{host}", f"//benches/b:bench{host}",
                   "//a:t3(//build/toolchain/ours:user-x86)", f"//a:unreached{host}"],
            "desc": [f"//a:t1{host}", f"//a:t2{host}", f"//benches/b:bench{host}",
                     "//a:t3(//build/toolchain/ours:user-x86)", "//other:thing"],
            "outputs": ["host/obj/a/t1", "host/obj/a/t2"],
        }
        calls = []

        def fake(root, *args):
            calls.append(args)
            return answers[args[0]]

        return fake, calls

    def declare(self, *entries):
        (self.out / cov_host.HOST_TESTS_FILE).write_text(json.dumps(
            [{"label": label, "binary": binary} for label, binary in entries]))

    def test_without_declarations_the_tests_are_what_gn_says_the_default_build_reaches(self):
        host = f"({cov_host.HOST_TOOLCHAIN})"
        fake, calls = self.fake_gn(host)
        with mock.patch.object(cov_host, "gn", fake):
            tests, declared = cov_host.host_tests(self.root, self.out)
        self.assertEqual(tests, [(f"//a:t1{host}", "host/obj/a/t1"),
                                 (f"//a:t2{host}", "host/obj/a/t2")])
        self.assertEqual(declared, 0)
        self.assertEqual(calls[-1][2:], (f"//a:t1{host}", f"//a:t2{host}"))

    def test_declared_tests_come_first_and_are_not_found_a_second_time(self):
        host = f"({cov_host.HOST_TOOLCHAIN})"
        fake, _ = self.fake_gn(host)
        self.declare((f"//a:t2{host}", "host/obj/a/t2"), (f"//c:declared{host}", "host/obj/c/declared"))
        with mock.patch.object(cov_host, "gn", fake):
            tests, declared = cov_host.host_tests(self.root, self.out)
        self.assertEqual(tests, [(f"//a:t2{host}", "host/obj/a/t2"),
                                 (f"//c:declared{host}", "host/obj/c/declared"),
                                 (f"//a:t1{host}", "host/obj/a/t1")])
        self.assertEqual(declared, 2)

    def test_a_declaration_in_another_toolchain_is_not_a_host_test(self):
        host = f"({cov_host.HOST_TOOLCHAIN})"
        self.declare(("//a:t9(//build/toolchain/ours:kernel-x86)", "kernel/obj/a/t9"),
                     (f"//a:t8{host}", "host/obj/a/t8"))
        self.assertEqual(cov_host.declared_tests(self.out), [(f"//a:t8{host}", "host/obj/a/t8")])

    def test_declared_tests_alone_are_enough(self):
        host = f"({cov_host.HOST_TOOLCHAIN})"
        self.declare((f"//a:t1{host}", "host/obj/a/t1"))
        with mock.patch.object(cov_host, "gn", lambda root, *args: []):
            tests, declared = cov_host.host_tests(self.root, self.out)
        self.assertEqual((tests, declared), ([(f"//a:t1{host}", "host/obj/a/t1")], 1))

    def test_no_host_tests_is_an_error(self):
        with mock.patch.object(cov_host, "gn", lambda root, *args: []):
            with self.assertRaisesRegex(cov_host.CovError, "no host tests"):
                cov_host.host_tests(self.root, self.out)

    def test_each_test_gets_its_own_profile_and_a_failure_is_reported(self):
        def program(name, exit_code):
            path = touch(self.out / name,
                         '#!/bin/sh\nf=$(echo "$LLVM_PROFILE_FILE" | sed "s/%p/1/")\n'
                         f'echo data > "$f"\nexit {exit_code}\n')
            path.chmod(0o755)

        program("ok", 0)
        program("bad", 3)
        profiles = self.out / "coverage" / "profraw"
        touch(profiles / "stale.profraw", "from an older build")
        failed = cov_host.run_tests(self.out, [("//:ok", "ok"), ("//:bad", "bad")], profiles, 2)
        self.assertEqual([label for label, _ in failed], ["//:bad"])
        self.assertIn("exit 3", failed[0][1])
        self.assertEqual(sorted(p.name for p in profiles.glob("*.profraw")),
                         ["000-1.profraw", "001-1.profraw"])


if __name__ == "__main__":
    unittest.main()
