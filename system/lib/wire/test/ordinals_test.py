#!/usr/bin/env python3
"""Recompute every method ordinal the hand-written codecs declare.

    python3 system/lib/wire/test/ordinals_test.py

A codec declares each method as a wire::Method, its ordinal written beside the
selector it is hashed from (system/lib/wire/include/wire/wire.hpp):

    inline constexpr auto READ = wire::Method{
        .selector = "ours.io/File.Read",
        .ordinal = 0x2348'9a6f'12eb'b0f0,
        ...

The ordinal is the first 8 bytes of SHA-256(selector), read little-endian,
with the top bit cleared (component-framework design D20). The tree has no
SHA-256 in C++, so this test reads every such pair out of the codec headers
and recomputes it with hashlib. An ordinal typed wrong would make the codec
speak a method nobody else knows, and nothing on the target would say so:
the peer would close the connection as for any unknown ordinal.

Python 3.9 and the standard library only, as every tool here.
"""

from __future__ import annotations

import hashlib
import re
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[4]

# Where codec headers live: the public headers of every library under
# system/lib, and of the codecs beside the examples whose protocols they are
# (examples/echo/protocol).
HEADER_GLOBS = ("system/lib/*/include/**/*.hpp", "examples/**/include/**/*.hpp")

DECLARATION = re.compile(r"\bwire::Method\s*\{")
PAIR = re.compile(
    r'\bwire::Method\s*\{\s*\.selector\s*=\s*"(?P<selector>[^"]+)"\s*,'
    r"\s*\.ordinal\s*=\s*(?P<ordinal>0[xX][0-9A-Fa-f']+)"
)


def method_ordinal(selector: str) -> int:
    """The ordinal of the method |selector| names."""
    digest = hashlib.sha256(selector.encode("utf-8")).digest()
    return int.from_bytes(digest[:8], "little") & 0x7FFF_FFFF_FFFF_FFFF


def code_of(text: str) -> str:
    """|text| without its whole-line comments, so that a declaration shown in
    a comment is neither counted nor checked."""
    return "\n".join(line for line in text.splitlines() if not line.lstrip().startswith("//"))


def declarations(root: Path = ROOT) -> list[tuple[Path, str, int]]:
    """Every (header, selector, ordinal) the codec headers declare. Fails
    when a header declares a wire::Method this cannot read, so that a
    declaration written another way is not silently skipped."""
    found = []
    headers = {header for pattern in HEADER_GLOBS for header in root.glob(pattern)}
    for header in sorted(headers):
        code = code_of(header.read_text(encoding="utf-8"))
        pairs = list(PAIR.finditer(code))
        declared = len(DECLARATION.findall(code))
        if declared != len(pairs):
            raise AssertionError(
                f"{header.relative_to(root)}: {declared} wire::Method declarations, but "
                f"{len(pairs)} with .selector first and .ordinal right after it"
            )
        for match in pairs:
            ordinal = int(match["ordinal"].replace("'", ""), 16)
            found.append((header.relative_to(root), match["selector"], ordinal))
    return found


class OrdinalTest(unittest.TestCase):
    def test_hasher_known_answer(self) -> None:
        # A known answer, computed outside this file by another
        # implementation of the same rule: checks the hasher itself, not only
        # its agreement with the values typed into the headers.
        self.assertEqual(method_ordinal("test.anonymous/SomeProtocol.SomeMethod"), 2515914517457749720)

    def test_codec_headers_declare_methods(self) -> None:
        selectors = {selector for _, selector, _ in declarations()}
        # The protocol subset (component-framework design, "The protocol
        # subset"), and the echo example's protocol: if the scan finds none
        # of these, it is looking in the wrong place, and every other check
        # here would pass vacuously.
        for selector in (
            "ours.io/Directory.Open",
            "ours.io/Node.Close",
            "ours.io/Cloneable.Clone",
            "ours.io/File.Read",
            "ours.io/File.GetBackingMemory",
            "ours.io/Directory.ReadDirents",
            "ours.process/Launcher.AddArgs",
            "ours.process/Launcher.AddHandles",
            "ours.process/Launcher.AddNames",
            "ours.process/Launcher.Launch",
            "ours.resource/Debug.Get",
            "ours.resource/Irq.Get",
            "ours.resource/Ioport.Get",
            "ours.resource/Mmio.Get",
            "ours.boot/Acpi.Get",
            "ours.acpi/Route.IsaIrq",
            "ours.acpi/Route.EcamBase",
            "ours.resource/Iommu.Get",
            "ours.examples.echo/Echo.EchoString",
            "ours.examples.echo/Echo.Stop",
        ):
            self.assertIn(selector, selectors)

    def test_every_ordinal_is_its_selectors_hash(self) -> None:
        for header, selector, ordinal in declarations():
            with self.subTest(header=str(header), selector=selector):
                self.assertEqual(
                    ordinal,
                    method_ordinal(selector),
                    f"{header}: {selector} is {method_ordinal(selector):#018x}, not {ordinal:#018x}",
                )

    def test_ordinals_are_unique(self) -> None:
        # Two selectors with one ordinal could not both be served on one
        # channel: a server could not tell which method a message calls.
        seen: dict[int, str] = {}
        for header, selector, ordinal in declarations():
            with self.subTest(header=str(header), selector=selector):
                self.assertNotIn(ordinal, seen, f"{selector} and {seen.get(ordinal)}")
                seen[ordinal] = selector

    def test_unreadable_declaration_is_refused(self) -> None:
        # A declaration with .ordinal before .selector cannot be paired, and
        # must fail rather than be skipped.
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            header = root / "system/lib/codec/include/codec/codec.hpp"
            header.parent.mkdir(parents=True)
            header.write_text(
                "inline constexpr auto X = wire::Method{\n"
                "    .ordinal = 0x1,\n"
                '    .selector = "a/B.C",\n'
                "};\n",
                encoding="utf-8",
            )
            with self.assertRaises(AssertionError):
                declarations(root)


if __name__ == "__main__":
    unittest.main()
