# pc-ps2 changes

## Title: a user-space driver reads the PS/2 keyboard, and the kernel's keyboard driver goes (2026-10-05)

**Files**
- `system/driver/pc-ps2/pc_ps2.cpp` — the driver: both resources through its /svc, the i8042's two ports, ISA 1 with the check of what hands the line back, a port bound to the interrupt, and the loop that reads the keyboard on each packet and acks
- `system/driver/pc-ps2/i8042.hpp`, `i8042.cpp` — the controller: port I/O, commands with bounded waits, the reset and the keyboard's identity, and `read_byte()`
- `system/driver/pc-ps2/keymap.hpp`, `keymap.cpp` — scancode set 1, moved from the kernel's tables: `Key`, `NamedKey`, and `Decoder`, which tracks the shift keys and the 0xE0 prefix
- `system/driver/pc-ps2/BUILD.gn` — the program `pc-ps2`, installed as `bin/pc-ps2`, and its manifest `pc-ps2.cm`, which uses `ours.resource.Irq` and `ours.resource.Ioport`
- `kernel/products/image/dev/BUILD.gn` — root.cm starts `pc-ps2` as its fourth eager child and offers it the two protocols; the comment over root.cm no longer says routing_checks alone is offered them
- `system/service/init/test/routing_checks/BUILD.gn` — the comment over routing_checks.cm says the same: root.cm offers the two protocols to it and to pc-ps2
- `kernel/platform/pc/keyboard.cpp` — deleted: the kernel's i8042 driver, its controller setup, its interrupt handler, its scancode tables and `KEY_BUF`, the ring nothing read
- `kernel/platform/pc/BUILD.gn` — without keyboard.cpp
- `kernel/platform/include/ours/platform/init.hpp` — the declaration of `platform_init_keyboard()` goes; keyboard.cpp defined it `static`, so it named nothing anyone could call
- `kernel/platform/pc/ioport.cpp` — 0x60 and 0x64 off the deny table, and the comment says why the reboot's write to 0x64 does not keep them there
- `kernel/platform/pc/interrupt.cpp` — the comment that names the kernel drivers finding the line table: the PIT and the HPET, no more the keyboard
- `.agents/skills/ours-verify/scripts/boot_check.py` — on dev, once `pc-ps2: ready` appears, it types `sendkey a` and `sendkey shift-a` into a QEMU monitor of its own, on a socket pair whose far end QEMU inherits, keeps what that monitor answered in `rel.monitor.log`, and requires `pc-ps2: ready`, `pc-ps2: key a` and `pc-ps2: key A`
- `.agents/skills/ours-verify/SKILL.md` — says so, and where to look when a `pc-ps2:` line is missing
- `docs/repairs/261005/pc-ps2.md` — what the step leaves open: the waits counted in reads, the Pause key, and three copies of the Get client
- `docs/repairs/261004/kernel-irq.md` — item 2's example of a handler that takes a second spin lock no longer names the kernel's keyboard handler
- `system/README.md` — `system/driver`
- `.agents/tasks/microkernel-interrupts/PLAN.md` — step 12 and the test plan say the key check is scripted, and the files list names every file this step changed

**Symptom**
None; this is step 12, the last, of `.agents/tasks/microkernel-interrupts/PLAN.md`. Before it the kernel drove the keyboard itself: `kernel/platform/pc/keyboard.cpp` set the i8042 up at `InitLevel::Irq + 2`, held ISA 1 with an observer of its own, and decoded every scancode in interrupt context into `KEY_BUF`, a ring nothing read (DESIGN.md, defect 11). Because the kernel held the line, `interrupt_create` on ISA 1 answered AlreadyBound; because it drove the controller, step 9 put 0x60 and 0x64 on the deny table. And no device's interrupt had yet reached an interrupt object: step 11's checks raised a line by IPI or by a dispatch from a thread (`interruptline.test`), never from a device through the IO-APIC.

