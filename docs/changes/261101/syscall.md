# syscall changes

## `interrupt_map` and `interrupt_create` answer AccessDenied for a line the kernel holds for its own use (2026-11-01)

**Files**
- `kernel/include/ours/irq/mod.hpp`, `kernel/kernel/irq/mod.cpp`, `kernel/include/ours/irq/desc_table.hpp`, `kernel/kernel/irq/desc_table.cpp`, `kernel/include/ours/irq/desc.hpp` — `irq::is_kernel_owned()`, down to a public read of the line's Permanent flag.
- `kernel/lib/syscall/src/interrupt.cpp` — `interrupt_map` refuses a pin whose line the kernel holds.
- `kernel/object/interrupt_event_dispatcher.cpp`, `kernel/object/include/ours/object/interrupt_event_dispatcher.hpp` — `InterruptEventDispatcher::create()` refuses it before touching the line; the header's comment lists the status codes in the order they are checked.
- `system/public/ours/syscalls.gni` — the two entries say so.
- `kernel/commands/interrupt_line_test.cpp`, `system/service/init/test/routing_checks/routing_checks.cpp`, `system/app/shell/shell_tests.cpp` — the three places that stated the old answers.
- `.agents/tasks/driver-capabilities/RESULT.md`, `.agents/skills/ours-kernel-design/references/task.md`, `.agents/tasks/irq-multi-arch/{DESIGN,PLAN}.md` — the hand-over to zcode, the rule, and the plan.

**Symptom**
Latent. A program holding the interrupt root could not tell the three ways a pin may be unavailable apart. `interrupt_map` answered Ok for the timer's pin, and `interrupt_create` on that line answered AlreadyBound, the same code as for a line another program's interrupt holds, which may well be free later. The ACPI route service, and on an arm or riscv platform the device-tree one, will name the kernel's own lines (the timer, a console UART, an SMMU) from the firmware's tables, and a driver manager that asks about them has to know they are never available.

**Cause**
Not a mistake: a distinction the kernel had and the interface did not show. The kernel holds its own lines with an action that says Permanent, which the line layer refuses to share and never lets go (`Desc::attach()` answers AlreadyBound for it); a line an interrupt object holds is an ordinary exclusive one, freed when the object is destroyed. Both reach `interrupt_create` as AlreadyBound, because the layer below answers for the line and not for who holds it. And `interrupt_map` only looked the number up, so it said Ok for any line made, the kernel's included, leaving the driver to find out later and from a different call.

**Change**
One query and two refusals. The query reads the line's Permanent flag:

```cpp
auto DescTable::is_kernel_owned(VIrqNum const virqnum) -> bool {
    auto const *const desc = irqnum_to_desc(virqnum);
    return desc != nullptr && desc->is_kernel_owned();
}
```

and the system call layer turns it into the answer, in `interrupt_map` after the line is found and before the number is written, and in `InterruptEventDispatcher::create()` before anything is allocated:

```cpp
if (irq::is_kernel_owned(virq)) { return Status::AccessDenied; }
```

