#!/usr/bin/env python3
"""pack-smoke — round-trips a representative .rsp through the real `obit`
binary: pack it, then confirm `obit validate`/`list`/`extract` agree with
what was packed. Exercises the compressed-BootFS, cmdline, and raw
--type=container embed paths in one pass, and checks that compression is
real: the same files packed with and without lz4 extract to the same bytes,
and the compressed image is the smaller.
"""
from __future__ import annotations

import argparse
import pathlib
import subprocess
import tempfile


def run(args: list[str]) -> subprocess.CompletedProcess:
    return subprocess.run(args, check=True, capture_output=True, text=True)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--stamp", type=pathlib.Path, required=True)
    parser.add_argument("--obit", type=pathlib.Path, required=True)
    args = parser.parse_args()

    with tempfile.TemporaryDirectory() as tmp:
        directory = pathlib.Path(tmp)

        kernel_bin = directory / "kernel.bin"
        setup_yaml = directory / "setup.yaml"
        physload_bin = directory / "physload.bin"
        kernel_bin.write_bytes(b"fake kernel elf payload, long enough to exercise lz4 extension bytes 0123456789")
        setup_yaml.write_bytes(b"setup: true\n")
        physload_bin.write_bytes(b"raw physload bytes, not yet an OBI container")

        manifest = directory / "manifest.fini"
        manifest.write_text(f"kernel={kernel_bin}\nsetup={setup_yaml}\n")

        rsp = directory / "test.rsp"
        rsp.write_text(
            "--reset\n"
            "--files-type=kernel-storage\n"
            "--prefix=ours\n"
            "--compressed=lz4\n"
            "--files\n"
            f"{manifest}\n"
            "--reset\n"
            "--type=cmdline\n"
            "--entry=console=uart\n"
            "--entry=loglevel=info\n"
            "--reset\n"
            "--type=container\n"
            f"{physload_bin}\n"
        )

        out = directory / "out.obi"
        run([str(args.obit), "pack", f"@{rsp}", "--output", str(out)])
        run([str(args.obit), "validate", str(out)])

        listing = run([str(args.obit), "list", str(out)]).stdout
        for expected_name in ("kernel", "cmdline", "physload"):
            assert expected_name in listing, f"{expected_name!r} missing from `obit list` output"

        extracted = directory / "kernel-extracted.bin"
        run([str(args.obit), "extract", str(out), "kernel", str(extracted)])
        assert extracted.stat().st_size > 0, "extracted kernel item is empty"

        extracted_physload = directory / "physload-extracted.bin"
        run([str(args.obit), "extract", str(out), "physload", str(extracted_physload)])
        assert extracted_physload.read_bytes() == physload_bin.read_bytes(), (
            "physload item did not round-trip byte-for-byte"
        )

        # --- Scenario 2: kernel-package merge + --type=executable + --bootable.
        #
        # Mirrors what kernel_image() emits: an Executable item 0 (the
        # physload ELF), an (empty) cmdline entry, and two kernel-storage
        # blocks separated by --reset (one per kernel_package()).  The two
        # blocks must merge into a single "kernel" item, --bootable must
        # accept that topology, and it must reject the same rsp without the
        # executable or with a non-ELF executable.
        physload_elf = directory / "physload.elf"
        physload_elf.write_bytes(b"\x7fELF" + b"\x00" * 64)

        vmours_bin = directory / "vmours-image.elf"
        vmours_bin.write_bytes(b"stripped vmours image")
        kernel_a = directory / "kernel-a.fini"
        kernel_a.write_text(f"vmours/image.elf={vmours_bin}\n")

        physboot_bin = directory / "physboot-module.bin"
        physboot_bin.write_bytes(b"physboot module bytes")
        kernel_b = directory / "kernel-b.fini"
        kernel_b.write_text(f"physboot={physboot_bin}\n")

        def merge_rsp(executable_line: str) -> pathlib.Path:
            path = directory / "merge.rsp"
            path.write_text(
                executable_line
                + "--reset\n"
                "--type=cmdline\n"
                "--entry=\n"
                "--reset\n"
                "--files-type=kernel-storage\n"
                "--compressed=lz4\n"
                "--files\n"
                f"{kernel_a}\n"
                "--reset\n"
                "--files-type=kernel-storage\n"
                "--compressed=lz4\n"
                "--files\n"
                f"{kernel_b}\n"
            )
            return path

        merged = directory / "merged.obi"
        rsp2 = merge_rsp(f"--type=executable\n{physload_elf}\n")
        run([str(args.obit), "pack", f"@{rsp2}", "--output", str(merged), "--bootable=x86"])
        run([str(args.obit), "validate", str(merged)])

        lines = run([str(args.obit), "list", str(merged)]).stdout.splitlines()
        names = [line.split()[1] for line in lines]
        assert names[0] == "physload", f"item 0 is {names[0]!r}, want the executable"
        assert names.count("kernel") == 1, (
            f"kernel-storage blocks must merge into one item, got {names.count('kernel')}"
        )
        assert "cmdline" in names, "empty cmdline must still materialize as an item"

        # --bootable without the executable item: pack fails.
        no_exec = directory / "no-exec.obi"
        rsp_no_exec = merge_rsp("")
        failed = subprocess.run(
            [str(args.obit), "pack", f"@{rsp_no_exec}", "--output", str(no_exec),
             "--bootable=x86"],
            capture_output=True, text=True)
        assert failed.returncode != 0, "--bootable accepted a container with no executable"

        # --type=executable with a non-ELF payload: pack fails.
        bad_exec = directory / "bad-exec.obi"
        rsp_bad_exec = merge_rsp(f"--type=executable\n{physboot_bin}\n")
        failed = subprocess.run(
            [str(args.obit), "pack", f"@{rsp_bad_exec}", "--output", str(bad_exec)],
            capture_output=True, text=True)
        assert failed.returncode != 0, "--type=executable accepted a non-ELF file"

        # --- Scenario 3: real compression.
        #
        # The same files packed with and without --compressed=lz4 must
        # extract to the same bytes, and the compressed image must be the
        # smaller: obit compresses with upstream LZ4 (//vendor/lz4), and the
        # literal-only encoder it replaced made images a few bytes larger,
        # never smaller. The payload is repetitive so that LZ4 has matches to
        # find -- back-references, the part of the decoder a literal-only
        # stream never exercised -- including one longer than 64 KiB, the
        # farthest an LZ4 match can reach back.
        repetitive = directory / "repetitive.bin"
        repetitive.write_bytes(b"ours boot image, compressed with lz4. " * 4096)
        varied = directory / "varied.bin"
        varied.write_bytes(bytes((i * 131 + i // 7) & 0xFF for i in range(70000)))
        files = directory / "compress.fini"
        files.write_text(f"repetitive={repetitive}\nvaried={varied}\n")

        def pack_files(compressed: bool) -> pathlib.Path:
            rsp_path = directory / f"compress-{compressed}.rsp"
            rsp_path.write_text(
                "--reset\n"
                "--files-type=kernel-storage\n"
                "--prefix=ours\n"
                + ("--compressed=lz4\n" if compressed else "")
                + f"--files\n{files}\n"
            )
            out_path = directory / f"compress-{compressed}.obi"
            run([str(args.obit), "pack", f"@{rsp_path}", "--output", str(out_path)])
            run([str(args.obit), "validate", str(out_path)])
            return out_path

        plain = pack_files(False)
        squeezed = pack_files(True)
        plain_kernel = directory / "plain-kernel.bin"
        squeezed_kernel = directory / "squeezed-kernel.bin"
        run([str(args.obit), "extract", str(plain), "kernel", str(plain_kernel)])
        run([str(args.obit), "extract", str(squeezed), "kernel", str(squeezed_kernel)])
        assert squeezed_kernel.read_bytes() == plain_kernel.read_bytes(), (
            "the lz4-compressed item did not extract to the bytes of the uncompressed one"
        )
        assert squeezed.stat().st_size < plain.stat().st_size // 2, (
            f"lz4 did not compress: {squeezed.stat().st_size} bytes against "
            f"{plain.stat().st_size} uncompressed"
        )

    args.stamp.write_text("ok\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
