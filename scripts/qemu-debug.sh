#!/bin/sh
# Boot an ours kernel image under QEMU, halted, with the gdbstub open.
#
# This is the half of one-click debugging that VS Code cannot express in
# tasks.json: it has to clear away a previous session, start QEMU, wait until
# the stub is actually accepting connections, and only then announce itself --
# because the debugger attaches the moment the task reports ready, and
# attaching to a port nothing is listening on fails the whole launch.
#
# Usable on its own too:
#     ./scripts/qemu-debug.sh                       # dbg.1 image, port 1234
#     IMAGE=$PWD/out/bringup.x86/kernel.dev.obi ./scripts/qemu-debug.sh
# then in another terminal:
#     lldb -o 'target create out/bringup.x86/kernel_x86.dbg.1/obj/kernel/products/bin/vmours' \
#          -o 'target modules load --file vmours --slide 0xffffff8840000000' \
#          -o 'gdb-remote localhost:1234'
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=${OUT:-$ROOT/out/bringup.x86}
IMAGE=${IMAGE:-$OUT/kernel.dev.dbg_1.obi}
PORT=${PORT:-1234}

# The first line the VS Code task keys on; the last one is the ready line at
# the bottom of this script.
echo "qemu-debug: booting $IMAGE, gdbstub on localhost:$PORT"

# Two debuggers on one port take each other down, and the survivor is not
# always the one you just started -- so a stale QEMU or lldb from an
# interrupted session has to go before this one comes up. Only processes
# actually holding the port are touched; an ordinary `qemu-boot.sh` running
# beside this one is left alone.
stale=$(lsof -ti "tcp:$PORT" 2>/dev/null || true)
if [ -n "$stale" ]; then
    echo "qemu-debug: killing stale process(es) on port $PORT: $stale"
    # shellcheck disable=SC2086
    kill $stale 2>/dev/null || true
    sleep 1
fi

# -S halts the guest before its first instruction, which is what makes a
# breakpoint on early boot possible at all: by the time a running kernel could
# be attached to, start_kernel is long past. -s is QEMU's shorthand for
# `-gdb tcp::1234`; PORT overrides it via the long form.
QEMU_PID=""
cleanup() {
    if [ -n "$QEMU_PID" ]; then
        kill "$QEMU_PID" 2>/dev/null || true
    fi
}
trap cleanup EXIT INT TERM

IMAGE="$IMAGE" OUT="$OUT" "$ROOT/scripts/qemu-boot.sh" -S -gdb "tcp::$PORT" &
QEMU_PID=$!

# Poll rather than sleep: the ESP is rebuilt (grub-mkimage, mformat, mcopy)
# before QEMU is even exec'd, and how long that takes is not a constant worth
# guessing at.
python3 - "$PORT" "$QEMU_PID" <<'PY'
import socket, sys, time, os
port, qemu_pid = int(sys.argv[1]), int(sys.argv[2])
deadline = time.monotonic() + 60
while time.monotonic() < deadline:
    try:
        os.kill(qemu_pid, 0)
    except OSError:
        sys.exit("qemu-debug: QEMU exited before the gdbstub came up")
    try:
        with socket.create_connection(("127.0.0.1", port), timeout=0.25):
            sys.exit(0)
    except OSError:
        time.sleep(0.1)
sys.exit(f"qemu-debug: the gdbstub did not open on port {port} within 60s")
PY

# The line the VS Code task watches for. Nothing else prints it, and the
# debugger attaches as soon as it appears.
echo "qemu-debug: gdbstub ready on localhost:$PORT"

wait "$QEMU_PID"
