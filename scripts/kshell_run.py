#!/usr/bin/env python3
"""Boot one dev image under QEMU, type kernel-shell commands into it, and
report what each one printed.

Run from the repo root, after a build:

    python3 scripts/kshell_run.py --out out/mine vmosnapshot.test
    python3 scripts/kshell_run.py --out out/mine --image rel vmaseal.test usercopy.test

The kernel shell (//kernel/lib/kshell) reads COM1 and runs registered console
commands, the *.test commands of //kernel/commands among them. Nothing runs
them at boot, which is what this is for. The commands are typed once the boot
is over -- the user shell's closing "--- end ---" line -- one at a time. A
*.test command is done at its verdict, the "<name>: ok" or "<name>: FAILED"
line every test there ends with. Any other command is done when the kernel
shell prints its "kshell> " prompt again. The prompt ends no line, so the
serial port is read in raw chunks rather than by line, or a quiet image would
never show it. It is also written a byte at a time, outside the log's lock, so
on the dbg image, whose other CPUs log thousands of lines a second, its bytes
land between theirs and it is seldom whole: there, only a test's verdict is a
sure end, and a command that prints none is best run on rel.

A command fails if it prints a FAILED line or an error-level line, or if the
prompt is not back within --command-timeout. A *.test command also fails
unless it says "<name>: ok", as every test in //kernel/commands does when it
passes: the kernel shell says nothing about a name it does not know, so a
misspelt test would otherwise pass. Debug and trace lines are left out
of what is printed unless --all is given; the whole serial log stays on disk.
Environment variables such as SMP=1 pass straight through to
scripts/qemu_boot.py, as they do for the ours-verify skill's boot_check.py.

A `crash` command (//kernel/commands/crash.cpp) panics the kernel on purpose,
so it has to be the last command, and it is judged by the panic report it
leads to rather than by a prompt that never comes back. It is done once the
panic banner and the line after it are in. It passes if that line is the
panic message, an error-level line, and if the CPU that panicked printed
nothing between the command and the banner: the reason comes first, before
any context (other CPUs saying they have stopped may come before it). The
console is then read for PANIC_SETTLE seconds more, and the banner must have
come out exactly once: one panic report, however many CPUs panicked. In it,
the ring's replay must stand between its two marker lines, and a replay the
first marker calls a tail of N lines must be no longer than that.

With --reboot, QEMU runs with REBOOT=1, so a guest reset reboots it instead
of ending it. A crash command on an image with kernel.panic.reboot then
resets the machine after its report, and it passes only if the next boot
logs the crash record the panic left, with the panic's message as its first
line ("crashlog: | <message>"), within --boot-timeout.
"""

# macOS still ships Python 3.9 as python3; this keeps the annotations lazy.
from __future__ import annotations

import argparse
import codecs
import os
import queue
import re
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
IMAGES = {"dbg": "kernel.dev.dbg_1.obi", "rel": "kernel.dev.obi"}
END_MARKER = "--- end ---"  # the last line examples/shell/shell.cpp prints
PROMPT = "kshell> "  # what kernel/lib/kshell writes before it reads a line
ANSI = re.compile(r"\x1b\[[0-9;=?]*[A-Za-z]")
# The log-level tags, as boot_check.py reads them.
LEVEL = r"\[(?:\d+ )?(?:cpu\d+ )?(?:{})\]"
ERROR_LINE = re.compile(LEVEL.format("error"))
# The CPU an error line was printed on, as its prefix gives it.
ERROR_CPU = re.compile(r"\[(?:\d+ )?(cpu\d+) error\]")
# The line a kernel panic report starts with (kernel/top/panic.cpp).
PANIC_BANNER = "OURS KERNEL PANIC"
# The marker lines around a panic's replay of the record ring
# (kernel/top/panic.cpp). The first gives the tail's length, when it is one.
RECENT_START = re.compile(r"--- recent log: (?:the last (\d+) lines|every line kept)[^\n]*\n")
RECENT_END = "--- end of recent log ---"
FAILED_LINE = re.compile(r"\bFAILED\b")
NOISE = re.compile(LEVEL.format("debug|trace"))
SETTLE = 1.0  # seconds between the end marker and the first command
# Seconds a crash command's console is read after its first lines, for a
# second panic report. A CPU that panics while another's report is under way
# would, without the guard in vpanic(), first wait out suspend_others()'s
# bound, which is several seconds under QEMU.
PANIC_SETTLE = 10.0
AFTER_VERDICT = 0.5  # seconds for a test to return from its verdict to the prompt
TAIL = 512  # characters searched again, for a match that straddles two reads
BURST = 64  # reads taken in per pump, so that a deadline is looked at in between