**Cause**
The objects:
- the i8042: two I/O ports, the data port 0x60 and the command and status port 0x64. Its status says whether a byte waits for the driver (OBF, bit 0) and whether it has taken the last byte written to it (IBF, bit 1). Its configuration byte turns each port's interrupt and clock on or off, and translation, which hands the keyboard's set 2 on as set 1. It holds one byte for the driver at a time, and raises ISA 1 when a byte from its first port arrives with that port's interrupt on;
- ISA 1: GSI 1 on q35, whose MADT has no override for it, edge-triggered and active high as an ISA line is by default, on vector 33 (step 7);
- the driver's interrupt object: an `InterruptEventDispatcher` on GSI 1 (step 11), made in the default mode, which reads the line back as edge and so sets no MASK_POSTWAIT. It is IDLE until it sends its one packet, embedded in it, and NEEDACK until the ack;
- the port, made with BindToInterrupt (step 4), whose interrupt queue is served first;
- the driver's I/O bitmap, which holds 0x60 and 0x64 (step 10);
- the two resources, duplicates of the IRQ root and the I/O port root from the root's built-ins (step 13);
- the decoder: which shift keys are held, and whether the last byte was the 0xE0 prefix.

The events of one key, typed through QEMU's monitor with `sendkey a`:
1. QEMU's keyboard sends the make code of A; with translation on, the controller holds set 1's 0x1E, sets OBF and raises ISA 1.
2. The IO-APIC delivers vector 33. The irq layer runs the object's handler under the line's lock L1; the object is IDLE, so it queues its packet on the port, posts the port's semaphore and goes to NEEDACK. The LAPIC EOI follows the handler.
3. The driver's `port_wait` returns the Interrupt packet. It reads the status, sees OBF and reads 0x1E; the decoder finds `a` in `SET1`, and the driver logs `pc-ps2: key a`. The next status read shows no byte.
4. The driver acks; no interrupt came meanwhile, so the object goes back to IDLE.
5. 100 ms later QEMU releases the key: 0x9E takes the same path, and the decoder, seeing a break code, reports nothing.

`sendkey shift-a` is 0x2A, the left shift going down, which is logged and held by the decoder; 0x1E, read from `SET1_SHIFTED` as `A`; then 0x9E and 0xAA, the two releases.

Why each piece is shaped the way it is:
- **Why the kernel's driver goes whole (D3 b).** The controller holds one byte for whoever reads 0x60 first. With the kernel's handler still on ISA 1, every key would be a race between it and the driver, and that handler, which reads until OBF clears, would also take the answers to the driver's reset commands. D3 put the deletion after a working replacement, which this step is. The kernel keeps only what reboot needs, the one command 0xFE to 0x64 in `platform_reboot()`, which `power.cpp` already wrote with constants of its own. Nothing else referred to keyboard.cpp but a declaration of `platform_init_keyboard()` in init.hpp, a function keyboard.cpp kept `static`.
- **Why 0x60 and 0x64 come off the deny table though the reboot still writes 0x64.** The table lists ports a kernel driver keeps state behind, where a second user would change that state under it (step 9). The reboot keeps no state in the i8042: it writes one command as the machine goes down, after the chipset's reset register has failed to reset it, and takes nothing from the driver. What it does show is that granting 0x64 grants reset with it, since the controller that reads the keyboard is also wired to the CPU's reset line. 0xCF9 stays denied because no driver needs it; 0x64 cannot be denied without denying the keyboard.
- **Why the default mode, and so no mask until the ack (D2 a).** ISA 1 is edge-triggered. An edge line is not masked between a packet and its ack: an interrupt that comes meanwhile is recorded as pending, and the ack sends the next packet at once. Masking it would lose those edges, since nothing replays an edge that came while the line was masked (D2 b).
- **Why the driver reads until OBF clears, then acks.** The ack re-arms the object, and a byte not read by then must raise the line again or wait unread. Every byte the controller loads raises the line, so a byte that lands after the last status read raises it while the object is in NEEDACK, and the ack sends a packet for it at once. Reading everything there before the ack spares a packet per byte, and it is the order a level line needs, where the ack unmasks and the device must be quiet first.
- **Why it reads once more before `pc-ps2: ready`.** The reset turns the first port's interrupt on before the driver holds the line, so a key pressed in between raises ISA 1 while the line is masked, and its edge is gone. The byte stays in the controller; if the controller raises the line only when a byte arrives, which DESIGN.md's open questions infer without having checked it, the keyboard would never interrupt again. So once the line is held and bound, the driver reads what is there; a byte that comes during that read sends a packet. The kernel's driver made the same read after its unmask (`i8042_interrupt(0, &KEYBOARD)`).
- **Why the driver checks the line's lifecycle first.** Closing an object's last handle destroys it, which detaches it from the line and masks the line: that is what hands a dead driver's line back. Step 11 checked it on ISA 3 from the dev.dbg_1 shell. The driver checks it on the line it is about to take, on dev, where the line belongs to a component, and fails the boot check with a FAILED line if a second create does not answer AlreadyBound or the third does not answer Ok.
- **Why the reset and the identity follow the reference.** Both ports go off and the controller is flushed, so that no device's byte is taken for an answer; the configuration loses both interrupts and translation while the controller and the first port test themselves; then translation and the first port's interrupt come on, and the second port stays off, as nothing reads a mouse. Last, the keyboard is asked who it is, with its keys off while it answers, and its keys are turned on, which is what makes it send any.
- **Why the resources are closed once used.** The ports stay open in the I/O bitmap and the object holds the line without them, while each Get's duplicate counts against the root's handle quota until the client closes it (`docs/repairs/261004/init.md`, item 4).
- **Why the waits count status reads.** A wait for the controller is a wait for a bit, and its bound should be a time. No program can read a clock or wait until a deadline yet, so the bound is 100,000 reads, about 100 ms at the microsecond a legacy port takes on hardware; under QEMU the answer is there at the first read (repair item 1).
- **Why the decoder's state is one word.** The left shift, the right shift and the prefix are three yes/no facts about one decoder, all written by its one thread, so they are one `State` word, as the repo's style asks.
- **What changed in the tables on their way out of the kernel.** They keep their content, now typed as `Key`: a character below 0x80, a `NamedKey` from 0x80 on, the numbering the kernel's `enum ExtendedKeys` had. The extended table gains Delete at 0xE0 0x53, which the kernel's lacked beside Insert at 0x52, and which the reference's has. Named keys no table used, F13 to F20, Pause and the keypad's multiply, stayed behind. The extended table is set by number, since most of its numbers press nothing; the two full tables are written eight codes to a row.

