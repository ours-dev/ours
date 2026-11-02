# pc-ps2 outstanding repairs

## 1. A key the keyboard sends while the reset waits for an answer is taken for the answer, and the driver gives up

**Severity**
Medium: wrong, but not reached today. With `-nographic` QEMU's PS/2 keyboard sends only what the monitor's `sendkey` types, and boot_check types after `pc-ps2: ready`; on hardware, a key pressed while the machine boots is enough.

**Symptom**
Latent, cannot be triggered today. Where a key is pressed during boot, the driver logs `pc-ps2: disabling the keyboard's keys: answered 0x1e where 0xfa was due`, with that key's make code in place of 0x1e, then `pc-ps2: resetting the keyboard controller: BadState`, and init logs `init: pc-ps2 exited with 1`: the machine has no keyboard until it reboots.

**Cause**
`reset()` (`system/driver/pc-ps2/i8042.cpp`) holds the first port off while it tests the controller, then turns it on and only then has `identify_keyboard()` send SCAN_DISABLE. A PS/2 keyboard keeps the keys pressed while its port is held off, and so does QEMU's, and sends them once the port comes on: they arrive ahead of SCAN_DISABLE's ACK. `expect()` takes the first byte that comes as the answer, so a scancode there fails the reset with BadState. Keys typed before the driver starts end the same way: `flush()` drops the one byte the controller holds, but the keyboard sends the rest only once the port is on.

**Change**
While a command to the keyboard waits for its ACK, a byte that is neither ACK (0xFA) nor RESEND (0xFE) is a key the keyboard sent before it took the command: it is dropped, up to `MAX_BUFFERED` of them, and the wait goes on. SCAN_DISABLE's ACK ends that: the keyboard sends no key after it, so IDENTIFY's answer and SCAN_ENABLE's ACK need nothing more. Ruled out: retrying the whole reset, which only moves the window; a second `flush()` before SCAN_DISABLE, which cannot drop what the keyboard has not sent yet.

**Reference**
Linux `ps2_handle_ack()` (`drivers/input/serio/libps2.c:498-560`): a byte other than an ACK while it waits for the ACK of a command's first byte is not an error, since "the device might not be quiesced yet and continue delivering data" (`:532-543`); it keeps waiting. Fuchsia's `I8042Device::Identify()` (`src/ui/input/drivers/pc-ps2/device.cc:171-226`) fails on such a byte, as this driver does.

**Acceptance**
A byte that is no answer, placed ahead of SCAN_DISABLE's ACK by a temporary change (the controller's command 0xD2 loads a byte into its output buffer as if the keyboard had sent it), leaves the reset succeeding: rel logs `pc-ps2: ready`, and `python3 .agents/skills/ours-verify/scripts/boot_check.py` passes with the change reverted.

## 2. The controller's waits are bounded by a count of status reads, not by time

**Severity**
Low: under QEMU every answer is there at the first read, and on hardware the bound comes to about 100 ms, more than an i8042 or a keyboard takes. It is wrong only where a status read is much faster than the microsecond a legacy port usually takes, or a device much slower than usual.

**Symptom**
Latent, cannot be triggered today. On a machine whose port reads take 100 ns, `MAX_POLLS` (100,000 reads, `system/driver/pc-ps2/i8042.cpp`) gives a keyboard 10 ms to answer `IDENTIFY` or `SCAN_ENABLE`; a slow one that misses it fails the reset with `pc-ps2: identifying the keyboard: TimeOut`, and the driver exits 1 with no keyboard.

**Cause**
A wait for the controller is a wait for a bit in the status port, and its bound should be a time: the reference sleeps 10 µs between reads, 500 times, 5 ms in all. No program here can do that yet. `clock_get_monotonic` reads the clock, but `port_wait` and `object_wait_one` take only 0 or `INSTANT_INFINITE` as a deadline, and there is no sleep. So `wait_write()` and `wait_read()` count status reads instead, and how long the count lasts depends on how fast the machine answers an `in`.

**Change**
Once a program can wait until a deadline, the waits sleep a short interval between reads up to a deadline taken from the clock, as the reference does, and `MAX_POLLS` goes. Ruled out: a calibrated busy loop, which measures nothing that holds on the next machine; reading the TSC from ring 3, whose frequency a program cannot learn.