class Console:
    """QEMU's COM1: what it says, as text, and a way to type into it.

    The dbg image logs thousands of lines a second and never stops, so nothing
    is kept by default: only what arrives between keep() and the needle
    wait_for() finds, which is one command's output. Everything still goes to
    the log file."""

    def __init__(self, out: Path, tag: str, logdir: Path) -> None:
        env = dict(os.environ, OUT=str(out), IMAGE=str(out / IMAGES[tag]),
                   WORK=str(logdir / f"esp-{tag}"))
        # qemu_boot.py execs QEMU, so this pid becomes QEMU itself.
        self.proc = subprocess.Popen([sys.executable, "scripts/qemu_boot.py"], cwd=ROOT, env=env,
                                     stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     stderr=subprocess.STDOUT)
        self.log_path = logdir / f"{tag}.log"
        self.log = open(self.log_path, "wb")
        self.decoder = codecs.getincrementaldecoder("utf-8")("replace")
        self.pending = ""  # the start of an escape sequence not yet whole
        self.parts: list[str] = []
        self.keeping = False
        self.after = ""  # what came in behind wait_for()'s last match
        self.gone = False
        self.chunks: queue.Queue[bytes | None] = queue.Queue()
        threading.Thread(target=self._read, daemon=True).start()

    def _read(self) -> None:
        assert self.proc.stdout is not None
        fd = self.proc.stdout.fileno()
        while True:
            chunk = os.read(fd, 65536)
            if not chunk:
                self.chunks.put(None)
                return
            self.chunks.put(chunk)

    def _pump(self, wait: float) -> str:
        """What arrives within |wait| seconds, escapes stripped, taken a burst
        at a time: a port that never goes quiet must not hold a deadline off.
        An escape split across two reads is held back until it is whole,
        unless it never becomes one. Sets |gone| once QEMU has gone."""
        try:
            chunks = [self.chunks.get(timeout=wait)]
        except queue.Empty:
            return ""
        while len(chunks) < BURST:
            try:
                chunks.append(self.chunks.get_nowait())
            except queue.Empty:
                break
        decoded = []
        for chunk in chunks:
            if chunk is None:
                self.gone = True
                break
            self.log.write(chunk)
            decoded.append(self.decoder.decode(chunk))
        text = self.pending + "".join(decoded)
        cut = text.rfind("\x1b")
        if cut >= 0 and not ANSI.match(text, cut) and len(text) - cut < 64:
            text, self.pending = text[:cut], text[cut:]
        else:
            self.pending = ""
        return ANSI.sub("", text)

    def keep(self) -> None:
        """Keeps what arrives from now on, until wait_for() finds its needle."""
        self.parts = []
        self.keeping = True

    def kept(self) -> str:
        return "".join(self.parts)

    def take(self, seconds: float) -> str:
        """Takes in whatever arrives for |seconds|, and returns it, starting
        with what came in behind wait_for()'s last match: a panic report
        often arrives in one read, and the match is only its first lines."""
        parts = [self.after]
        self.after = ""
        until = time.monotonic() + seconds
        while time.monotonic() < until and not self.gone:
            parts.append(self._pump(0.2))
        return "".join(parts)

    def drain(self, seconds: float) -> None:
        """Takes in, and drops, whatever arrives for |seconds|."""
        until = time.monotonic() + seconds
        while time.monotonic() < until and not self.gone:
            self._pump(0.2)

    def wait_for(self, pattern: re.Pattern[str], deadline: float, through: bool = False) -> bool:
        """Takes in what arrives until |pattern| matches it, then stops
        keeping: what was kept is what came before the match, or what came up
        to its end with |through|. False if |deadline| passes or QEMU goes
        first."""
        tail = ""  # the end of what was searched, kept already
        while True:
            fresh = self._pump(0.2)
            window = tail + fresh
            match = pattern.search(window)
            if match:
                self.after = window[match.end():]
                if self.keeping:
                    stop = (match.end() if through else match.start()) - len(tail)
                    if stop >= 0:
                        self.parts.append(fresh[:stop])
                    else:
                        whole = self.kept()
                        self.parts = [whole[:len(whole) + stop]]
                    self.keeping = False
                return True
            if self.keeping:
                self.parts.append(fresh)
            tail = window[-TAIL:]
            if self.gone or time.monotonic() >= deadline:
                return False

    def type(self, line: str) -> None:
        assert self.proc.stdin is not None
        self.proc.stdin.write((line + "\n").encode())
        self.proc.stdin.flush()

    def close(self) -> None:
        self.proc.kill()
        self.proc.wait()
        self.log.close()