**Change**
The kernel, before (keyboard.cpp, now deleted):

```cpp
auto const irqnum = apic_io_isa_to_global(ISA_IRQ_KEYBOARD);
auto const attached = irq::attach(irqnum, KEYBOARD_OBSERVER);
...
auto const unmasked = irq::unmask(irqnum);
i8042_interrupt(0, &KEYBOARD);
```

and the deny table (ioport.cpp), before:

```cpp
IoPortRange{.base = 0x40, .count = 4},
IoPortRange{.base = 0x60, .count = 1},
IoPortRange{.base = 0x64, .count = 1},
IoPortRange{.base = 0xA0, .count = 2},
```

and after, without the two, and a comment above the table on why.

The driver, the order of its calls (pc_ps2.cpp):

```cpp
auto const ioport = get_resource(IOPORT_RESOURCE, ours_resource::IOPORT_PROTOCOL);
open_ports(ioport.unwrap());                 // ioports_request(0x64, 1), then (0x60, 1)
pc_ps2::i8042::reset();                      // the controller, then the keyboard's identity
auto const irq = get_resource(IRQ_RESOURCE, ours_resource::IRQ_PROTOCOL);
auto const interrupt = take_line(irq.unwrap()); // ISA 1, RemapIrq, after the lifecycle check
ours::port_create(ours::PortCreateOptions::BindToInterrupt, &port);
ours::interrupt_bind(interrupt.unwrap(), port, KEYBOARD_KEY, ours::InterruptBindOptions::Bind);
read_keys(decoder);                          // what came before the line was held
// then for each packet:
ours::port_wait(port, ours::INSTANT_INFINITE, &packet);
read_keys(decoder);                          // until OBF clears
ours::interrupt_ack(interrupt);
```

root.cm gains the child and the offer:

```text
{
  name = "pc-ps2"
  manifest = "//system/driver/pc-ps2:pc-ps2.cm"
  startup = "eager"
}
...
protocol = [ "ours.resource.Irq", "ours.resource.Ioport" ]
from = "parent"
to = [ "#routing_checks", "#pc-ps2" ]
```

boot_check, on dev: a monitor of the keys' own when QEMU starts, and the keys when `pc-ps2: ready` comes in:

```python
self.monitor, far = socket.socketpair()      # QEMU inherits far (pass_fds)
qemu_args = ["-chardev", f"socket,id=keys,fd={far.fileno()}", "-monitor", "chardev:keys"]
...
commands = "".join(f"sendkey {key}\n" for key in self.keys[1]).encode()
self.monitor.sendall(commands)
```

