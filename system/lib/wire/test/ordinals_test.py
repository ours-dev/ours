#!/usr/bin/env python3
"""Recompute every method ordinal the hand-written codecs declare.

    python3 system/lib/wire/test/ordinals_test.py

A codec declares each method as a wire::Method, its ordinal written beside the
selector it is hashed from (system/lib/wire/include/wire/wire.hpp):

    inline constexpr auto READ = wire::Method{
        .selector = "fuchsia.io/File.Read",
        .ordinal = 0x057e'419a'298c'8ede,
        ...

The ordinal is fidlc's, Sha256MethodHasher in tools/fidl/fidlc/src/compiler.cc:
the first 8 bytes of SHA-256(selector), read little-endian, with the top bit
cleared. The tree has no SHA-256 in C++, so this test reads every such pair
out of the codec headers and recomputes it with hashlib. An ordinal typed
wrong would make the codec speak a method nobody else knows, and nothing on
the target would say so: the peer would close the connection as for any
unknown ordinal.

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
# system/lib.
HEADER_GLOB = "system/lib/*/include/**/*.hpp"

DECLARATION = re.compile(r"\bwire::Method\s*\{")
PAIR = re.compile(
    r'\bwire::Method\s*\{\s*\.selector\s*=\s*"(?P<selector>[^"]+)"\s*,'
    r"\s*\.ordinal\s*=\s*(?P<ordinal>0[xX][0-9A-Fa-f']+)"
)


def fidl_ordinal(selector: str) -> int:
    """fidlc's Sha256MethodHasher."""
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
    for header in sorted(root.glob(HEADER_GLOB)):
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
    def test_hasher_matches_fidlc(self) -> None:
        # From fidlc's own golden output, tools/fidl/fidlc/goldens/
        # anonymous.json.golden at Fuchsia 51f05104c13: the method SomeMethod
        # of test.anonymous/SomeProtocol. Checks the hasher against fidlc
        # itself, not only against the values typed into the headers.
        self.assertEqual(fidl_ordinal("test.anonymous/SomeProtocol.SomeMethod"), 2515914517457749720)

    def test_codec_headers_declare_methods(self) -> None:
        selectors = {selector for _, selector, _ in declarations()}
        # The protocol subset (component-framework design, "The protocol
        # subset"): if the scan finds none of these, it is looking in the
        # wrong place, and every other check here would pass vacuously.
        for selector in (
            "fuchsia.io/Directory.Open3",
            "fuchsia.io/Node.Close",
            "fuchsia.unknown/Cloneable.Clone",
            "fuchsia.io/File.Read",
            "fuchsia.io/File.GetBackingMemory",
            "fuchsia.io1/Directory.ReadDirents",
            "fuchsia.process/Launcher.AddArgs",
            "fuchsia.process/Launcher.AddHandles",
            "fuchsia.process/Launcher.AddNames",
            "fuchsia.process/Launcher.Launch",
            "fuchsia.kernel/DebugResource.Get",
        ):
            self.assertIn(selector, selectors)

    def test_every_ordinal_is_its_selectors_hash(self) -> None:
        for header, selector, ordinal in declarations():
            with self.subTest(header=str(header), selector=selector):
                self.assertEqual(
                    ordinal,
                    fidl_ordinal(selector),
                    f"{header}: {selector} is {fidl_ordinal(selector):#018x}, not {ordinal:#018x}",
                )

    def test_ordinals_are_unique(self) -> None:
        # Two selectors with one ordinal could not both be served on one
        # channel; fidlc refuses a protocol where that happens.
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
