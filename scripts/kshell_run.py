#!/usr/bin/env python3
"""Boot one dev image under QEMU, type kernel-shell commands into it, and
report what each one printed.

Run from the repo root, after a build:

    python3 scripts/kshell_run.py --out out/mine vmosnapshot.test vmaseal.test usercopy.test

Only the dbg image (the default) has a kernel shell on COM1. The rel image's
belongs to the user shell (kernel.shell=false since the interactive user
shell), so --image rel reaches no kernel command.

The kernel shell (//kernel/lib/kshell) reads COM1 and runs registered console
commands, the *.test commands of //kernel/commands among them. Nothing runs
them at boot, which is what this is for. The commands are typed once the boot
is over -- the user shell's closing "--- end ---" line -- one at a time, or
--delay seconds after it, for a command such as `lockup status` whose answer
means more the longer the machine has run. A
*.test command is done at its verdict, the "<name>: ok" or "<name>: FAILED"
line every test there ends with. Any other command is done when the kernel
shell prints its "kshell> " prompt again. The prompt ends no line, so the
serial port is read in raw chunks rather than by line, or a quiet image would
never show it. It is also written a byte at a time, outside the log's lock, so
on the dbg image, whose other CPUs may log meanwhile, its bytes can land
between theirs: there a test's verdict is the surer end, and a command that
prints none may need a longer --command-timeout.

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

The lockup commands (//kernel/commands/lockup.cpp) that end in the lockup
detector's report end the machine too, as crash does: `lockup nmi`, and
`lockup spin` without a length, `lockup spin <cpu>` or `lockup spin all`.
Each is judged by its report and the panic after it, which must say what the
command asked for: one report, one block per CPU and none that did not
answer, then the panic's message and one banner, as for crash. `lockup nmi`
is a dump asked for by an NMI. `lockup spin <cpu>` is found by the
detector's own check, with that CPU's block last and its interrupts off.
`lockup spin all` leaves no CPU to check the others, so it is reported only
through the `nmi` this script sends after a silence (below), and every
block has interrupts off. `lockup spin <cpu> <ms>` returns, and is done at
its "lockup: cpu<N> has interrupts on again" line; `lockup status` returns
too, and is done at its last line, the closest any CPU has come to the
threshold.

Once the kernel has printed its first line and until the boot's end marker,
a console silent for --silence seconds (15 by default) means every CPU has
stopped: a booting kernel logs as it goes. After the end marker a quiet
console is an idle machine, so silence counts again only while
`lockup spin all` runs, the one command that stops every CPU. The script then
sends `nmi`
to a QEMU monitor of its own, as boot_check.py types its keys, and keeps
reading: the NMI reaches the boot CPU, which asks every CPU for its state and
prints the lockup report, and the panic ends the machine. The wait in hand
then fails, with the report shown and kept in the log, which the verdict
says. One `nmi` per run, and none once a panic has begun. A wait whose
timeout comes while the console is already quiet waits for the silence to
run its course rather than end without a report. What the monitor answered is in
<image>.monitor.log beside the log. A hunt for an intermittent hang is a
capped loop of this script over the commands the hang was seen in, stopping
at the first run that fails: a run that hangs reports itself.

With --reboot, QEMU runs with REBOOT=1, so a guest reset reboots it instead
of ending it. A crash command, or a lockup command that ends the machine, on
an image with kernel.panic.reboot then resets the machine after its report,
and it passes only if the next boot logs the crash record the panic left,
with the panic's message as its first line ("crashlog: | <message>"), and
userboot reports the VMO it was handed ("userboot: the previous boot's crash
record: <message>"), within --boot-timeout.
"""

# macOS still ships Python 3.9 as python3; this keeps the annotations lazy.
from __future__ import annotations

