#!/usr/bin/env python3
"""Give Claude Code a live lldb through lldb's own MCP server.

lldb can serve the Model Context Protocol itself (`protocol-server start MCP`,
in Apple's lldb and in upstream LLDB 21+). It exposes two tools: `command`,
which runs one lldb command and returns its output, and `debugger_list`. But it
serves only on a socket, from inside a running debugger, while Claude Code
starts its MCP servers as child processes that speak over stdio. This script
joins the two: it creates a debugger through lldb's Python module, imports
scripts/lldb_ours.py (cl, cpus, nodes, kthread), opens the server on a free
port, and copies the newline-delimited JSON-RPC stream between its own stdio
and that socket. Each Claude session gets its own debugger, gone when the
session ends.

Why a Python host rather than the lldb command-line driver: the driver's main
thread sits in its interactive command loop holding the script interpreter, so
any Python-backed command arriving over MCP -- `script`, and every command in
lldb_ours.py -- waits for it forever and wedges the server behind it. Here the
main thread only waits on an event, which releases the interpreter.

Registered in .mcp.json. How to drive it against a kernel under QEMU is in
.agents/skills/verify/references/debugging.md.

Environment:
    LLDB    the lldb whose Python module to load (default: /usr/bin/lldb; the
            prebuilt LLVM tree ships no lldb)
"""

# macOS still ships Python 3.9 as python3; this keeps the annotations lazy.
from __future__ import annotations

import os
import re
import socket
import subprocess
import sys
import threading
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
LLDB = os.environ.get("LLDB", "/usr/bin/lldb")
# What the server prints once it is up:
#   MCP server started with connection listeners: connection://[127.0.0.1]:50087
LISTENING = re.compile(r"\[127\.0\.0\.1\]:(\d+)")
# lldb's module is built against one Python -- the Command Line Tools' 3.9 on
# this Mac -- so a different python3 first on PATH re-runs the script under it.
SYSTEM_PYTHON = "/usr/bin/python3"
REEXEC_GUARD = "LLDB_MCP_REEXEC"


def die(message: str) -> None:
    # stderr, never stdout: stdout carries the protocol, and a stray line there
    # would be read by Claude Code as a malformed message.
    sys.stderr.write(f"lldb-mcp: {message}\n")
    sys.exit(1)


def import_lldb():
    path = subprocess.run([LLDB, "-P"], capture_output=True, text=True).stdout.strip()
    sys.path.insert(0, path)
    try:
        import lldb
    except ImportError as err:
        if os.environ.get(REEXEC_GUARD) != "1":
            os.execve(SYSTEM_PYTHON, [SYSTEM_PYTHON, __file__, *sys.argv[1:]],
                      dict(os.environ, **{REEXEC_GUARD: "1"}))
        die(f"cannot import lldb's Python module from {path}: {err}")
    return lldb


def start_server(lldb) -> tuple[object, int]:
    debugger = lldb.SBDebugger.Create()
    # Async, so `continue` returns at once ("Process 1 resuming") and the caller
    # polls `process status`. In sync mode it would hold the server's single
    # request thread until the kernel stopped -- forever, if it never does.
    debugger.SetAsync(True)
    interpreter = debugger.GetCommandInterpreter()

    def run(command: str):
        result = lldb.SBCommandReturnObject()
        interpreter.HandleCommand(command, result)
        return result

    imported = run(f"command script import {ROOT / 'scripts/lldb_ours.py'}")
    if not imported.Succeeded():
        die("cannot import lldb_ours.py: " + (imported.GetError() or ""))
    started = run("protocol-server start MCP listen://127.0.0.1:0")
    match = LISTENING.search(started.GetOutput() or "")
    if not started.Succeeded() or not match:
        die("lldb did not start its MCP server: " + (started.GetError() or started.GetOutput() or ""))

    def pump_events() -> None:
        # In async mode a process's public state -- what `process status`,
        # `bt` and `continue` consult -- only advances when its events are
        # taken off the debugger's listener. The command-line driver does that
        # on its event thread; with no driver, this loop has to, or the state
        # freezes at the attach and lldb contradicts itself ("already running"
        # from `continue`, "not running" from `process interrupt`).
        listener = debugger.GetListener()
        event = lldb.SBEvent()
        while True:
            listener.WaitForEvent(1, event)

    threading.Thread(target=pump_events, daemon=True).start()
    return debugger, int(match.group(1))


def main() -> None:
    # Keep a private copy of stdout for the protocol, then point fd 1 at stderr.
    # The debugger and every Python script share this process: lldb_ours.py
    # prints a banner on import, lldb writes process events ("Process 1
    # stopped") to its output handle, and any of that landing on the protocol
    # channel is a line Claude Code cannot parse.
    protocol_out = os.fdopen(os.dup(1), "wb")
    os.dup2(2, 1)
    sys.stdout = sys.stderr

    # Relative paths in commands (target create out/.debug/kernel.elf) mean the
    # repo root, whatever directory the client started us in.
    os.chdir(ROOT)
    lldb = import_lldb()
    debugger, port = start_server(lldb)

    try:
        sock = socket.create_connection(("127.0.0.1", port), timeout=10)
    except OSError as err:
        die(f"cannot reach lldb's MCP server on port {port}: {err}")
    sock.settimeout(None)
    done = threading.Event()

    def to_lldb() -> None:
        # Claude -> lldb. EOF on stdin means the session is over.
        try:
            for line in sys.stdin.buffer:
                sock.sendall(line)
        except OSError:
            pass
        done.set()

    def to_claude() -> None:
        # lldb -> Claude, one message per line, flushed at once: a message left
        # sitting in the buffer is a tool call Claude waits on forever.
        try:
            for line in sock.makefile("rb"):
                protocol_out.write(line)
                protocol_out.flush()
        except OSError:
            pass
        done.set()

    for pump in (to_lldb, to_claude):
        threading.Thread(target=pump, daemon=True).start()
    # Waiting releases the interpreter; see the module docstring for why that
    # matters.
    done.wait()
    sock.close()
    lldb.SBDebugger.Destroy(debugger)


if __name__ == "__main__":
    main()