Rejected:
- Taking the line before the controller's interrupt comes on, as Linux orders it, instead of the read before `pc-ps2: ready`: it would split the reset around `interrupt_create`, and the read covers the same window with one call.
- Keeping 0x64 on the deny table for the reboot's sake: the driver cannot run without it, and the reboot keeps no state there.
- Logging releases too: the plan names one line per key pressed, `pc-ps2: key a`.
- Typing the keys at a fixed time into the boot instead of on `pc-ps2: ready`: a key typed while the reset runs would be taken for a command's answer, or lost.
- Typing them into the monitor that `-serial mon:stdio` puts behind Ctrl-A c, as this step first did: that monitor shares stdout with COM1, and its line editor echoes each byte typed as a write of its own, while the guest prints a line one write per character. A guest line printed while the commands are echoed can come out with the echo inside it, and the first key's own line is printed about then: a required line split that way fails a good boot, and a FAILED or error line split that way lets a bad one pass (found at review).

**Reference**
- Fuchsia pc-ps2 at `51f05104c13`: `i8042::Controller::DdkInit()` (`src/ui/input/drivers/pc-ps2/controller.cc:47-185`) requests 0x64 and 0x60 with `zx_ioports_request()` (`:57-68`), disables both ports and flushes (`:70-83`), reads the control byte and writes it back with `kbdint`, `auxint` and `xlate` cleared (`:85-107`), expects 0x55 from the self-test (`:109-120`) and 0x00 from port 1's test (`:122-133`), and turns translation and port 1 with its interrupt back on (`:151-164`); `SendControllerCommand()` and `SendDeviceCommand()` (`:197-264`); `WaitWrite()`, `WaitRead()` and `Flush()` (`:281-306`). `I8042Device::Bind()` (`device.cc:124-159`) creates the interrupt with `ZX_INTERRUPT_REMAP_IRQ` alone (`:140-141`); `I8042Device::Identify()` (`:171-226`); `I8042Device::HandleIrq()` (`:353-377`) reads while OBF is set and then calls `irq_.ack()`; `I8042Device::ProcessScancode()` (`:384-418`) decodes with `kSet1UsageMap` and `kSet1ExtendedUsageMap` (`keymap.h`), whose extended map has Delete at 0x53 (`:194-197`).
- Zircon: `pc_keyboard_reboot()` (`zircon/kernel/platform/pc/keyboard.rs:101-111`), all that is left of its kernel's i8042 driver, "Remnants of a full i8042 keyboard driver, now just used to reboot the system as a fallback" (`:9`), called from `reboot()` (`zircon/kernel/platform/pc/power.cc:43-53`). Its I/O port deny list holds the debug UART's ports alone (`zircon/kernel/platform/debug.cc:260-263`), so the i8042's are grantable there as here. `InterruptDispatcher::InterruptHandler()` (`zircon/kernel/object/interrupt_dispatcher.cc:204`) and `InterruptDispatcher::Ack()` (`:371`): the NEEDACK protocol the driver's loop follows.
- Linux: `i8042_setup_kbd()` (`drivers/input/serio/i8042.c:1487-1505`) requests the keyboard's IRQ before `i8042_enable_kbd_port()` (`:617-630`) sets `I8042_CTR_KBDINT`, so the handler is in place before the controller can raise the line.
- Design: `.agents/tasks/microkernel-interrupts/DESIGN.md`, D2, D3 and D8, and "Edge lines: the PS/2 case".

**Verification**
- `boot_check.py`: dbg PASS (34 passed, 2 skipped, 0 failed, 0 error lines), rel PASS (30 passed, 10 skipped, 0 failed, 6/6 required, 0 error lines): step 13's counts, with three more required lines. rel's log holds `init: started pc-ps2`, `pc-ps2: ready`, QEMU's monitor taking `sendkey a` and `sendkey shift-a`, then `pc-ps2: key a`, `pc-ps2: key left-shift` and `pc-ps2: key A`: a device's interrupt, through the IO-APIC, the irq layer, an interrupt object and a port, to a user-space driver.
- Kernel tests on dev.dbg_1, in one boot: `resource.test: ok -- 32 checks`, which now counts 5 of the kernel's port ranges (7 before); `irq.test: ok -- 33 checks`; `interruptline.test: ok -- 37 checks`.
- Mutation checks, rel only, each reverted:
  - 0x60 and 0x64 back on the deny table: `pc-ps2: ioports_request: AccessDenied`, `init: pc-ps2 exited with 1`, and FAIL: no "pc-ps2: ready", "pc-ps2: key a", "pc-ps2: key A" within 30s, 3/6 required;
  - the driver never acks: `pc-ps2: key a` and nothing after it, FAIL: no "pc-ps2: key A" within 30s, 5/6 required. The release of `a` came while the object waited for its ack, so it only folded into the pending interrupt, and its byte stayed unread in the controller, which then had no room for the next.