import argparse
import codecs
import os
import queue
import re
import socket
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
IMAGES = {"dbg": "kernel.dev.dbg_1.obi", "rel": "kernel.dev.obi"}
END_MARKER = "--- end ---"  # the last line system/app/shell/shell.cpp prints
PROMPT = "kshell> "  # what kernel/lib/kshell writes before it reads a line
ANSI = re.compile(r"\x1b\[[0-9;=?]*[A-Za-z]")
# The log-level tags, as boot_check.py reads them.
LEVEL = r"\[(?:\d+ )?(?:cpu\d+ )?(?:{})\]"
ERROR_LINE = re.compile(LEVEL.format("error"))
# The CPU an error line was printed on, as its prefix gives it.
ERROR_CPU = re.compile(r"\[(?:\d+ )?(cpu\d+) error\]")
# Any line the kernel logs: the console is watched for silence only from the
# first, since before it there is no kernel to answer an NMI. The kernel's
# tag starts with its timestamp; phys's "[info]" has none, as boot_check.py's
# "kernel" milestone reads them.
KERNEL_LINE = re.compile(r"\[\d+ (?:cpu\d+ )?(?:info|debug|trace|warn|error)\]")
# The line a kernel panic report starts with (kernel/top/panic.cpp).
PANIC_BANNER = "OURS KERNEL PANIC"
# The marker lines around a panic's replay of the record ring
# (kernel/top/panic.cpp). The first gives the tail's length, when it is one.
RECENT_START = re.compile(r"--- recent log: (?:the last (\d+) lines|every line kept)[^\n]*\n")
RECENT_END = "--- end of recent log ---"
FAILED_LINE = re.compile(r"\bFAILED\b")
NOISE = re.compile(LEVEL.format("debug|trace"))
# The lockup detector's report (//kernel/lib/lockup_detector): every line of
# its own is an error-level "lockup: " line; a CPU's registers follow its
# first line without the prefix. The header says why the report was made,
# each block's first line whose state it is, and the last two lines are for a
# CPU that left no state.
REPORT_LINE = re.compile(LEVEL.format("error") + r" lockup: ")
REPORT_HEADER = re.compile(r"lockup: (?:cpu(\d+) had no heartbeat for \d+ s, found by cpu\d+"
                           r"|dump asked for by an NMI on cpu(\d+)); each CPU's state follows")
REPORT_BLOCK = re.compile(r"lockup: cpu(\d+)(?: \([^)]*\))? thread .*? "
                          r"(?:at \+|in user space at )0x[0-9a-f]+, interrupts (on|off)")
REPORT_MISSING = re.compile(r"lockup: cpu(\d+) (?:did not answer its NMI|was sent no NMI)")
SETTLE = 1.0  # seconds between the end marker and the first command
# Seconds a crash command's console is read after its first lines, for a
# second panic report. A CPU that panics while another's report is under way
# would, without the guard in vpanic(), first wait out suspend_others()'s
# bound, which is several seconds under QEMU.
PANIC_SETTLE = 10.0
AFTER_VERDICT = 0.5  # seconds for a test to return from its verdict to the prompt
TAIL = 512  # characters searched again, for a match that straddles two reads
BURST = 64  # reads taken in per pump, so that a deadline is looked at in between
# Seconds of silence, once the kernel has spoken, after which QEMU is sent
# `nmi`: the hang hunt's own measure (docs/repairs/261007/kernel-smp.md), and
# more than the detector's 10 s threshold, so that a hang the detector can see
# from inside is mostly reported by it first.
SILENCE = 15.0
# Seconds after the `nmi` for its report and the panic after it to come out:
# the reporter waits up to 2 s for the other CPUs, and the report and the
# panic's replay of the ring take a few seconds more on the serial port.
REPORT_GRACE = 15.0
# Seconds of quiet that, when a wait's deadline comes, hold the deadline off
# until the quiet is a silence of --silence seconds and the report has come:
# a hang just before a deadline would otherwise go unreported.
QUIET = 2.0