**Reference**
Fuchsia `i8042::Controller::WaitWrite()` and `WaitRead()` (`src/ui/input/drivers/pc-ps2/controller.cc:281-297`), which poll with `zx::nanosleep(zx::deadline_after(kStatusPollDelay))`, 10 µs, at most `kStatusPollTimeout`, 500, times (`:28-31`).

**Acceptance**
`MAX_POLLS` is gone from `i8042.cpp`, and its waits take a deadline from the clock; `python3 .agents/skills/ours-verify/scripts/boot_check.py` passes, with `pc-ps2: key a` and `pc-ps2: key A` on rel.

## 3. The Pause key logs as left-control and num-lock

**Severity**
Low: one key reports two wrong names in the debug log; nothing consumes keys yet.

**Symptom**
Pressing Pause, or `sendkey pause` in QEMU's monitor, logs `pc-ps2: key left-control` and then `pc-ps2: key num-lock`.

**Cause**
Pause has no break code and no 0xE0 prefix: it sends the six bytes 0xE1 0x1D 0x45 0xE1 0x9D 0xC5 at once. `Decoder::feed()` (`system/driver/pc-ps2/keymap.cpp`) knows only the 0xE0 prefix, so it reads 0xE1 as the break code of 0x61, which presses nothing, then 0x1D as the left Ctrl key going down and 0x45 as Num Lock going down. The kernel's table it was moved from had a `KEY_PAUSE` and never decoded it either.

**Change**
The decoder treats 0xE1 as a prefix of its own: it drops the byte after it and reads the next as Pause, a make code or a break code, so 0xE1 0x1D 0x45 presses one key, `NamedKey::Pause`, named "pause", and 0xE1 0x9D 0xC5 releases it. The prefix state joins `Decoder::State` beside `Extended`. Ruled out: a table entry for 0x61, which would report Pause and still log the Ctrl and Num Lock bytes after it.

**Reference**
Linux `atkbd_receive_byte()` (`drivers/input/keyboard/atkbd.c:436`), which on 0xE1, `ATKBD_RET_EMUL1`, sets `emul = 2` (`:474-475`), so that the byte after the prefix is dropped and the next decoded as one emulated key. Fuchsia's `I8042Device::ProcessScancode()` (`src/ui/input/drivers/pc-ps2/device.cc:384-418`) knows only 0xE0, as this one does.

**Acceptance**
With `sendkey pause` added to rel's keys in `boot_check.py`, rel logs `pc-ps2: key pause` and no `left-control` or `num-lock` line between it and the next key.

## 4. Three programs carry their own client of an ours.resource Get

**Severity**
Low: the three copies agree today; a fix to one has to be made to all three.

**Symptom**
None at run time. `get_debug_resource()` in `examples/shell/shell.cpp`, `get_resource()` in `system/service/init/test/routing_checks/routing_checks.cpp` and `get()` in `system/driver/pc-ps2/pc_ps2.cpp` each connect to `/svc/<protocol>`, write Get, wait for the reply, tell an epitaph from a reply, and close a handle a malformed reply carried, each in about sixty lines.

**Cause**
`system/lib/ours.resource` is a codec of bytes and handle values, with no system call, so that its tests run on the host; the calls that send the bytes have no home, and each client wrote them. Step 13 of `.agents/tasks/microkernel-interrupts/PLAN.md` made one server class for the three protocols for the same reason this item exists: three copies of one exchange, where every later fix, to the epitaph handling say, would have to be made three times.

**Change**
One client, `ours_resource::get(path, protocol)` in a user-toolchain target beside the codec, as `//system/lib/fsio` keeps its system calls apart from the host-built namespace code; the shell, routing_checks and pc-ps2 call it. Ruled out: putting it in fsio, whose calls speak ours.io, not ours.resource.

**Reference**
Fuchsia components reach `fuchsia.kernel.IrqResource` through `component::Connect<fuchsia_kernel::IrqResource>()` and the FIDL-generated synchronous client's `Get()`, one implementation for every caller.

**Acceptance**
`git grep -n "decode_get_reply" -- examples system/driver system/service` finds only the shared client, and `boot_check.py` passes on both images.
