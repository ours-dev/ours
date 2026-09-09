#!/bin/sh
# Boot an ours kernel image under QEMU via GRUB.
#
# QEMU's own -kernel implements Multiboot 1 only, and the phys bootshim carries
# a Multiboot2 header (0xE85250D6), so it has to come up through GRUB the same
# way it does on real hardware -- which is also what the design calls for:
# GRUB (Multiboot2) -> bootshim -> physload -> kernel.
#
# QEMU and the GRUB tools live in prebuilt/ (Homebrew bottles, extracted, never
# installed). Their dylibs record absolute /opt/homebrew paths that do not
# exist here; DYLD_FALLBACK_LIBRARY_PATH is what dyld consults when a recorded
# path is missing, so pointing it at the extracted lib/ resolves them without
# rewriting a single install_name.
set -eu

# CPU model matters: the kernel is built -march=x86-64-v2, and QEMU's default
# qemu64 model is roughly v1 -- it does not advertise POPCNT, so the first
# popcntq the compiler emits raises #UD. Nehalem is the canonical v2 baseline.

# NUMA=1 swaps the flat machine for a four-node topology, so the kernel's NUMA
# paths (SRAT parsing, per-node zone queues, node-affine allocation) see
# something to work with. It needs a kernel actually built for it -- the switch
# is a build argument, not a runtime one:
#
#     gn gen out/numa-on --args='mm_numa = true'
#     ninja -C out/numa-on default
#     OUT=$PWD/out/numa-on IMAGE=$PWD/out/numa-on/kernel.dev.dbg_1.obi \
#         NUMA=1 ./scripts/qemu-boot.sh
#
# The layout is the one //.legacy/ours-dev/scripts/shell/runvm.sh described,
# kept because its distance matrix is deliberately not uniform -- a kernel that
# only ever sees 10/20 distances never exercises the fallback ordering:
#
#   node 0 (cpu 0-1) --10-- node 1 (cpu 2-3)
#      |  \                     |
#      20    \30            20  |
#      |        \  30           |
#   node 2 (cpu 4-6) --10-- node 3 (cpu 7)
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=${OUT:-$ROOT/out/bringup.x86}
IMAGE=${IMAGE:-$OUT/kernel.dev.obi}
SHIM=$OUT/kernel.phys_x86.phys.dbg_0/obj/kernel/arch/x86/phys/bootshim/bootshim
WORK=${WORK:-/tmp/ours-boot}

export DYLD_FALLBACK_LIBRARY_PATH="$ROOT/prebuilt/qemu/lib:$ROOT/prebuilt/grub/lib"
export PATH="$ROOT/prebuilt/grub/bin:$PATH"

[ -f "$SHIM" ]  || { echo "missing shim: $SHIM" >&2; exit 1; }
[ -f "$IMAGE" ] || { echo "missing image: $IMAGE" >&2; exit 1; }

rm -rf "$WORK"; mkdir -p "$WORK/esp/EFI/BOOT" "$WORK/esp/boot/grub"
cp "$SHIM" "$WORK/esp/boot/bootshim"
cp "$IMAGE" "$WORK/esp/boot/image.obi"
cat > "$WORK/esp/boot/grub/grub.cfg" <<'CFG'
set timeout=0
set default=0
terminal_output console
menuentry "ours" {
    multiboot2 /boot/bootshim
    module2 /boot/image.obi
    boot
}
CFG

# grub-mkimage, not grub-mkrescue. mkrescue insists on data files (unicode.pf2,
# locales) under the pkgdatadir it was compiled with -- /opt/homebrew/... --
# and there is no env var to redirect it. mkimage takes -d for the modules and
# needs nothing else, and the font is only for the graphical menu we do not use.
GRUB_PLATFORM=$ROOT/prebuilt/grub/lib/x86_64-elf/grub/x86_64-efi
x86_64-elf-grub-mkimage -d "$GRUB_PLATFORM" -O x86_64-efi -p /boot/grub \
    -o "$WORK/esp/EFI/BOOT/BOOTX64.EFI" \
    normal multiboot2 configfile echo test part_gpt part_msdos fat iso9660 \
    serial terminal ls minicmd >"$WORK/mkimage.log" 2>&1 \
    || { echo "grub-mkimage failed:" >&2; cat "$WORK/mkimage.log" >&2; exit 1; }