class Console:
    """QEMU's COM1: what it says, as text, and a way to type into it; and
    QEMU's monitor, which is sent `nmi` once COM1 has been silent too long.

    A dbg boot logs at debug level, a great deal, so nothing is kept by
    default: only what arrives between keep() and the needle wait_for()
    finds, which is one command's output, and what arrives after the `nmi`.
    Everything still goes to the log file."""

    def __init__(self, out: Path, tag: str, logdir: Path, silence: float) -> None:
        env = dict(os.environ, OUT=str(out), IMAGE=str(out / IMAGES[tag]),
                   WORK=str(logdir / f"esp-{tag}"))
        # The monitor is one end of a socket pair that QEMU inherits, as in
        # boot_check.py, and not the one behind Ctrl-A c on COM1: that one
        # echoes what is typed into it onto COM1, into the log's lines.
        self.monitor, far = socket.socketpair()
        # qemu_boot.py execs QEMU, so this pid becomes QEMU itself, and the
        # inherited descriptor stays open across the exec.
        self.proc = subprocess.Popen([sys.executable, "scripts/qemu_boot.py",
                                      "-chardev", f"socket,id=monitor,fd={far.fileno()}",
                                      "-monitor", "chardev:monitor"], cwd=ROOT, env=env,
                                     stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     stderr=subprocess.STDOUT, pass_fds=[far.fileno()])
        far.close()
        self.monitor_log_path = logdir / f"{tag}.monitor.log"
        self.silence = silence
        self.last_input = time.monotonic()  # when the last read came in
        self.kernel_seen = False  # the kernel has printed a line: silence counts
        self.panicked = False  # a panic banner has come: silence no longer counts
        self.nmi_at: float | None = None  # when `nmi` was sent, once
        self.nmi_quiet = 0.0  # how long the console had been silent then
        self.since_nmi: list[str] = []  # what came in after it
        self.nmi_tail = ""  # the end of it, for a marker that straddles two reads
        self.report_done = False  # the panic after the report has replayed the ring
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
            self.last_input = time.monotonic()
            self.chunks.put(chunk)

    def _pump(self, wait: float) -> str:
        """What arrives within |wait| seconds, escapes stripped, taken a burst
        at a time: a port that never goes quiet must not hold a deadline off.
        An escape split across two reads is held back until it is whole,
        unless it never becomes one. Sets |gone| once QEMU has gone. With
        nothing to take in, sees whether the silence has lasted long enough
        to send `nmi`."""
        try:
            chunks = [self.chunks.get(timeout=wait)]
        except queue.Empty:
            self._watch()
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
        text = ANSI.sub("", text)
        if not self.kernel_seen and KERNEL_LINE.search(text):
            self.kernel_seen = True
        if PANIC_BANNER in text:
            self.panicked = True
        if self.nmi_at is not None:
            self.since_nmi.append(text)
            window = self.nmi_tail + text
            self.report_done = self.report_done or RECENT_END in window
            self.nmi_tail = window[-TAIL:]
        return text

    def _watch(self) -> None:
        """Sends `nmi` to QEMU's monitor once the console has been silent for
        |silence| seconds, after the kernel's first line and before any
        panic, once per run.

        Silence counts only while it means no CPU is running: during the
        boot, which logs as it goes, and during `lockup spin all`. The NMI
        reaches the boot CPU through LINT1, whatever its interrupt flag,
        and the lockup detector takes an NMI nothing in the kernel sent as a
        request for its report."""
        if (self.silence <= 0 or self.nmi_at is not None or not self.kernel_seen
                or self.panicked or self.gone):
            return
        now = time.monotonic()
        quiet = now - self.last_input
        if quiet < self.silence:
            return
        self.nmi_at = now
        self.nmi_quiet = quiet
        print(f"silence: nothing on the console for {quiet:.0f}s; sent QEMU's monitor `nmi` "
              f"for a lockup report", flush=True)
        try:
            self.monitor.sendall(b"nmi\n")
        except OSError:
            # QEMU has gone already, which the next read says.
            pass

    def falling_silent(self) -> bool:
        """Whether the console has been quiet for QUIET seconds while
        silence counts, with no `nmi` sent yet: a silence that _watch() will
        turn into a report if it lasts, which a deadline waits for."""
        return (self.silence > 0 and self.nmi_at is None and self.kernel_seen
                and not self.panicked and not self.gone
                and time.monotonic() - self.last_input >= QUIET)

    def nmi_text(self) -> str:
        """What came in after the `nmi`, if one was sent."""
        return "".join(self.since_nmi)

    def report_over(self) -> bool:
        """Whether the report an `nmi` asked for is out, or never will be:
        the panic after it has replayed the ring, QEMU has gone, or
        REPORT_GRACE has passed. False if no `nmi` was sent."""
        if self.nmi_at is None:
            return False
        return self.gone or self.report_done or time.monotonic() >= self.nmi_at + REPORT_GRACE

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
        first, or once the report an `nmi` asked for is over: after an
        `nmi`, |deadline| waits for that report, and a deadline that comes
        while the console is falling silent waits for the `nmi`. A wait that
        begins once that report is already over keeps its own deadline."""
        tail = ""  # the end of what was searched, kept already
        # A report already over was waited for by an earlier wait, the one
        # it ended. Ending this one too would fail --reboot's wait for the
        # next boot's crash record after `lockup spin all` at its first
        # look, unless that boot had already logged it during PANIC_SETTLE.
        report_waited = self.report_over()
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
            if self.gone:
                return False
            if self.nmi_at is not None and not report_waited:
                if self.report_over():
                    return False
            elif time.monotonic() >= deadline and not self.falling_silent():
                return False

    def type(self, line: str) -> None:
        assert self.proc.stdin is not None
        self.proc.stdin.write((line + "\n").encode())
        self.proc.stdin.flush()

    def close(self) -> None:
        self.proc.kill()
        self.proc.wait()
        self.log.close()
        # QEMU is gone, so what its monitor answered ends here: an `nmi` it
        # refused says why in it.
        with self.monitor, open(self.monitor_log_path, "wb") as log:
            self.monitor.settimeout(1)
            try:
                while chunk := self.monitor.recv(4096):
                    log.write(chunk)
            except OSError:
                pass


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


def ends_machine(command: str) -> bool:
    """Whether |command| ends the machine, and so has to be the last: `crash`,
    which panics, and the lockup commands whose report ends in a panic --
    `lockup nmi`, and `lockup spin` without a length."""
    words = command.split()
    if words[0] == "crash":
        return True
    return words[0] == "lockup" and (words[1:] == ["nmi"]
                                     or (words[1:2] == ["spin"] and len(words) == 3))


def stops_every_cpu(command: str) -> bool:
    """Whether |command| leaves no CPU running, so that only an NMI after the
    console's silence reaches it: `lockup spin all`."""
    return command.split() == ["lockup", "spin", "all"]


