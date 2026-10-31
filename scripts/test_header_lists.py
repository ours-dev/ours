import json
import os
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).parent))

import header_lists

KERNEL = "//build/switch/set/dbg.1:kernel_x86.dbg.1"
HOST = "//build/toolchain/ours:host"


def interface(label, public, toolchain=KERNEL, **extra):
    """What `gn desc` says about a cpp_interface: the label as GN prints it
    (a toolchain suffix unless it is the default toolchain's)."""
    base = label.split("(")[0]
    target = {"type": "source_set", "toolchain": toolchain, "public": public,
              "public_configs": [base + ".config"]}
    target.update(extra)
    return {label: target}


class Tree(unittest.TestCase):
    """A repository root with files under it, and a fake `gn desc`."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.out = self.root / "out"

    def touch(self, *paths):
        for path in paths:
            file = self.root / path
            file.parent.mkdir(parents=True, exist_ok=True)
            file.write_text("")

    def run_check(self, targets, roots, scope=header_lists.DEFAULT_SCOPE):
        """`roots` maps a config label to its include directories, as GN says."""
        def fake(root, *args):
            self.assertEqual(args[0], "desc")
            query = args[2]
            if query == "//*":
                return json.dumps(targets)
            config = query.split("(")[0]
            return json.dumps({config: {"include_dirs": roots[config]}})

        with mock.patch.object(header_lists, "gn", fake):
            return header_lists.check(self.root, self.out, scope)


class Check(Tree):
    def test_a_list_that_is_the_whole_root_passes(self):
        self.touch("kernel/arch/x86/include/a.hpp", "kernel/arch/x86/include/sub/b.hpp")
        targets = interface(f"//kernel/arch/x86:headers({KERNEL})",
                            ["//kernel/arch/x86/include/a.hpp", "//kernel/arch/x86/include/sub/b.hpp"])
        result = self.run_check(targets, {"//kernel/arch/x86:headers.config": ["//kernel/arch/x86/include/"]})
        self.assertTrue(result.ok)
        self.assertEqual((result.targets, result.roots, result.headers), (1, 1, 2))

    def test_a_file_in_the_root_that_the_list_lacks_is_missing_and_named(self):
        self.touch("kernel/arch/x86/include/a.hpp", "kernel/arch/x86/include/new.hpp")
        targets = interface(f"//kernel/arch/x86:headers({KERNEL})", ["//kernel/arch/x86/include/a.hpp"])
        result = self.run_check(targets, {"//kernel/arch/x86:headers.config": ["//kernel/arch/x86/include/"]})
        self.assertEqual([(p.kind, p.path, p.targets) for p in result.problems],
                         [("missing", "//kernel/arch/x86/include/new.hpp", ("//kernel/arch/x86:headers",))])
        self.assertIn("new.hpp", result.problems[0].describe())

    def test_an_entry_with_no_file_is_stray_and_named(self):
        self.touch("kernel/arch/x86/include/a.hpp")
        targets = interface(f"//kernel/arch/x86:headers({KERNEL})",
                            ["//kernel/arch/x86/include/a.hpp", "//kernel/arch/x86/include/gone.hpp"])
        result = self.run_check(targets, {"//kernel/arch/x86:headers.config": ["//kernel/arch/x86/include/"]})
        self.assertEqual([(p.kind, p.path) for p in result.problems],
                         [("stray", "//kernel/arch/x86/include/gone.hpp")])

    def test_a_real_file_outside_the_root_is_stray_too(self):
        self.touch("kernel/arch/x86/include/a.hpp", "kernel/arch/x86/other/b.hpp")
        targets = interface(f"//kernel/arch/x86:headers({KERNEL})",
                            ["//kernel/arch/x86/include/a.hpp", "//kernel/arch/x86/other/b.hpp"])
        result = self.run_check(targets, {"//kernel/arch/x86:headers.config": ["//kernel/arch/x86/include/"]})
        self.assertEqual([(p.kind, p.path) for p in result.problems],
                         [("stray", "//kernel/arch/x86/other/b.hpp")])

    def test_targets_sharing_a_root_are_judged_by_the_union_of_their_lists(self):
        self.touch("kernel/arch/x86/include/a.hpp", "kernel/arch/x86/include/b.hpp",
                   "kernel/arch/x86/include/c.hpp")
        targets = {}
        targets.update(interface(f"//kernel/arch/x86:one({KERNEL})", ["//kernel/arch/x86/include/a.hpp"]))
        targets.update(interface(f"//kernel/arch/x86:two({KERNEL})", ["//kernel/arch/x86/include/b.hpp"]))
        roots = {"//kernel/arch/x86:one.config": ["//kernel/arch/x86/include/"],
                 "//kernel/arch/x86:two.config": ["//kernel/arch/x86/include/"]}
        result = self.run_check(targets, roots)
        # c.hpp is in neither list, and the finding names both targets that publish the root.
        self.assertEqual([(p.kind, p.path, p.targets) for p in result.problems],
                         [("missing", "//kernel/arch/x86/include/c.hpp",
                           ("//kernel/arch/x86:one", "//kernel/arch/x86:two"))])
        self.assertEqual((result.targets, result.roots), (2, 1))

    def test_a_stray_entry_names_the_one_target_that_lists_it(self):
        self.touch("kernel/arch/x86/include/a.hpp", "kernel/arch/x86/include/b.hpp")
        targets = {}
        targets.update(interface(f"//kernel/arch/x86:one({KERNEL})",
                                 ["//kernel/arch/x86/include/a.hpp", "//kernel/arch/x86/include/typo.hpp"]))
        targets.update(interface(f"//kernel/arch/x86:two({KERNEL})", ["//kernel/arch/x86/include/b.hpp"]))
        roots = {"//kernel/arch/x86:one.config": ["//kernel/arch/x86/include/"],
                 "//kernel/arch/x86:two.config": ["//kernel/arch/x86/include/"]}
        result = self.run_check(targets, roots)
        self.assertEqual([(p.kind, p.targets) for p in result.problems],
                         [("stray", ("//kernel/arch/x86:one",))])

    def test_the_same_problem_in_two_toolchains_is_reported_once(self):
        self.touch("kernel/arch/x86/include/a.hpp", "kernel/arch/x86/include/new.hpp")
        targets = {}
        for toolchain in (KERNEL, "//build/switch/set/dbg.0:kernel_x86.dbg.0"):
            targets.update(interface(f"//kernel/arch/x86:headers({toolchain})",
                                     ["//kernel/arch/x86/include/a.hpp"], toolchain=toolchain))
        result = self.run_check(targets, {"//kernel/arch/x86:headers.config": ["//kernel/arch/x86/include/"]})
        self.assertEqual(len(result.problems), 1)
        self.assertEqual((result.targets, result.roots), (1, 1))

    def test_a_file_is_one_problem_when_the_publishers_of_its_root_differ_by_toolchain(self):
        # Two targets publish the root in the kernel toolchain and one in the
        # host's (phys's three header targets are like this): new.hpp is listed
        # by none, in either, and is one file.
        self.touch("kernel/arch/x86/include/a.hpp", "kernel/arch/x86/include/new.hpp")
        listed = ["//kernel/arch/x86/include/a.hpp"]
        targets = {}
        targets.update(interface(f"//kernel/arch/x86:one({KERNEL})", listed))
        targets.update(interface(f"//kernel/arch/x86:two({KERNEL})", listed))
        targets.update(interface(f"//kernel/arch/x86:one({HOST})", listed, toolchain=HOST))
        roots = {"//kernel/arch/x86:one.config": ["//kernel/arch/x86/include/"],
                 "//kernel/arch/x86:two.config": ["//kernel/arch/x86/include/"]}
        result = self.run_check(targets, roots)
        self.assertEqual([(p.kind, p.path, p.targets) for p in result.problems],
                         [("missing", "//kernel/arch/x86/include/new.hpp",
                           ("//kernel/arch/x86:one", "//kernel/arch/x86:two"))])
        self.assertTrue(header_lists.summary(result).startswith("1 missing, 0 stray among "))

    def test_a_problem_in_only_one_toolchain_is_still_found(self):
        self.touch("kernel/arch/x86/include/a.hpp", "kernel/arch/x86/include/b.hpp")
        targets = {}
        targets.update(interface(f"//kernel/arch/x86:headers({KERNEL})",
                                 ["//kernel/arch/x86/include/a.hpp", "//kernel/arch/x86/include/b.hpp"]))
        targets.update(interface(f"//kernel/arch/x86:headers({HOST})", ["//kernel/arch/x86/include/a.hpp"],
                                 toolchain=HOST))
        result = self.run_check(targets, {"//kernel/arch/x86:headers.config": ["//kernel/arch/x86/include/"]})
        self.assertEqual([p.path for p in result.problems], ["//kernel/arch/x86/include/b.hpp"])

    def test_dotfiles_are_not_headers(self):
        self.touch("kernel/arch/x86/include/a.hpp", "kernel/arch/x86/include/.DS_Store")
        targets = interface(f"//kernel/arch/x86:headers({KERNEL})", ["//kernel/arch/x86/include/a.hpp"])
        self.assertTrue(self.run_check(
            targets, {"//kernel/arch/x86:headers.config": ["//kernel/arch/x86/include/"]}).ok)

    def test_only_files_named_like_headers_have_to_be_listed(self):
        self.touch("system/public/a.hpp", "system/public/BUILD.gn", "system/public/syscalls.gni",
                   "system/public/impl.tcc")
        targets = interface(f"//kernel/arch/x86:headers({KERNEL})", ["//system/public/a.hpp"])
        result = self.run_check(targets, {"//kernel/arch/x86:headers.config": ["//system/public/"]})
        # BUILD.gn and the .gni are not headers; the .tcc is.
        self.assertEqual([p.path for p in result.problems], ["//system/public/impl.tcc"])

    def test_a_listed_file_that_is_in_the_root_is_not_stray_whatever_its_name(self):
        self.touch("kernel/arch/x86/include/a.hpp", "kernel/arch/x86/include/table.def")
        targets = interface(f"//kernel/arch/x86:headers({KERNEL})",
                            ["//kernel/arch/x86/include/a.hpp", "//kernel/arch/x86/include/table.def"])
        self.assertTrue(self.run_check(
            targets, {"//kernel/arch/x86:headers.config": ["//kernel/arch/x86/include/"]}).ok)

    def test_a_custom_include_dir_is_the_root(self):
        self.touch("kernel/arch/x86/lib/arch/include/arch/p.hpp", "kernel/arch/x86/BUILD.gn")
        targets = interface(f"//kernel/arch/x86:headers({KERNEL})",
                            ["//kernel/arch/x86/lib/arch/include/arch/p.hpp"])
        result = self.run_check(
            targets, {"//kernel/arch/x86:headers.config": ["//kernel/arch/x86/lib/arch/include/"]})
        self.assertTrue(result.ok)  # BUILD.gn is beside the root, not under it


class What(Tree):
    def test_only_targets_in_scope_are_held_to_the_list(self):
        self.touch("kernel/lib/x/include/a.hpp", "kernel/lib/x/include/b.hpp", "kernel/arch/x86/include/c.hpp")
        targets = interface(f"//kernel/lib/x:headers({KERNEL})", ["//kernel/lib/x/include/a.hpp"])
        targets.update(interface(f"//kernel/arch/x86:headers({KERNEL})", ["//kernel/arch/x86/include/c.hpp"]))
        roots = {"//kernel/lib/x:headers.config": ["//kernel/lib/x/include/"],
                 "//kernel/arch/x86:headers.config": ["//kernel/arch/x86/include/"]}
        narrow = self.run_check(targets, roots)
        self.assertTrue(narrow.ok)
        self.assertEqual(narrow.targets, 1)  # b.hpp is unlisted, in a target the scope does not reach
        wide = self.run_check(targets, roots, header_lists.ALL)
        self.assertEqual([p.path for p in wide.problems], ["//kernel/lib/x/include/b.hpp"])

    def test_the_default_scope_holds_libarchs_generic_half_to_its_list_too(self):
        # <arch/...> has two halves: each CPU's under kernel/arch/<cpu>/lib/arch,
        # and the generic one in //kernel/lib/arch. A header added to the second
        # and left out of its list is as invisible to `gn check` as to the first.
        self.touch("kernel/lib/arch/include/arch/a.hpp", "kernel/lib/arch/include/arch/new.hpp")
        targets = interface(f"//kernel/lib/arch:headers({KERNEL})", ["//kernel/lib/arch/include/arch/a.hpp"])
        roots = {"//kernel/lib/arch:headers.config": ["//kernel/lib/arch/include/"]}
        result = self.run_check(targets, roots)
        self.assertEqual([(p.kind, p.path, p.targets) for p in result.problems],
                         [("missing", "//kernel/lib/arch/include/arch/new.hpp", ("//kernel/lib/arch:headers",))])

    def test_the_default_scope_reaches_the_directory_of_libarch_and_no_neighbour_of_it(self):
        # The prefix ends in a colon, so a sibling whose name only begins with
        # "arch" (arch_foo, archive) and a subdirectory of libarch are not held
        # to the rule.
        targets = {}
        for label in ("//kernel/lib/arch:headers", "//kernel/lib/arch_foo:headers",
                      "//kernel/lib/archive:headers", "//kernel/lib/arch/sub:headers"):
            targets.update(interface(f"{label}({KERNEL})", []))
        found = header_lists.interfaces(targets, header_lists.DEFAULT_SCOPE)
        self.assertEqual(sorted(header_lists.without_toolchain(label) for label in found),
                         ["//kernel/lib/arch:headers"])

    def test_a_library_that_compiles_sources_is_not_a_header_interface(self):
        self.touch("kernel/arch/x86/include/a.hpp", "kernel/arch/x86/include/b.hpp")
        targets = interface(f"//kernel/arch/x86:lib({KERNEL})", ["//kernel/arch/x86/include/a.hpp"],
                            sources=["//kernel/arch/x86/lib.cpp"])
        self.assertEqual(header_lists.interfaces(targets, header_lists.DEFAULT_SCOPE), {})

    def test_a_source_set_whose_public_is_the_default_is_not_either(self):
        self.touch("kernel/arch/x86/include/a.hpp")
        targets = interface(f"//kernel/arch/x86:lib({KERNEL})", "*")
        self.assertEqual(header_lists.interfaces(targets, header_lists.DEFAULT_SCOPE), {})

    def test_a_source_set_that_publishes_no_config_of_its_own_is_not_one(self):
        self.touch("kernel/arch/x86/include/a.hpp")
        targets = interface(f"//kernel/arch/x86:lib({KERNEL})", ["//kernel/arch/x86/include/a.hpp"])
        targets[f"//kernel/arch/x86:lib({KERNEL})"]["public_configs"] = ["//somewhere/else:other.config"]
        self.assertEqual(header_lists.interfaces(targets, header_lists.DEFAULT_SCOPE), {})

    def test_other_target_types_are_ignored(self):
        targets = {f"//kernel/arch/x86:g({KERNEL})": {"type": "group", "toolchain": KERNEL}}
        self.assertEqual(header_lists.interfaces(targets, header_lists.DEFAULT_SCOPE), {})

    def test_a_scope_that_matches_no_cpp_interface_is_an_error_not_a_pass(self):
        self.touch("kernel/arch/x86/include/a.hpp")
        targets = interface(f"//kernel/arch/x86:headers({KERNEL})", ["//kernel/arch/x86/include/a.hpp"])
        roots = {"//kernel/arch/x86:headers.config": ["//kernel/arch/x86/include/"]}
        self.assertTrue(self.run_check(targets, roots).ok)
        with self.assertRaisesRegex(header_lists.HeaderListsError,
                                    r"no cpp_interface target has a label under //kernel/arch/typo/"):
            self.run_check(targets, roots, ("//kernel/arch/typo/",))

    def test_a_scope_that_ends_in_a_slash_does_not_reach_the_target_of_the_directory_itself(self):
        # //kernel/mm:headers does not start with //kernel/mm/, which is how a
        # repair item came to name a scope that matched nothing.
        self.touch("kernel/mm/include/a.hpp")
        targets = interface(f"//kernel/mm:headers({KERNEL})", [])
        roots = {"//kernel/mm:headers.config": ["//kernel/mm/include/"]}
        with self.assertRaises(header_lists.HeaderListsError):
            self.run_check(targets, roots, ("//kernel/mm/",))
        self.assertEqual([p.path for p in self.run_check(targets, roots, ("//kernel/mm:",)).problems],
                         ["//kernel/mm/include/a.hpp"])

    def test_a_graph_with_no_cpp_interface_at_all_is_an_error_under_every_scope(self):
        for scope in (header_lists.DEFAULT_SCOPE, header_lists.ALL):
            with self.assertRaises(header_lists.HeaderListsError):
                self.run_check({}, {}, scope)

    def test_a_config_with_two_include_directories_is_an_error_not_a_guess(self):
        self.touch("kernel/arch/x86/include/a.hpp")
        targets = interface(f"//kernel/arch/x86:headers({KERNEL})", ["//kernel/arch/x86/include/a.hpp"])
        roots = {"//kernel/arch/x86:headers.config": ["//kernel/arch/x86/include/", "//kernel/arch/x86/x/"]}
        with self.assertRaisesRegex(header_lists.HeaderListsError, "publishes 2 include directories"):
            self.run_check(targets, roots)


class Report(Tree):
    def test_a_pass_is_one_line_in_boot_check_s_style(self):
        self.touch("kernel/arch/x86/include/a.hpp")
        targets = interface(f"//kernel/arch/x86:headers({KERNEL})", ["//kernel/arch/x86/include/a.hpp"])
        roots = {"//kernel/arch/x86:headers.config": ["//kernel/arch/x86/include/"]}
        printed = []
        with mock.patch("builtins.print", lambda *a: printed.append(" ".join(a))), \
                mock.patch.object(header_lists, "gn", lambda r, *args: json.dumps(
                    targets if args[2] == "//*" else {args[2].split("(")[0]: {"include_dirs": roots[args[2].split("(")[0]]}})):
            ok = header_lists.report(self.root, self.out)
        self.assertTrue(ok)
        self.assertEqual(len(printed), 1)
        self.assertTrue(printed[0].startswith("headers    PASS  1 targets, 1 include roots, 1 headers under //kernel/arch/"))

    def test_the_summary_names_each_prefix_of_the_default_scope(self):
        self.touch("kernel/arch/x86/include/a.hpp", "kernel/lib/arch/include/arch/b.hpp")
        targets = interface(f"//kernel/arch/x86:headers({KERNEL})", ["//kernel/arch/x86/include/a.hpp"])
        targets.update(interface(f"//kernel/lib/arch:headers({KERNEL})", ["//kernel/lib/arch/include/arch/b.hpp"]))
        roots = {"//kernel/arch/x86:headers.config": ["//kernel/arch/x86/include/"],
                 "//kernel/lib/arch:headers.config": ["//kernel/lib/arch/include/"]}
        result = self.run_check(targets, roots)
        self.assertTrue(result.ok)
        self.assertEqual((result.targets, result.roots, result.headers), (2, 2, 2))
        self.assertIn("2 headers under //kernel/arch/ and //kernel/lib/arch:, every one listed",
                      header_lists.summary(result))

    def test_a_gn_failure_is_a_fail_line_not_a_traceback(self):
        def broken(root, *args):
            raise header_lists.HeaderListsError("gn desc: no such build directory")

        printed = []
        with mock.patch("builtins.print", lambda *a: printed.append(" ".join(a))), \
                mock.patch.object(header_lists, "gn", broken):
            self.assertFalse(header_lists.report(self.root, self.out))
        self.assertEqual(printed, ["headers    FAIL  gn desc: no such build directory"])

    def test_an_empty_scope_is_a_fail_line_naming_it_not_a_pass(self):
        self.touch("kernel/arch/x86/include/a.hpp")
        targets = interface(f"//kernel/arch/x86:headers({KERNEL})", ["//kernel/arch/x86/include/a.hpp"])
        printed = []
        with mock.patch("builtins.print", lambda *a: printed.append(" ".join(a))), \
                mock.patch.object(header_lists, "gn", lambda r, *args: json.dumps(targets)):
            self.assertFalse(header_lists.report(self.root, self.out, ("//kernel/arch/typo/",)))
        self.assertEqual(len(printed), 1)
        self.assertTrue(printed[0].startswith("headers    FAIL  no cpp_interface target has a label under "
                                              "//kernel/arch/typo/"))

    def test_it_is_a_runtime_error_so_boot_check_s_handler_catches_it(self):
        self.assertTrue(issubclass(header_lists.HeaderListsError, RuntimeError))


class Main(unittest.TestCase):
    def test_the_root_is_the_checkout_holding_the_script_not_the_working_directory_s(self):
        # A temporary directory is in no repository: the git-based root this
        # replaced raised CalledProcessError here, and from another checkout's
        # directory it named that checkout.
        previous = os.getcwd()
        with tempfile.TemporaryDirectory() as elsewhere, \
                mock.patch.object(header_lists, "report", return_value=True) as report:
            os.chdir(elsewhere)
            try:
                code = header_lists.main(["out/bringup.x86"])
            finally:
                os.chdir(previous)
        self.assertEqual(code, 0)
        root = report.call_args.args[0]
        self.assertEqual(root, Path(header_lists.__file__).resolve().parents[1])
        self.assertTrue((root / "scripts" / "header_lists.py").is_file())
        self.assertEqual(report.call_args.args[1], (root / "out/bringup.x86").resolve())


if __name__ == "__main__":
    unittest.main()