- The decoder, built on the host with a scratch harness that is not committed: 0x1E 0x9E gives `a`; shift-a gives `left-shift A`; the right shift and 1 give `right-shift !`; 0xE0 with 0x48, 0x53 and 0x1D gives `up delete right-control`; 0xE0 0x2A 0xE0 0x37 0xE0 0xB7 0xE0 0xAA gives `print-screen` alone, and an `a` after it is lower case; Pause's 0xE1 0x1D 0x45 0xE1 0x9D 0xC5 gives `left-control num-lock` (repair item 2).
- `llvm-nm` on the dbg kernel: no symbol with keyboard or i8042 in its name, `platform_reboot()` and `kernel_ioports()` still there. The driver's three tables are 128 bytes each in `.rodata`, and it has no `.init_array`.
- clang-format clean on every changed line; clang-tidy reports nothing on `pc_ps2.cpp`, `i8042.cpp`, `keymap.cpp` and `ioport.cpp`, and on `interrupt.cpp` only naming warnings in `apic.hpp` and `common.hpp` that predate this change.
- Boots: two full boot checks of both images, one kshell boot of dev.dbg_1, and two rel boots for the mutations.
- After the review moved the keys to a monitor of their own (Rejected, last item): dbg PASS (34 passed, 2 skipped, the shell done at 42.1s), rel PASS (30 passed, 10 skipped, 6/6 required). rel's log holds nothing of the monitor, and `pc-ps2: key a` is the line after `pc-ps2: ready`; `rel.monitor.log` holds the monitor's banner and both commands. With the second key misspelt as `shift-nosuchkey`, a temporary mutation, rel logs `pc-ps2: key a` alone and fails, and `rel.monitor.log` says `invalid parameter: nosuchkey`.
- Not tested: a key pressed before the driver holds the line, which the read before `pc-ps2: ready` is for, since the keys are typed after it; real hardware.

**Deliberately not done**
- A consumer for the keys: they go to the debug log alone. An input protocol, and whatever reads it, is later work; wiring PS/2 into the console is what D3 recommends against.
- The second port, where a mouse would be: left off.
- A deadline for the controller's waits, which needs a clock programs can read (repair item 1); the Pause key (item 2); one Get client for the three programs that carry one (item 3).
- A host unit test of the decoder: the boot check types an unshifted and a shifted key on every boot, and the scratch harness above was a one-off.
- An exclusive sub-resource for the driver (D5 b): it holds the whole IRQ and I/O port roots for as long as it takes its line and ports, then closes them.

**How it was found**
None, a planned step. The two mutation checks under Verification were run to show that the deny table and the ack are each what the result rests on. The review read rel's log, where the monitor's echo of both commands sat between `pc-ps2: ready` and `pc-ps2: key a`, one byte at a time, and found that the guest's lines and the echo share one stream with nothing keeping a line whole (Rejected, last item). It also found two manifest comments, over root.cm and routing_checks.cm, still saying that routing_checks alone is offered the two resource protocols (Files).

**The user's prediction**
None.

**Three questions you should be able to answer**
- The driver acks only after reading every byte the controller holds. If it acked first and read after, what would happen to a byte that lands between the ack and the read, and to the packet it raises? Is either order wrong on this edge line, and which order does a level line need, and why?
- The reset turns the keyboard's interrupt on before the driver holds ISA 1. What happens to a key pressed in that window without the read before `pc-ps2: ready`, and how would the same calls in another order close the window instead?
- 0xCF9 stays on the deny table while 0x64 comes off, though either can reset the machine. What makes the two cases different, and what can a holder of the I/O port root do now that it could not before this step?