# A bare FAT volume is enough: the firmware looks for /EFI/BOOT/BOOTX64.EFI on
# any FAT partition, and a whole-disk FAT with no partition table qualifies.
dd if=/dev/zero of="$WORK/esp.img" bs=1m count=64 status=none
MTOOLS_SKIP_CHECK=1 mformat -i "$WORK/esp.img" -F -v OURS ::
MTOOLS_SKIP_CHECK=1 mcopy -i "$WORK/esp.img" -s "$WORK/esp/EFI" "$WORK/esp/boot" ::

FW=$ROOT/prebuilt/qemu/share/qemu
cp "$FW/edk2-x86_64-code.fd" "$WORK/code.fd"
cp "$FW/edk2-i386-vars.fd" "$WORK/vars.fd"

NUMA_ARGS=""
if [ "${NUMA:-0}" != "0" ]; then
    # Defaults that make the topology above come out whole: 8 CPUs to split
    # 2/2/3/1, and memory that divides by four.
    MEM=${MEM:-1024}
    SMP=${SMP:-8}
    NODE_MEM=$((MEM / 4))
    [ $((NODE_MEM * 4)) -eq "$MEM" ] || {
        echo "MEM=$MEM does not divide into 4 NUMA nodes" >&2; exit 1; }

    # Every node needs its own backend object: with -numa node,mem=... QEMU only
    # splits the -m total, and it has been deprecated for exactly the reason it
    # matters here -- a backend is what gives the guest a real ACPI SRAT entry.
    for id in 0 1 2 3; do
        NUMA_ARGS="$NUMA_ARGS -object memory-backend-ram,size=${NODE_MEM}M,id=m$id"
    done
    NUMA_ARGS="$NUMA_ARGS -numa node,nodeid=0,cpus=0-1,memdev=m0"
    NUMA_ARGS="$NUMA_ARGS -numa node,nodeid=1,cpus=2-3,memdev=m1"
    NUMA_ARGS="$NUMA_ARGS -numa node,nodeid=2,cpus=4-6,memdev=m2"
    NUMA_ARGS="$NUMA_ARGS -numa node,nodeid=3,cpus=7,memdev=m3"
    # Both directions spelled out, as the legacy script did: QEMU mirrors a
    # single direction on its own, but saying it twice keeps the matrix
    # readable next to the diagram at the top. Written out rather than looped
    # because the loop would have to go through `set --`, and that would eat
    # the caller's own arguments -- they are forwarded to QEMU at the bottom.
    for d in "0,dst=1,val=10" "1,dst=0,val=10" "2,dst=3,val=10" "3,dst=2,val=10" \
             "0,dst=2,val=20" "2,dst=0,val=20" "1,dst=3,val=20" "3,dst=1,val=20" \
             "0,dst=3,val=30" "3,dst=0,val=30" "1,dst=2,val=30" "2,dst=1,val=30"; do
        NUMA_ARGS="$NUMA_ARGS -numa dist,src=$d"
    done
fi

# Four CPUs by default, not one -- and settled after the NUMA block above, so
# that a NUMA run still gets the eight its topology is written for.
#
# The kernel brings up every CPU the MADT reports and spreads threads over
# them, and almost none of that code is exercised by a uniprocessor boot: the
# 16-bit trampoline, the per-CPU areas, the reschedule IPIs, the remote
# run-queue inserts, the local APIC's own software enable. Every one of those
# held a bug that a single-CPU boot could not see. Pass SMP=1 to compare
# against a uniprocessor run when a failure might be an SMP race.
SMP=${SMP:-4}

# shellcheck disable=SC2086
exec "$ROOT/prebuilt/qemu/bin/qemu-system-x86_64" \
    -M q35 -cpu "${CPU:-Nehalem}" -m "${MEM:-512}" -smp "$SMP" \
    $NUMA_ARGS \
    -no-reboot -nographic \
    -drive if=pflash,format=raw,unit=0,readonly=on,file="$WORK/code.fd" \
    -drive if=pflash,format=raw,unit=1,file="$WORK/vars.fd" \
    -drive format=raw,file="$WORK/esp.img" \
    -serial mon:stdio "$@"
