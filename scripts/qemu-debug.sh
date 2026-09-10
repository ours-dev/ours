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
#     ./scripts/qemu-debug.sh                       # bringup x86 dbg.1
#     ./scripts/qemu-debug.sh bringup x86 dbg.0
#     PORT=1235 ./scripts/qemu-debug.sh
#     IMAGE=/path/to.obi ELF=/path/to/elf ./scripts/qemu-debug.sh   # off-manifest
#
# Three levels, narrowing: the product says which images exist, the product and
# the architecture together say which build directory (out/<product>.<arch>,
# the same one tools/set.py writes), and the switchset picks one image out of
# that product's set. None of the three is a list kept in this file -- pass a
# wrong one and it prints what the build actually has.
# then in another terminal:
#     lldb -o 'target create out/.debug/kernel.elf' \
#          -o 'target modules load --file kernel.elf --slide 0xffffff8840000000' \
#          -o 'gdb-remote localhost:1234'
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
PORT=${PORT:-1234}

PRODUCT=${1:-${PRODUCT:-bringup}}
ARCH=${2:-${ARCH:-x86}}
SWITCHSET=${3:-${SWITCHSET:-dbg.1}}

# The build directory is not a fourth choice, it IS the first two: tools/set.py
# <product>.<arch> writes args.gn and runs gn gen into exactly this path.
OUT=${OUT:-$ROOT/out/$PRODUCT.$ARCH}

# Captured before the table below assigns to IMAGE/ELF: the table has to run
# first, since it is what defines the defaults, and would otherwise erase what
# the caller passed. ${VAR+x} asks "was it set at all", which is the question
# here -- an empty IMAGE is a mistake, not an absence, and this is `set -u`.
ENV_IMAGE=${IMAGE:-}; ENV_IMAGE_SET=${IMAGE+x}
ENV_ELF=${ELF:-};     ENV_ELF_SET=${ELF+x}

# Which image, and which ELF carries its symbols -- both read out of the build
# rather than written down here.
#
# The pairing is not mechanical and cannot be guessed from the names:
# kernel.dev.obi packages vmours.with-tests, kernel.dev.dbg_1.obi packages
# vmours, and fake.obi packages neither. It also moves: the copy that used to
# live in this script named a binary that had quietly stopped being a build
# target (docs/changes/260909/tooling.md). So kernel_image() writes an
# <image>.debug.json beside each .obi, and this reads that.
#
# The override is tested FIRST, before the build directory is even looked at:
# booting something the build does not describe is exactly the case where there
# is nothing to read, and requiring it would close the escape hatch at the
# moment it is needed. The names match qemu-boot.sh beside this script, which
# already calls it IMAGE. They override together -- naming a new image while
# keeping the build's symbols is precisely the mistake all of this prevents.
if [ -n "$ENV_IMAGE_SET$ENV_ELF_SET" ]; then
    if [ -z "$ENV_IMAGE_SET" ] || [ -z "$ENV_ELF_SET" ]; then
        echo "qemu-debug: IMAGE and ELF override the build together, or neither" >&2
        exit 2
    fi
    IMAGE=$ENV_IMAGE
    ELF=$ENV_ELF
