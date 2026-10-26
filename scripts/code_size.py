#!/usr/bin/env python3
"""The bytes of code an operation runs through, for a performance report.

What one function's size says is not what an operation costs in code: the
compiler inlines some callees into their callers and leaves others out of
line, so the same work can sit in one large function or in five small ones.
This script sizes an operation by its closure instead: the entry function and
every function it can reach through a direct call, a jump to another function
(a tail call) or a function's address it takes, each counted once.

The closure stays inside the code being measured: a callee whose demangled
name matches none of the --within patterns is not followed or counted (a lock,
the allocator, the TLB shootdown), only reported as an external callee. An
indirect call cannot be followed and is counted, so that a closure that grew
only because it lost sight of a callee is not mistaken for a smaller one.

It is code that may run, error paths included, not code that does run on one
call: that is what lldb's instruction count is for (docs/perf/TEMPLATE.md).

Usage:

    python3 scripts/code_size.py ELF --within REGEX [--within REGEX...] \\
        --case NAME=ENTRY_REGEX [--case NAME=ENTRY_REGEX...] [--list]

    ENTRY_REGEX is searched in the demangled names of the ELF's functions and
    must match exactly one. Prints samples in the format scripts/perf_stats.py
    reads, one row per case and metric:

        code bytes   the closure's size in bytes
        functions    how many functions it holds
        indirect     indirect calls and jumps inside it
        external     distinct callees outside --within

    --list prints each closure's functions, largest first, to stderr.
"""

# macOS still ships Python 3.9 as python3; this keeps the annotations lazy.
from __future__ import annotations

import argparse
import bisect
import re
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Callable, Optional

sys.path.insert(0, str(Path(__file__).parent))

import fetch_llvm_coverage as tools  # noqa: E402

# A direct target as llvm-objdump prints it, in an operand (`callq 0x24150
# <sym>`, `bl 0x1000 <sym>`) or in the comment of a RIP-relative address
# (`leaq -0x64(%rip), %rdx  # 0x55540 <sym>`).
TARGET = re.compile(r"\b0x([0-9a-f]+) <")
# A call or jump through a register or memory: x86's `callq *%rax`, arm64's
# `blr x8` and `br x8`.
INDIRECT = re.compile(r"\s(call|callq|jmp|jmpq)\s+\*|\s(blr|br)\s+x\d+")
TEXT_TYPES = set("tTwW")


@dataclass(frozen=True)
class Function:
    address: int
    size: int
    name: str  # demangled


def parse_symbols(nm_output: str) -> list[Function]:
    """Functions from `llvm-nm -S --defined-only --demangle`, sorted by
    address. Names that share an address (folded copies) keep the first."""
    by_address: dict[int, Function] = {}
    for line in nm_output.splitlines():
        fields = line.split(" ", 3)
        if len(fields) != 4 or fields[2] not in TEXT_TYPES:
            continue
        address, size = int(fields[0], 16), int(fields[1], 16)
        if size == 0 or address in by_address:
            continue
        by_address[address] = Function(address, size, fields[3])
    return sorted(by_address.values(), key=lambda f: f.address)


class Symbols:
    def __init__(self, functions: list[Function]):
        self.functions = functions
        self.starts = [f.address for f in functions]

    def containing(self, address: int) -> Optional[Function]:
        at = bisect.bisect_right(self.starts, address) - 1
        if at < 0:
            return None
        function = self.functions[at]
        if address < function.address + function.size:
            return function
        return None

    def matching(self, pattern: str) -> list[Function]:
        regex = re.compile(pattern)
        return [f for f in self.functions if regex.search(f.name)]


def references(disassembly: str) -> tuple[list[int], int]:
    """The direct target addresses in a function's disassembly, and how many
    indirect calls and jumps it makes."""
    targets: list[int] = []
    indirect = 0
    for line in disassembly.splitlines():
        if ":" not in line:
            continue
        targets.extend(int(m.group(1), 16) for m in TARGET.finditer(line.split(":", 1)[1]))
        if INDIRECT.search(line):
            indirect += 1
    return targets, indirect


@dataclass
class Closure:
    functions: list[Function]
    indirect: int
    external: set[str]

    @property
    def size(self) -> int:
        return sum(f.size for f in self.functions)


def closure(entry: Function, symbols: Symbols, within: list[re.Pattern],
            disassemble: Callable[[Function], str]) -> Closure:
    """Every function |entry| reaches inside |within|, breadth first."""
    seen = {entry.address: entry}
    order = [entry]
    indirect = 0
    external: set[str] = set()
    at = 0
    while at < len(order):
        function = order[at]
        at += 1
        targets, calls = references(disassemble(function))
        indirect += calls
        for target in targets:
            callee = symbols.containing(target)
            if callee is None or callee.address == function.address:
                continue  # data, or a branch inside the function
            if callee.address in seen:
                continue
            if not any(p.search(callee.name) for p in within):
                external.add(callee.name)
                continue
            seen[callee.address] = callee
            order.append(callee)
    return Closure(order, indirect, external)


def run(tool: Path, *args: str) -> str:
    result = subprocess.run([str(tool), *args], check=True, capture_output=True, text=True)
    return result.stdout


def short(name: str) -> str:
    """A name without its parameter list, for --list."""
    depth = 0
    out = []
    for ch in name:
        if ch == "(":
            depth += 1
        if depth == 0:
            out.append(ch)
        if ch == ")":
            depth -= 1
    return "".join(out)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("elf", type=Path)
    parser.add_argument("--within", action="append", required=True,
                        help="a regex over demangled names: the code being measured")
    parser.add_argument("--case", action="append", required=True,
                        help="NAME=ENTRY_REGEX, one operation and its entry function")
    parser.add_argument("--list", action="store_true", help="print each closure to stderr")
    args = parser.parse_args()

    compiler = tools.find_compiler(None)
    nm = tools.locate_tool(compiler, "llvm-nm")
    objdump = tools.locate_tool(compiler, "llvm-objdump")
    if nm is None or objdump is None:
        sys.exit(f"llvm-nm or llvm-objdump is missing next to {compiler.clang}")

    symbols = Symbols(parse_symbols(run(nm, "-S", "--defined-only", "--demangle", str(args.elf))))
    within = [re.compile(p) for p in args.within]

    def disassemble(function: Function) -> str:
        return run(objdump, "-d", "--no-show-raw-insn", f"--start-address={function.address:#x}",
                   f"--stop-address={function.address + function.size:#x}", str(args.elf))

    print(f"# elf: {args.elf}")
    print(f"# within: {' | '.join(args.within)}")
    print("case,metric,unit,value")
    for case in args.case:
        name, _, pattern = case.partition("=")
        entries = symbols.matching(pattern)
        if len(entries) != 1:
            names = "\n  ".join(f.name for f in entries[:10])
            sys.exit(f"--case {name}: {pattern!r} matches {len(entries)} functions, not one\n  {names}")
        found = closure(entries[0], symbols, within, disassemble)
        print(f"{name},code bytes,bytes,{found.size}")
        print(f"{name},functions,functions,{len(found.functions)}")
        print(f"{name},indirect,calls,{found.indirect}")
        print(f"{name},external,callees,{len(found.external)}")
        if args.list:
            print(f"== {name}: {found.size} bytes", file=sys.stderr)
            for function in sorted(found.functions, key=lambda f: -f.size):
                print(f"  {function.size:6d}  {short(function.name)}", file=sys.stderr)
            for callee in sorted(found.external):
                print(f"  external  {short(callee)}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
