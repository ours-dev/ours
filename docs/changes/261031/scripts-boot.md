# scripts-boot changes

## `DISK=` attaches a virtio-blk disk to the boot scripts, off by default (2026-10-31)

**Files**
- `scripts/qemu_boot.py` — new `disk_args()`, the `DISK` entry in the list of environment variables, and the two QEMU arguments it returns, added to the command line when `DISK` is set.
- `.agents/tasks/interrupt-system/DESIGN.md`, `.agents/tasks/driver-capabilities/DESIGN.md` — the open question about the device's Interrupt Line register is answered (no), and the plan's sentence that relied on it says so.

**Symptom**
No machine these scripts start has a PCI device with an interrupt of its own. q35's own functions (host bridge, VGA, SATA, ISA bridge, SMBus) are all a guest finds, so a user-space block driver, and steps I2 to I5 of `.agents/tasks/interrupt-system/PLAN.md`, have nothing to find, take an interrupt from or test against.

**Cause**
Not a mistake: a missing input. QEMU provides `virtio-blk-pci` with nothing written, only a command line. Two details decide how the command line must look.

- **Two machines, one file.** `boot_check.py` starts the dbg and the rel image at the same time. A raw file opened read-write is locked by QEMU, so with `DISK` set the second machine would die at start-up, and every check of the default images would fail the moment anyone set the variable. `snapshot=on` opens the file read-only and sends the guest's writes to a private temporary overlay that goes away with the machine. Both machines run, and the file is never changed.
- **A comma in a path** is the separator of QEMU's option string, and is written twice to mean itself.

**Change**
The variable, its meaning and the helper:

```python
def disk_args(disk: str) -> list[str]:
    path = Path(disk).resolve()
    if not path.is_file():
        die(f"DISK={disk} is not a file")
    name = str(path).replace(",", ",,")
    return ["-drive", f"file={name},if=none,id=d0,format=raw,snapshot=on",
            "-device", "virtio-blk-pci,drive=d0"]
```

and, after the ESP drive in the command line:

```python
        "-drive", f"format=raw,file={esp_img}",
        *(disk_args(disk) if disk else []),
        "-serial", "mon:stdio",
```

An unset or empty `DISK` adds nothing, so the default machine is the one it was. `scripts/kshell_run.py` needs no change: it builds its environment from `os.environ`, so the variable reaches `qemu_boot.py` already.

Rejected: opening the file read-write, which fails on the lock as above; a second variable to choose between the two, which nothing needs yet.

**Reference**
None, this project's own tooling. The design reason is D6 (a) of `.agents/tasks/interrupt-system/DESIGN.md`: a test device behind a variable that is off by default, so the default images and `boot_check` stay as they were.

**Verification**
Run in a cloud container with substituted tools (QEMU 8.2.2 and Ubuntu's OVMF 2024.02, not the QEMU 11.1.1 the project normally uses; GRUB 2.12-1ubuntu7; gn `1000 (1a7c151)`; no KVM, so every boot is emulated). The images were built before the change; the change touches no build input.

| Run | dbg | rel | host tests |
|---|---|---|---|
| no `DISK` | PASS, 15.6 s, 44 passed, 4 skipped, 0 failed | PASS, 12.6 s, 41 passed, 8 skipped, 0 failed, 6/6 required | PASS, 151 passed, 0 failed |
| `DISK=disk.img` (16 MiB, zeros) | PASS, 12.3 s, 44 passed, 4 skipped, 0 failed | PASS, 9.8 s, 41 passed, 8 skipped, 0 failed, 6/6 required | PASS, 151 passed, 0 failed |

That both pass with the same counts shows the device does no harm; it does not show the device is there, so it was asked for through a QEMU monitor socket (`-monitor unix:...`, forwarded after the options):

- With `DISK`: `info pci` lists 7 functions, among them `Bus 0, device 3, function 0: SCSI controller: PCI device 1af4:1001`, subsystem `1af4:0002` (virtio, device type 2, block). Without it, 6 functions and none with `1af4:1001`.
- After the guest has booted (25 s), the same function shows `IRQ 11, pin A`, `BAR0: I/O at 0x6000 [0x607f]`, `BAR1: 32 bit memory at 0x810c5000 [0x810c5fff]` and `BAR4: 64 bit prefetchable memory at 0x800000000 [0x800003fff]`. Asked at the first moment the socket exists, it shows `IRQ 0` and unassigned BARs: the firmware had not run yet, so that value says nothing.
- The Interrupt Line register therefore reads 11, which is not a GSI. The plan assumed it would be (`.agents/tasks/interrupt-system/DESIGN.md`, "How a device gets its interrupt"). Where the driver takes its GSI from is now an open question for the user, to be decided before I5.
- Two machines on one 16 MiB file, `timeout`-bounded: read-write, the second dies with `Failed to get "write" lock`; with `snapshot=on` both run, and afterwards the file is still 16777216 bytes and its SHA-256 still begins `080acf35a507ac98`, which is that of the 16 MiB of zeros it was made of (computed separately, not read off before and after).
- `DISK` naming a file that does not exist: `qemu-boot: DISK=... is not a file`, exit status 1.

Not measured: a guest using the device (that is I5); the same checks on QEMU 11.1.1 and the project's edk2, where the `IRQ` value in particular should be read again.

**Deliberately not done**
- A way to keep the guest's writes. `snapshot=on` discards them, which suits interrupt tests and a read of block 0. A storage layer will want a disk that persists, and then two machines must not share it; that is decided with the first user of it.
- A helper to make the disk file. The user supplies one (`truncate -s 16M disk.img` is enough).
- `disable-legacy`: the device is QEMU's default transitional one (`1af4:1001`, a legacy I/O BAR and a modern capability BAR 4). Which flavour a driver wants is for the driver to say.
- Any kernel or image change.

**How it was found**
None.

**The user's prediction**
None.

**Three questions you should be able to answer**
1. `boot_check.py` runs two images at once. What happens to the second machine if the disk is opened read-write, and what does `snapshot=on` change so that both run?
2. Why does a pass of both images with `DISK` set not show that the device exists, and what did show it?
3. What did the device's Interrupt Line register read after boot, and why does that leave the plan without a source for the GSI?