A line with no pin is still NoResource (it is not owned), a line another program holds is still AlreadyBound, and `kernel/irq` itself still answers AlreadyBound for a second action on a Permanent line, so `irq.test` is unchanged. The query takes no lock, and the check is advisory: `attach()` stays the authority, and it answers AlreadyBound for a Permanent line whatever the caller asked first. What the order of the two does guarantee is that a "yes" is final, since a line is Permanent for good once it is (it is never detached, and an interrupt object's action is Oneshot, so no program can make one), while a "no" may be stale by the time of the attach, if a kernel driver takes the line in between; the attach then answers AlreadyBound, which is the answer this change did not need to improve, because today a kernel driver takes its lines at boot, before any program runs. A kernel driver that takes a line for its own therefore uses a Permanent action and keeps programs off it with no further work; that is written into `task.md`, with the limits of such an action (not Oneshot, not Shared, never detached).

AccessDenied was chosen for both calls by the assistant, the user having left D4b to it. `interrupt_map` already used AccessDenied for "the resource is not the root", so for a caller without the root the two meanings are one answer; the caller this is for holds the root, and for it AccessDenied after the root is accepted can only mean the kernel's line, and a caller without the root learns nothing about the line. Rejected: a new status code (a change to the status table and every generated table for a distinction one caller needs, and `AccessDenied` is what the word means); leaving `interrupt_map` as it was (the route service would learn it only by trying `interrupt_create`, which makes it take a line to find out); NotSupported or NoResource (NoResource is already "no such pin", which the driver manager must tell apart from this).

**Reference**
The marker (a Permanent action) is this project's own earlier design, `.agents/tasks/microkernel-interrupts/DESIGN.md:214`. The answer departs from what that document records for Zircon: there a user's attach on a kernel line answers AlreadyBound (`interrupt_manager.rs:294-296`, as that document cites it, not re-read on 2026-11-01), here `interrupt_map` and `interrupt_create` answer AccessDenied. The departure is deliberate and is the assistant's, by the user's delegation; the user may reopen it. A note at that line and at `PLAN.md:149` points here.

**Verification**
- `boot_check.py`: dbg PASS, 45 passed and 4 skipped; rel PASS, 42 passed and 8 skipped, 6 of 6 required; host tests 154 passed. The same numbers as before the change.
- `interruptline.test`: 63 checks, all ok (59 before). The timer's line refuses an object, with a mode or without, with AccessDenied and keeps its own mode; `irq::is_kernel_owned()` is true for it, false for the free line and for `INVALID_V_IRQ_NUM`, and counting it over every hardware number of the machine finds exactly one line that is. The other kernel-shell tests of the layer unchanged and ok: `irqchip.test` 18, `irq.test` 72, `pinlines.test` 28, `irqdomain.test` 39, `interrupt.test` 58.
- `routing_checks` (the rel image's required line) and the user shell's checks, both moved: `interrupt_map` of GSI 2 answers AccessDenied and writes nothing (the shell test starts the out value at a marker and checks it is still there); the timer's line is found as the one before GSI 3's (the kernel numbers lines consecutively in the order of their pins) and `interrupt_create` on it answers AccessDenied, with a mode or without; the route service's GSI for ISA 0, which is GSI 2, is refused by `interrupt_map` and every other ISA GSI is Ok; GSI 3 is still free and a second create on it is still AlreadyBound. The order of the answers is pinned in the user shell: a handle that is not a handle answers BadHandle and one that is not the root BadType, for `interrupt_map` and for `interrupt_create` (which also checks its options, InvArgs, before the line), each asked about the timer's own pin, so none of them is turned into AccessDenied.
- clang-tidy on the changed sources: no finding on a changed line (the findings it prints are in headers and lines this change did not touch). Four mutations, each restored byte for byte, each killed: `interrupt_map` not refusing (the rel image fails its required line); `interrupt_create` not refusing (`interruptline.test` fails 5 checks); every line counted as the kernel's (26); a line a program holds counted as the kernel's, by also reading the Oneshot flag (13). The checks added after the independent review (the count, the order of the answers) were not mutation-tested.
- Not verified: arm and riscv, where the kernel will hold lines of its own (a console UART, a timer frame) and must hold them with Permanent actions to be refused; the tests find the timer's line from GSI 3's, which holds on this machine and not in general.

**Deliberately not done**
- No other line is marked: which lines a platform's kernel holds is that platform's code, and today only the legacy timer's is.
- `kernel/irq` answers as it did and gains one query, `is_kernel_owned()`; no new status code.
- Resource narrowing (an Irq resource cut to one line) is a later step of the interrupt-system plan.

**How it was found**
By the design of the interrupt system for riscv, arm and GPIO (`.agents/tasks/irq-multi-arch/DESIGN.md`, 2026-11-01): the route service of those platforms will name the kernel's own lines, and the review of the design noted that `interrupt_map` answered Ok for them and `interrupt_create` AlreadyBound.

**The user's prediction**
None. The user left the choice to the assistant ("你决定吧", 2026-11-01).

**Three questions you should be able to answer**
- Why does the check go in the system call layer and not in `kernel/irq`, whose second action on a Permanent line is AlreadyBound?
- Why is asking whether a line is the kernel's before the attach safe even though a "no" can go stale, and which of the two calls decides?
- Why is AccessDenied from `interrupt_map` unambiguous for the route service but not for a caller without the interrupt root?