def lockup_end(command: str) -> re.Pattern[str] | None:
    """The line that ends a lockup command that returns: for
    `lockup spin <cpu> <ms>`, its CPU has interrupts on again; for
    `lockup status`, its last line, the closest any CPU has come to the
    threshold. None for any other command. On dbg the prompt after either
    seldom arrives whole."""
    words = command.split()
    if words == ["lockup", "status"]:
        return re.compile(r"lockup status: closest to the threshold: [^\n]*\n")
    if words[:2] != ["lockup", "spin"] or len(words) != 4 or not words[2].isdigit():
        return None
    return re.compile(rf"lockup: cpu{int(words[2])} has interrupts on again[^\n]*\n")


def panic_message(lines: list[str]) -> str:
    """The line after the panic banner in |lines|, which holds the banner:
    the panic's message, when the report opens as it should."""
    banner = next(i for i, l in enumerate(lines) if PANIC_BANNER in l)
    return lines[banner + 1] if banner + 1 < len(lines) else ""


def judge_one_panic(command: str, lines: list[str], after: str) -> bool:
    """Says whether the panic |command| led to is the only one, and replays
    the ring between its markers, within the tail its first marker gives.
    |lines| ends with the line after the banner, and |after| is what came in
    the PANIC_SETTLE seconds after that."""
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


def judge_crash(command: str, lines: list[str], after: str) -> bool:
    """Says whether the panic report |command| led to opens as it should:
    the banner, then the message, and nothing from the panicking CPU before
    them; and whether it is the only report. |lines| ends with the line after
    the banner, and |after| is what came in the PANIC_SETTLE seconds after
    that."""
    banner = next(i for i, l in enumerate(lines) if PANIC_BANNER in l)
    message = panic_message(lines)
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
    return judge_one_panic(command, lines, after)


