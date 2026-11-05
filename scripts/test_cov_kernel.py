from __future__ import annotations

import base64
import shutil
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))

import cov_kernel


def format_dump(data: bytes, chunk: int = 144, checksum: int | None = None) -> list[str]:
    """The lines kernel/commands/cov.cpp prints for `data`."""
    if checksum is None:
        checksum = cov_kernel.fnv1a64(data)
    lines = [f"COV-BEGIN {len(data)} {chunk}"]
    for offset in range(0, len(data), chunk):
        encoded = base64.b64encode(data[offset:offset + chunk]).decode()
        lines.append(f"COV {offset} {encoded}")
    lines.append(f"COV-END {len(data)} {checksum:016x}")
    return lines


def profile(size: int) -> bytes:
    return bytes((i * 7 + 3) & 0xff for i in range(size))


NOISE = "[6371029000 cpu2 debug] I'm Thread 1"


class FNV(unittest.TestCase):
    def test_known_vectors(self):
        self.assertEqual(cov_kernel.fnv1a64(b""), 0xcbf29ce484222325)
        self.assertEqual(cov_kernel.fnv1a64(b"a"), 0xaf63dc4c8601ec8c)
        self.assertEqual(cov_kernel.fnv1a64(b"foobar"), 0x85944171f73967e8)


class ParseDump(unittest.TestCase):
    def test_a_clean_dump_comes_back_whole(self):
        data = profile(1000)
        self.assertEqual(cov_kernel.parse_dump("\n".join(format_dump(data))), data)

    def test_the_last_chunk_may_be_short_and_the_size_a_multiple_of_the_chunk(self):
        for size in (1, 143, 144, 145, 288, 290):
            data = profile(size)
            self.assertEqual(cov_kernel.parse_dump("\n".join(format_dump(data))), data, size)

    def test_other_cpus_lines_between_ours_are_ignored(self):
        data = profile(700)
        lines = format_dump(data)
        mixed = []
        for line in lines:
            mixed += [NOISE, line, NOISE]
        self.assertEqual(cov_kernel.parse_dump("\r\n".join(mixed)), data)

    def test_the_prompt_and_the_echo_may_precede_the_first_line(self):
        data = profile(300)
        lines = format_dump(data)
        lines[0] = "kshell> cov dump" + lines[0]
        lines[1] = "kshell> " + lines[1]
        self.assertEqual(cov_kernel.parse_dump("\n".join(lines)), data)

    def test_a_line_another_writer_broke_makes_the_dump_not_add_up(self):
        data = profile(700)
        lines = format_dump(data)
        lines[2] = lines[2][:20] + NOISE + lines[2][20:]
        self.assertIsNone(cov_kernel.parse_dump("\n".join(lines)))

    def test_a_missing_line_is_caught_by_the_offsets(self):
        data = profile(700)
        lines = format_dump(data)
        del lines[3]
        self.assertIsNone(cov_kernel.parse_dump("\n".join(lines)))

    def test_a_wrong_checksum_is_caught(self):
        data = profile(700)
        self.assertIsNone(
            cov_kernel.parse_dump("\n".join(format_dump(data, checksum=cov_kernel.fnv1a64(data) ^ 1))))

    def test_a_line_that_ends_short_is_caught(self):
        data = profile(700)
        lines = format_dump(data)
        lines[2] = lines[2][:-8]
        self.assertIsNone(cov_kernel.parse_dump("\n".join(lines)))

    def test_no_end_line_is_no_dump(self):
        self.assertIsNone(cov_kernel.parse_dump("\n".join(format_dump(profile(500))[:-1])))

    def test_a_flipped_byte_inside_a_line_is_caught_by_the_checksum(self):
        data = profile(500)
        lines = format_dump(data)
        other = bytearray(data)
        other[10] ^= 0xff
        lines[1] = format_dump(bytes(other))[1]
        self.assertIsNone(cov_kernel.parse_dump("\n".join(lines)))

    def test_the_last_complete_dump_wins_and_a_broken_later_one_does_not_hide_it(self):
        first, second = profile(400), profile(600)
        text = "\n".join(format_dump(first) + [NOISE] + format_dump(second))
        self.assertEqual(cov_kernel.parse_dump(text), second)
        broken = format_dump(second)
        del broken[2]
        text = "\n".join(format_dump(first) + [NOISE] + broken)
        self.assertEqual(cov_kernel.parse_dump(text), first)

    def test_lines_before_a_begin_are_not_part_of_any_dump(self):
        data = profile(300)
        stray = format_dump(profile(300))[1]
        self.assertEqual(cov_kernel.parse_dump("\n".join([stray] + format_dump(data))), data)

    def test_an_empty_profile_is_not_a_dump(self):
        self.assertIsNone(cov_kernel.parse_dump(""))
        self.assertEqual(cov_kernel.parse_dump("\n".join(format_dump(b""))), b"")