else
    # python3 rather than a JSON parser in sh because the port poller below
    # already depends on it, so this adds no new tool.
    _resolved=$(ROOT="$ROOT" OUT="$OUT" PRODUCT="$PRODUCT" ARCH="$ARCH" \
                SWITCHSET="$SWITCHSET" python3 <<'RESOLVE'
import glob, json, os, sys

root = os.environ["ROOT"]
out = os.environ["OUT"]
product, arch, switchset = (os.environ[k] for k in ("PRODUCT", "ARCH", "SWITCHSET"))


def die(*lines):
    sys.exit("\n".join("qemu-debug: " + l for l in lines))


# Level 1+2. A missing directory is far more often "never configured this
# combination" than "typo", so lead with the command that creates it -- but
# still say which products exist, because that list is the only one this script
# cannot get from the build directory it is being told does not exist.
if not os.path.isdir(out):
    products = sorted(
        os.path.basename(g)[: -len(".gni")]
        for g in glob.glob(os.path.join(root, "products", "*.gni"))
    )
    die(f"no build directory at {out}",
        f"configure it with:  tools/set.py {product}.{arch}",
        f"products the tree defines: {', '.join(products)}")

info_path = os.path.join(out, "product_info.json")
if not os.path.exists(info_path):
    die(f"no {info_path} -- run 'prebuilt/gn/gn gen {out}' first")

info = json.load(open(info_path))[0]
if info["name"] != product:
    die(f"{out} was generated for product '{info['name']}', not '{product}'",
        f"re-run:  tools/set.py {product}.{arch}")


def normalise(label):
    """//dir -> //dir:dir, so a product's shorthand matches an image's label."""
    return label if ":" in label else f"{label}:{label.rsplit('/', 1)[-1]}"


# Level 3. The directory holds more images than the product does -- fake.obi is
# a dependency of //:default, not of any product -- and two images can share a
# switchset, so narrowing by the product first is what makes the switchset a
# unique key rather than an ambiguous one.
owned = {normalise(k["package"]) for k in info["kernel_images"]}

MARKER = "//build/switch/set/"


def switchset_of(kernel):
    """The directory under //build/switch/set that built this kernel."""
    label = kernel["label"]
    return label.split(MARKER, 1)[1].split(":", 1)[0] if MARKER in label else None


# Everything the directory describes, then the product's share of it. Both are
# kept: an image outside the product is not a candidate, but knowing it exists
# is what lets a wrong answer below say something better than "no".
candidates, foreign, broken = {}, {}, []
for manifest in sorted(glob.glob(os.path.join(out, "*.debug.json"))):
    records = json.load(open(manifest))
    identity = next((r for r in records if "obi" in r), None)
    if identity is None:
        continue
    kernels = [r for r in records if r.get("environment") == "kernel"]

    # Not `continue`: an image whose kernel cannot be identified is a thing
    # someone just changed, and silently dropping it out of the menu is the
    # worst possible way to report that. Collected and named instead.
    if len(kernels) != 1 or switchset_of(kernels[0]) is None:
        broken.append((identity["image"], kernels))
        continue

    where = candidates if identity["label"] in owned else foreign
    got = switchset_of(kernels[0])

    # Two images of one product in one switchset make the switchset ambiguous,
    # and picking either would be a coin toss the caller never sees. The third
    # level stops being a key at that point, so say so rather than answer.
    if got in where:
        where[got] = ("AMBIGUOUS", where[got], (identity, kernels[0]))
    else:
        where[got] = (identity, kernels[0])

if not candidates and not foreign:
    if broken:
        die(*[f"{out} describes no image this script can resolve; every one of them"
              " has an ambiguous kernel:"]
             + [f"  {n}: {len(k)} ELFs in the kernel environment"
                + (" -- " + ", ".join(e["label"] for e in k) if k else "")
                for n, k in broken]
             + ["a bootable image is expected to carry exactly one kernel; a second"
                " kernel_package would do this"])
    die(f"{out} describes no images at all",
        f"has 'prebuilt/ninja/ninja -C {out}' been run?")

if not candidates:
    die(f"{out} describes no image belonging to product '{product}'",
        "it does describe: "
        + ", ".join(f"{i['image']} ({s})" for s, (i, _) in sorted(foreign.items())),
        f"add one to products/{product}.gni to reach it from here")

chosen = candidates.get(switchset)

if isinstance(chosen, tuple) and chosen and chosen[0] == "AMBIGUOUS":
    names = ", ".join(i["image"] for i, _ in chosen[1:])
    die(f"product '{product}' has more than one image in switchset '{switchset}': {names}",
        "the switchset stops identifying an image once two of them share it;",
        "give one of them its own switchset, or select by image name instead")

if chosen is None:
    have = ", ".join(f"{s} ({i['image']})"
                     for s, v in sorted(candidates.items()) if v[0] != "AMBIGUOUS"
                     for i in (v[0],))
    lines = [f"product '{product}' has no image built in switchset '{switchset}'",
             f"it offers: {have}"]
    if switchset in foreign:
        other = foreign[switchset]
        name = other[1][0]["image"] if other[0] == "AMBIGUOUS" else other[0]["image"]
        lines.append(f"note: {name} is built in '{switchset}' but belongs to no product")
    if broken:
        lines.append("note: ignored " + ", ".join(
            f"{n} ({len(k)} kernel ELFs)" for n, k in broken))
    die(*lines)

identity, kernel = chosen
print(os.path.join(out, identity["obi"]))
print(os.path.join(out, kernel["debug"]))
print(identity["image"])
RESOLVE
    ) || exit $?

    IMAGE=$(printf '%s\n' "$_resolved" | sed -n 1p)
    ELF=$(printf '%s\n' "$_resolved" | sed -n 2p)
    echo "qemu-debug: $PRODUCT.$ARCH $SWITCHSET -> $(printf '%s\n' "$_resolved" | sed -n 3p)"
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
# Note the path: out/.debug, NOT $OUT/.debug. $OUT moves with the product and
# the architecture, and the debugger configuration has to name one fixed path
# -- naming a varying one would mean referencing the product and architecture
# inputs from the launch configuration as well as from the task, and VS Code
# asks for an input once per section, so every prompt would come up twice.
#
# One pointer per checkout is also what the situation actually is: one gdbstub
# port, one debugger, one kernel under it.
mkdir -p "$ROOT/out/.debug"
ln -sfn "$ELF" "$ROOT/out/.debug/kernel.elf"

# The first line the VS Code task keys on; the last one is the ready line at
# the bottom of this script.
echo "qemu-debug: booting $IMAGE"
echo "qemu-debug: symbols $ELF -> $ROOT/out/.debug/kernel.elf"
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