def show(command: str, output: str, show_all: bool) -> list[str]:
    """Prints what |command| printed, debug and trace lines left out unless
    |show_all|, and returns all of it as lines."""
    lines = [l.rstrip("\r") for l in output.splitlines()]
    # The shell echoes what was typed before it runs it.
    if lines and lines[0].strip() == command:
        lines = lines[1:]
    print(f"== {command}")
    for line in lines:
        if line.strip() and (show_all or not NOISE.search(line)):
            print(f"   {line[:220]}")
    return lines


def judge(command: str, lines: list[str]) -> bool:
    """Says whether |command|, which printed |lines|, passed."""
    failed = [l for l in lines if FAILED_LINE.search(l) or ERROR_LINE.search(l)]
    if failed:
        print(f"{command}: FAIL, {len(failed)} FAILED or error line(s)")
        return False
    # A test command ends with "<name>: ok -- N checks" when it passes. The
    # kernel shell says nothing about a name it does not know, so without
    # this a misspelt test would pass by printing nothing.
    name = command.split()[0]
    if name.endswith(".test") and not any(f"{name}: ok" in l for l in lines):
        print(f'{command}: FAIL, no "{name}: ok" line -- is there such a command?')
        return False
    print(f"{command}: ok")
    return True


def judge_crash(command: str, lines: list[str], after: str) -> bool:
    """Says whether the panic report |command| led to opens as it should:
    the banner, then the message, and nothing from the panicking CPU before
    them; and whether it is the only report. |lines| ends with the line after
    the banner, and |after| is what came in the PANIC_SETTLE seconds after
    that."""
    banner = next(i for i, l in enumerate(lines) if PANIC_BANNER in l)
    message = lines[banner + 1] if banner + 1 < len(lines) else ""
    found = ERROR_CPU.search(message)
    if not found:
        print(f"{command}: FAIL, the line after the panic banner is not an error-level message")
        return False
    # Lines the panicking CPU logged above debug level, by their intact tag.
    # Not "every line naming the CPU that is not noise": on dbg the shell's
    # echo lands a byte at a time inside other lines' prefixes, and a debug
    # line whose tag it broke no longer reads as noise.
    own = re.compile(r"\[(?:\d+ )?" + re.escape(found.group(1)) + r" (?:info|warn|error)\]")
    early = [l for l in lines[:banner] if own.search(l)]
    if early:
        print(f"{command}: FAIL, {found.group(1)} printed {len(early)} line(s) before its panic banner")
        return False
    banners = sum(PANIC_BANNER in l for l in lines) + after.count(PANIC_BANNER)
    if banners != 1:
        print(f"{command}: FAIL, {banners} panic banners within {PANIC_SETTLE:.0f}s; want one report")
        return False
    start = RECENT_START.search(after)
    end = after.find(RECENT_END, start.end()) if start else -1
    if end < 0:
        print(f"{command}: FAIL, no ring replay between its markers within {PANIC_SETTLE:.0f}s")
        return False
    replayed = [l for l in after[start.end():end].splitlines() if l.strip()]
    if start.group(1) and len(replayed) > int(start.group(1)):
        print(f"{command}: FAIL, a replay of {len(replayed)} lines under a {start.group(1)}-line marker")
        return False
    print(f"{command}: ok")
    return True


