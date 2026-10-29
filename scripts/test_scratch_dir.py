import hashlib
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))

import scratch_dir


class ScratchDirPath(unittest.TestCase):
    def test_one_build_directory_spelled_three_ways_gets_one_directory(self):
        # cov_kernel.py passes ROOT / --out unresolved, qemu_boot.py whatever
        # OUT says, boot_check.py a resolved path: all must meet.
        relative = scratch_dir.path("p", Path("out/x"))
        absolute = scratch_dir.path("p", Path.cwd() / "out/x")
        dotted = scratch_dir.path("p", Path.cwd() / "out/../out/x")
        self.assertEqual(relative, absolute)
        self.assertEqual(relative, dotted)

    def test_a_symlink_to_a_build_directory_gets_that_directory_s_name(self):
        with tempfile.TemporaryDirectory() as base:
            real = Path(base) / "real" / "x"
            real.mkdir(parents=True)
            link = Path(base) / "link"
            link.symlink_to(real)
            self.assertEqual(scratch_dir.path("p", link), scratch_dir.path("p", real))

    def test_two_worktrees_out_directories_of_one_name_get_two_directories(self):
        first = scratch_dir.path("p", Path("/a/out/x"))
        second = scratch_dir.path("p", Path("/b/out/x"))
        self.assertNotEqual(first, second)
        self.assertTrue(first.name.startswith("p-x-"))
        self.assertTrue(second.name.startswith("p-x-"))

    def test_the_name_is_the_one_skill_md_gives(self):
        # SKILL.md tells a reader where the last run's log is by this rule, and
        # the riscv mode named its directory by it before the helper existed.
        out = (Path.cwd() / "out/bringup.riscv").resolve()
        digest = hashlib.sha1(str(out).encode()).hexdigest()[:10]
        self.assertEqual(scratch_dir.path("ours-verify", out),
                         Path(tempfile.gettempdir()) / f"ours-verify-bringup.riscv-{digest}")

    def test_nothing_is_created(self):
        with tempfile.TemporaryDirectory() as base:
            result = scratch_dir.path("ours-test-scratch-dir", Path(base) / "never-built")
            self.assertFalse(result.exists())
            self.assertFalse((Path(base) / "never-built").exists())


if __name__ == "__main__":
    unittest.main()
