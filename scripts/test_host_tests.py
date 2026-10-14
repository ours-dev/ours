import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).parent))

import host_tests

HOST = f"({host_tests.HOST_TOOLCHAIN})"


def fake_gn(answers):
    calls = []

    def fake(root, *args):
        calls.append(args)
        return answers[args[0]]

    return fake, calls


class Find(unittest.TestCase):
    def setUp(self):
        self.root = Path(tempfile.mkdtemp())
        self.out = self.root / "out"

    def test_the_tests_are_the_host_testonly_executables_the_default_build_reaches(self):
        fake, calls = fake_gn({
            "ls": [f"//a:t1{HOST}", f"//a:t2{HOST}", f"//benches/b:bench{HOST}",
                   "//a:t3(//build/toolchain/ours:user-x86)", f"//a:unreached{HOST}"],
            "desc": [f"//a:t1{HOST}", f"//a:t2{HOST}", f"//benches/b:bench{HOST}",
                     "//a:t3(//build/toolchain/ours:user-x86)", "//other:thing"],
            "outputs": ["host/obj/a/t1", "host/obj/a/t2"],
        })
        with mock.patch.object(host_tests, "gn", fake):
            tests = host_tests.find(self.root, self.out)
        self.assertEqual(tests, [(f"//a:t1{HOST}", "host/obj/a/t1"),
                                 (f"//a:t2{HOST}", "host/obj/a/t2")])
        self.assertEqual(calls[-1][2:], (f"//a:t1{HOST}", f"//a:t2{HOST}"))

    def test_none_reached_is_an_empty_list_and_asks_for_no_files(self):
        fake, calls = fake_gn({"ls": [f"//a:t1{HOST}"], "desc": ["//other:thing"]})
        with mock.patch.object(host_tests, "gn", fake):
            self.assertEqual(host_tests.find(self.root, self.out), [])
        self.assertNotIn("outputs", [call[0] for call in calls])

    def test_a_file_count_that_does_not_match_the_tests_is_an_error(self):
        fake, _ = fake_gn({"ls": [f"//a:t1{HOST}", f"//a:t2{HOST}"],
                           "desc": [f"//a:t1{HOST}", f"//a:t2{HOST}"],
                           "outputs": ["host/obj/a/t1"]})
        with mock.patch.object(host_tests, "gn", fake):
            with self.assertRaisesRegex(host_tests.HostTestsError, "gn outputs gave 1 files"):
                host_tests.find(self.root, self.out)

    def test_it_is_a_runtime_error_so_boot_check_s_handler_catches_it(self):
        self.assertTrue(issubclass(host_tests.HostTestsError, RuntimeError))


if __name__ == "__main__":
    unittest.main()
