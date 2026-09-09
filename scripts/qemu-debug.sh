#!/bin/sh
# Boot an ours kernel image under QEMU, halted, with the gdbstub open, and
# publish the matching kernel ELF at a fixed path for the debugger.
#
# This is the half of one-click debugging that VS Code cannot express in
# tasks.json: it has to clear away a previous session, start QEMU, wait until
# the stub is actually accepting connections, and only then announce itself --
# because the debugger attaches the moment the task reports ready, and
# attaching to a port nothing is listening on fails the whole launch.
#
# Usable on its own too:
#     ./scripts/qemu-debug.sh              # dbg.1 (the default)
#     ./scripts/qemu-debug.sh dev
#     ./scripts/qemu-debug.sh fake
#     PORT=1235 ./scripts/qemu-debug.sh dbg.1
#     IMAGE=/path/to.obi ELF=/path/to/elf ./scripts/qemu-debug.sh   # off-table
# then in another terminal:
#     lldb -o 'target create out/bringup.x86/.debug/kernel.elf' \
#          -o 'target modules load --file kernel.elf --slide 0xffffff8840000000' \
#          -o 'gdb-remote localhost:1234'
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=${OUT:-$ROOT/out/bringup.x86}
PORT=${PORT:-1234}
VARIANT=${1:-${VARIANT:-dbg.1}}

# Captured before the table below assigns to IMAGE/ELF: the table has to run
# first, since it is what defines the defaults, and would otherwise erase what
# the caller passed. ${VAR+x} asks "was it set at all", which is the question
# here -- an empty IMAGE is a mistake, not an absence, and this is `set -u`.
ENV_IMAGE=${IMAGE:-}; ENV_IMAGE_SET=${IMAGE+x}
ENV_ELF=${ELF:-};     ENV_ELF_SET=${ELF+x}

# Which image, and which ELF carries its symbols.
#
# The pairing is the entire reason this table exists rather than living in the
# debugger configuration: it is NOT mechanical. kernel.dev.obi packages
# vmours.with-tests, not vmours -- two different binaries in the same switchset
# directory, and the wrong one puts every breakpoint tens of kilobytes off (see
# docs/changes/260909/tooling.md). fake.obi is not vmours at all. Anything that
# has to name an image and an ELF separately can get them out of step; here
# they are chosen together, once.
case "$VARIANT" in
    dbg.1)
        IMAGE=$OUT/kernel.dev.dbg_1.obi
        ELF=$OUT/kernel_x86.dbg.1/obj/kernel/products/bin/vmours
        ;;
    dev)
        IMAGE=$OUT/kernel.dev.obi
        ELF=$OUT/kernel_x86.dbg.0/obj/kernel/products/bin/vmours.with-tests
        ;;
    fake)
        # Kept because the pairing is right and costs one line, but NOT offered
        # in the editor's dropdown: fake.obi does not currently boot -- physboot
        # answers "Invalid kernel package: 4" before the kernel is entered. See
        # docs/repairs/260909/products.md.
        IMAGE=$OUT/fake.obi
        ELF=$OUT/kernel_x86.dbg.0/obj/kernel/tests/fake-kernel/fake-kernel
        ;;
    *)
        echo "qemu-debug: unknown variant '$VARIANT' (want: dbg.1, dev, fake)" >&2
        exit 2
        ;;
esac

# The table stays overridable for anyone booting something it does not know
# about. The names are IMAGE and ELF because qemu-boot.sh beside this script
# already calls it IMAGE -- one variable meaning two things across two sibling
# scripts costs more than the few characters a distinct name would save.
#
# They override together: naming a new image while keeping the table's symbols
# is precisely the mistake the table exists to prevent.
if [ -n "$ENV_IMAGE_SET$ENV_ELF_SET" ]; then
    if [ -z "$ENV_IMAGE_SET" ] || [ -z "$ENV_ELF_SET" ]; then
        echo "qemu-debug: IMAGE and ELF override the table together, or neither" >&2
        exit 2
    fi
    IMAGE=$ENV_IMAGE
    ELF=$ENV_ELF
fi

# Both checked before QEMU is touched. A missing ELF is the nastier of the two:
# QEMU would boot fine and the attach would come up with no symbols at all,
# which reads like a debugger problem rather than a missing build.
[ -f "$IMAGE" ] || { echo "qemu-debug: no image at $IMAGE -- build it first" >&2; exit 1; }
[ -f "$ELF" ] || { echo "qemu-debug: no kernel ELF at $ELF -- build it first" >&2; exit 1; }

# The fixed path the debugger attaches to. Publishing it here, from the same
# case arm that chose the image, is what makes "wrong symbols for this boot"
# unrepresentable: there is one name, and whoever booted last owns it.
#
# It is a symlink rather than a copy so that a rebuild is picked up without
# re-running this, and lldb reports the module basename as `kernel.elf` (it
# does not resolve the link), which is what `target modules load --file` in the
# debugger configuration matches on.
#
# Published only after both checks above pass, and note what is deliberately
# NOT done on failure: the old link is left alone. A second run that fails
# because something is unbuilt must not break the QEMU still running from the
# first one -- that link is still the right one for it, and
# "Kernel: attach to a QEMU I started myself" depends on it.
#
# -n is not in POSIX ln (only -f -i -L -P -s are), but every ln this repo will
# meet has it, and it is only insurance: the target is always a plain file, so
# -f alone would do.
mkdir -p "$OUT/.debug"
ln -sfn "$ELF" "$OUT/.debug/kernel.elf"

# The first line the VS Code task keys on; the last one is the ready line at
# the bottom of this script.
echo "qemu-debug: booting $IMAGE"
echo "qemu-debug: symbols $ELF -> $OUT/.debug/kernel.elf"
echo "qemu-debug: gdbstub on localhost:$PORT"

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
# be attached to, start_kernel is long past.
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
# debugger attaches as soon as it appears -- so everything it depends on, the
# symlink included, is already in place above.
echo "qemu-debug: gdbstub ready on localhost:$PORT"

wait "$QEMU_PID"