def report_lines(text: str) -> list[str]:
    """The lockup report's own lines in |text|, up to the panic banner: the
    panic's replay of the ring after it repeats them."""
    before = text.split(PANIC_BANNER, 1)[0]
    return [l.strip() for l in before.splitlines() if REPORT_LINE.search(l)]


def judge_lockup(command: str, lines: list[str], after: str, console: Console) -> bool:
    """Says whether the lockup report a lockup command that ends the machine
    led to says what the command asked for, and whether the panic after it
    is whole and alone. |lines| ends with the line after the banner, and
    |after| is what came in the PANIC_SETTLE seconds after that."""
    words = command.split()
    report = report_lines("\n".join(lines))
    headers = [m for m in map(REPORT_HEADER.search, report) if m]
    if len(headers) != 1:
        print(f"{command}: FAIL, {len(headers)} lockup report headers before the panic; want one")
        return False
    stuck, asked_by_nmi = headers[0].group(1), headers[0].group(2)
    blocks = [m for m in map(REPORT_BLOCK.search, report) if m]
    cpus = [b.group(1) for b in blocks]
    if not blocks or len(set(cpus)) != len(cpus):
        print(f"{command}: FAIL, the report's CPU blocks are cpu{' cpu'.join(cpus)}; want each CPU once")
        return False
    missing = [l for l in report if REPORT_MISSING.search(l)]
    if missing:
        print(f"{command}: FAIL, {len(missing)} CPU(s) left no state: {missing[0]}")
        return False
    sent = console.nmi_at is not None
    if words[1] == "nmi":
        if asked_by_nmi is None or sent:
            print(f"{command}: FAIL, want the dump its own NMI asks for, not {headers[0].group(0)}")
            return False
    elif words[2] == "all":
        # No CPU ticks, so no CPU is left to check the others: the only way
        # in is the NMI this script sends after the silence.
        if asked_by_nmi is None or not sent:
            print(f"{command}: FAIL, want a report only through the script's `nmi`, "
                  f"not {headers[0].group(0)}")
            return False
        on = [b.group(1) for b in blocks if b.group(2) == "on"]
        if on:
            print(f"{command}: FAIL, cpu{' cpu'.join(on)} had interrupts on; want every CPU off")
            return False
    else:
        # The stuck CPU is found by another's check, before any silence, and
        # its block is printed last, so that the crash record's tail keeps it.
        if stuck is None or int(stuck) != int(words[2]) or sent:
            print(f"{command}: FAIL, want the detector to find cpu{int(words[2])}, "
                  f"not {headers[0].group(0)}")
            return False
        if blocks[-1].group(1) != stuck or blocks[-1].group(2) != "off":
            print(f"{command}: FAIL, the last block is not cpu{stuck}'s with interrupts off")
            return False
    message = panic_message(lines)
    if not ERROR_CPU.search(message) or "lockup: " not in message:
        print(f"{command}: FAIL, the line after the panic banner is not the lockup's panic message")
        return False
    print(f"{command}: {headers[0].group(0)}; {len(blocks)} CPU blocks")
    return judge_one_panic(command, lines, after)


def judge_silence(what: str, console: Console) -> None:
    """Says that |what| failed because the console went silent, so the
    script sent `nmi`, and what the report that came of it says."""
    report = report_lines(console.nmi_text())
    headers = [m for m in map(REPORT_HEADER.search, report) if m]
    blocks = [m for m in map(REPORT_BLOCK.search, report) if m]
    if headers:
        outcome = f"the lockup report is shown above and kept in the log: {headers[0].group(0)}, " \
                  f"{len(blocks)} CPU blocks"
    else:
        outcome = f"no lockup report came within {REPORT_GRACE:.0f}s"
    print(f"{what}: FAIL, the console was silent for {console.nmi_quiet:.0f}s, so the script "
          f"sent QEMU's monitor `nmi`; {outcome}")