def judge_recovery(console: Console, command: str, lines: list[str], after: str,
                   timeout: float) -> bool:
    """With --reboot: says whether the boot after the panic logged the crash
    record the panic left. Its first line is the panic message, which the line
    after the banner in |lines| carries behind its prefix. The next boot may
    already have said so in |after|; otherwise it is waited for."""
    banner = next(i for i, l in enumerate(lines) if PANIC_BANNER in l)
    message = re.sub(r"^\[[^\]]*\] ", "", lines[banner + 1]).strip()
    recovered = re.compile(r"crashlog: \| " + re.escape(message))
    if not recovered.search(after) and not console.wait_for(recovered, time.monotonic() + timeout):
        print(f"{command}: FAIL, the next boot did not report the crash record within {timeout:.0f}s")
        return False
    print(f"{command}: the next boot reported the crash record")
    return True


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("commands", nargs="+", metavar="command", help="a kernel-shell command line")
    parser.add_argument("--out", default="out/bringup.x86", help="GN build directory (default: out/bringup.x86)")
    parser.add_argument("--image", choices=sorted(IMAGES), default="dbg", help="the image to boot (default: dbg)")
    parser.add_argument("--boot-timeout", type=float, default=60, help="seconds for the boot to end (default: 60)")
    parser.add_argument("--command-timeout", type=float, default=120, help="seconds per command (default: 120)")
    parser.add_argument("--all", action="store_true", help="print debug and trace lines too")
    parser.add_argument("--reboot", action="store_true",
                        help="let a guest reset reboot QEMU, and check the crash record a crash leaves")
    args = parser.parse_args(argv)

    crashes = [i for i, c in enumerate(args.commands) if c.split()[0] == "crash"]
    if crashes and crashes[0] != len(args.commands) - 1:
        print("a crash command ends the machine, so it can only be the last command")
        return 1

    out = (ROOT / args.out).resolve()
    image = out / IMAGES[args.image]
    if not image.is_file():
        print(f"missing image {image}; build it first (boot_check.py, or ninja -C {args.out})")
        return 1
    logdir = Path(tempfile.gettempdir()) / f"ours-kshell-{out.name}"
    logdir.mkdir(parents=True, exist_ok=True)

    if args.reboot:
        os.environ["REBOOT"] = "1"
    console = Console(out, args.image, logdir)
    results: list[bool] = []
    try:
        if not console.wait_for(re.compile(re.escape(END_MARKER)),
                                time.monotonic() + args.boot_timeout):
            print(f'boot: no "{END_MARKER}" within {args.boot_timeout:.0f}s (or QEMU exited)')
            return 1
        console.drain(SETTLE)
        for command in args.commands:
            name = command.split()[0]
            test = name.endswith(".test")
            crash = name == "crash"
            if crash:
                done = re.compile(re.escape(PANIC_BANNER) + r"[^\n]*\n[^\n]*\n")
                awaited = f'"{PANIC_BANNER}" and the line after it'
            elif test:
                done = re.compile(re.escape(name) + r": (?:ok|FAILED)\b[^\n]*\n")
                awaited = f'"{name}: ok" or FAILED line'
            else:
                done = re.compile(re.escape(PROMPT))
                awaited = "prompt (on dbg, other CPUs' log lines can break it up; try --image rel)"
            console.keep()
            console.type(command)
            ended = console.wait_for(done, time.monotonic() + args.command_timeout,
                                     through=test or crash)
            lines = show(command, console.kept(), args.all)
            if not ended:
                print(f"{command}: FAIL, no {awaited} within {args.command_timeout:.0f}s")
                results.append(False)
                break
            if crash:
                after = console.take(PANIC_SETTLE)
                passed = judge_crash(command, lines, after)
                if passed and args.reboot:
                    passed = judge_recovery(console, command, lines, after, args.boot_timeout)
                results.append(passed)
                break
            results.append(judge(command, lines))
            if test:
                console.drain(AFTER_VERDICT)
    finally:
        console.close()
        print(f"log: {console.log_path}")
    return 0 if len(results) == len(args.commands) and all(results) else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
