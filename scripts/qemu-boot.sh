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

exec "$ROOT/prebuilt/qemu/bin/qemu-system-x86_64" \
    -M q35 -cpu "${CPU:-Nehalem}" -m "${MEM:-512}" -smp "${SMP:-1}" \
    -no-reboot -nographic \
    -drive if=pflash,format=raw,unit=0,readonly=on,file="$WORK/code.fd" \
    -drive if=pflash,format=raw,unit=1,file="$WORK/vars.fd" \
    -drive format=raw,file="$WORK/esp.img" \
    -serial mon:stdio "$@"