def judge_recovery(console: Console, command: str, lines: list[str], after: str,
                   timeout: float) -> bool:
    """With --reboot: says whether the boot after the panic logged the crash
    record the panic left. Its first line is the panic message, which the line
    after the banner in |lines| carries behind its prefix. The next boot may
    already have said so in |after|; otherwise it is waited for."""
    banner = next(i for i, l in enumerate(lines) if PANIC_BANNER in l)
    message = re.sub(r"^\[[^\]]*\] ", "", lines[banner + 1]).strip()
    deadline = time.monotonic() + timeout
    # The kernel logs the record, then userboot reports the VMO it was handed.
    for who, needle in (("the kernel", r"crashlog: \| "),
                        ("userboot", r"userboot: the previous boot's crash record: ")):
        recovered = re.compile(needle + re.escape(message))
        if not recovered.search(after) and not console.wait_for(recovered, deadline):
            print(f"{command}: FAIL, {who} on the next boot did not report the crash record")
            return False
    print(f"{command}: the next boot reported the crash record, kernel and userboot")
    return True


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("commands", nargs="+", metavar="command", help="a kernel-shell command line")
    parser.add_argument("--out", default="out/bringup.x86", help="GN build directory (default: out/bringup.x86)")
    parser.add_argument("--image", choices=sorted(IMAGES), default="dbg", help="the image to boot (default: dbg)")
    parser.add_argument("--boot-timeout", type=float, default=60, help="seconds for the boot to end (default: 60)")
    parser.add_argument("--command-timeout", type=float, default=120, help="seconds per command (default: 120)")
    parser.add_argument("--delay", type=float, default=0,
                        help="seconds to let the image run after the boot's end before the first command "
                             "(default: 0)")
    parser.add_argument("--all", action="store_true", help="print debug and trace lines too")
    parser.add_argument("--silence", type=float, default=SILENCE,
                        help="seconds of console silence, once the kernel has spoken, after which "
                             f"QEMU is sent `nmi` for a lockup report (default: {SILENCE:.0f}; 0: never)")
    parser.add_argument("--reboot", action="store_true",
                        help="let a guest reset reboot QEMU, and check the crash record a crash leaves")
    args = parser.parse_args(argv)

    enders = [i for i, c in enumerate(args.commands) if ends_machine(c)]
    if enders and enders[0] != len(args.commands) - 1:
        print(f"`{args.commands[enders[0]]}` ends the machine, so it can only be the last command")
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
    console = Console(out, args.image, logdir, args.silence)
    results: list[bool] = []
    try:
        if not console.wait_for(re.compile(re.escape(END_MARKER)),
                                time.monotonic() + args.boot_timeout):
            if console.nmi_at is not None:
                show("boot, after the nmi", console.nmi_text(), args.all)
                judge_silence("boot", console)
            else:
                print(f'boot: no "{END_MARKER}" within {args.boot_timeout:.0f}s (or QEMU exited)')
            return 1
        # From here a quiet console is an idle machine, or the user shell
        # waiting for a line, not a hang: silence counts again only for the
        # one command that stops every CPU.
        console.silence = 0
        console.drain(SETTLE + args.delay)
        for command in args.commands:
            name = command.split()[0]
            test = name.endswith(".test")
            ends = ends_machine(command)
            returns = lockup_end(command)
            console.silence = args.silence if stops_every_cpu(command) else 0
            if ends:
                done = re.compile(re.escape(PANIC_BANNER) + r"[^\n]*\n[^\n]*\n")
                awaited = f'"{PANIC_BANNER}" and the line after it'
            elif test:
                done = re.compile(re.escape(name) + r": (?:ok|FAILED)\b[^\n]*\n")
                awaited = f'"{name}: ok" or FAILED line'
            elif returns is not None:
                done = returns
                awaited = f"line matching /{returns.pattern}/"
            else:
                done = re.compile(re.escape(PROMPT))
                awaited = "prompt"
            console.keep()
            console.type(command)
            ended = console.wait_for(done, time.monotonic() + args.command_timeout,
                                     through=test or ends or returns is not None)
            lines = show(command, console.kept(), args.all)
            if not ended:
                if console.nmi_at is not None:
                    judge_silence(command, console)
                else:
                    print(f"{command}: FAIL, no {awaited} within {args.command_timeout:.0f}s")
                results.append(False)
                break
            if ends:
                after = console.take(PANIC_SETTLE)
                if name == "crash":
                    passed = judge_crash(command, lines, after)
                else:
                    passed = judge_lockup(command, lines, after, console)
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