class Setup(unittest.TestCase):
    def setUp(self):
        self.root = Path(tempfile.mkdtemp()).resolve()
        self.addCleanup(shutil.rmtree, self.root, ignore_errors=True)

    def test_test_commands_come_from_where_they_are_registered(self):
        commands = self.root / "kernel" / "commands"
        commands.mkdir(parents=True)
        (commands / "a.cpp").write_text(
            'GKTL_COMMAND(VmaWxTest, "vmawx.test", ours::vmawx_test_command,\n  "help")\n'
            'GKTL_COMMAND(\n    SleepTest,\n    "sleep.test",\n    f, "h")\n'
            'GKTL_COMMAND(LogDump, "log", log_command, "help")\n')
        (commands / "b.cpp").write_text('GKTL_COMMAND(Again, "sleep.test", f, "h")\n'
                                        'GKTL_COMMAND(Port, "port.test", f, "h")\n')
        self.assertEqual(cov_kernel.test_commands(self.root),
                         ["vmawx.test", "sleep.test", "port.test"])

    def test_a_directory_without_the_kernel_variant_is_refused(self):
        out = self.root / "out"
        out.mkdir()
        (out / "build.ninja").write_text("")
        (out / "args.gn").write_text('target_cpu = "x86"\n')
        with self.assertRaisesRegex(cov_kernel.KernelCovError, "not configured for kernel coverage"):
            cov_kernel.ensure_configured(self.root, out)
        (out / "args.gn").write_text('select_variant = ["lto"]\n')
        with self.assertRaises(cov_kernel.KernelCovError):
            cov_kernel.ensure_configured(self.root, out)
        (out / "args.gn").write_text('select_variant = ["coverage-kernel"]\n')
        cov_kernel.ensure_configured(self.root, out)

    def test_the_kernel_elf_must_exist(self):
        with self.assertRaisesRegex(cov_kernel.KernelCovError, "is missing"):
            cov_kernel.kernel_elf(self.root)
        elf = self.root / cov_kernel.KERNEL_ELF
        elf.parent.mkdir(parents=True)
        elf.write_text("")
        self.assertEqual(cov_kernel.kernel_elf(self.root), elf)


class KtestCommands(unittest.TestCase):
    def test_turns_a_list_into_run_commands(self):
        text = "\n".join([
            "   [6351527000 cpu0 info] ktest: TEST Ktest.AssertionsPass",
            "   [6351664000 cpu0 info] ktest: TEST SamplerClaim.HoldsClaimUntilMemoryIsGone",
            "kshell> ",
        ])
        self.assertEqual(cov_kernel.ktest_commands(text),
                         ["ktest Ktest.AssertionsPass",
                          "ktest SamplerClaim.HoldsClaimUntilMemoryIsGone"])

    def test_a_kernel_without_the_interface_answers_nothing(self):
        self.assertEqual(cov_kernel.ktest_commands("kshell> "), [])


if __name__ == "__main__":
    unittest.main()
