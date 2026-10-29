# riscv-boot-chain: the phys boot chain on QEMU virt under OpenSBI, up to the fake kernel

**Date**: 2026-10-28
**Status**: Accepted

## Request
Port the boot chain to riscv (the user, 2026-10-26: "新开一个worktree，尝试去引入riscv"), split into pieces that run in parallel (2026-10-28: "拆解任务，并行化处理"). The scope ends where the fake kernel, started by physboot under OpenSBI on QEMU's `virt` machine, prints `[fake-kernel] handoff ok`. vmours on riscv is out of scope.

## Decisions
| Decision | Options | Recommendation | User's prediction | Decided |
|---|---|---|---|---|
| Layering | own minimal shim; Zircon's shim → physload → physboot | Zircon's layering, Linux boot-image header | None | the user, 2026-10-26 |
| Shim load address and console | fixed 0x80200000 + legacy SBI putchar; relocatable + DBCN | fixed + legacy putchar | None | the user, 2026-10-26 |
| Device tree's path to the kernel | mirror ACPI (address in Handoff, kernel walks it); convert to OBI items | mirror ACPI | None | the user, 2026-10-27 |
| FDT reader | hand-written, host-tested; vendor libfdt | hand-written | None | the user, 2026-10-27 |
| Page-table mode | Sv39; Sv48 | Sv39, as Zircon's `kernel_aspace.h` | None | the user, 2026-10-28 |
| physload's console | SBI legacy putchar; ns16550 MMIO found through `/chosen/stdout-path` | SBI putchar now | None | the user, 2026-10-28 |
| How the pieces run in parallel | separate sessions; a Workflow of subagents | separate sessions | None | the user, 2026-10-28: a Workflow (ultracode on) |
| Branch base | the old base; rebase onto `baseline/20260824` | rebase, to build Sv39 on the current paging engine | None | rebased 2026-10-28 (backup `feat/riscv-pre-rebase`), reported to the user |
| What the kernel may assume of the firmware | the SBI specification and the device tree only; OpenSBI's behaviour too | the specification only, the legacy console as the one optional call, a second implementation's boot as a repair item | None | the user, 2026-10-28: "只依赖SBI，不依赖OPENSBI" |

## Steps
0. `Handoff::device_tree` and `LegacyPlatformInfo::device_tree`. Landed (`01def0f9`).
0.5. The riscv phys headers `handoff.hpp` includes. Landed (`29c1e573`).
0.6. riscv `arch::boot_marker`. Landed (`fdddb62b`).
1. Lane `dtb` (`feat/riscv-dtb`): device tree → memory map, a host-tested conversion in `kernel/phys/lib/bootshim`, used by the riscv shim to print RAM, reserved ranges and the initrd, and to fill `LegacyPlatformInfo`. Landed (`5dd231af`, `da7538e8`).
2. Lane `paging` (`feat/riscv-paging`): Sv39 traits for the generic paging engine, with a host smoke test. Landed (`97e1e6c4`, `bbbbc898`).
3. Lane `phys` (`feat/riscv-phys`, starts from lane `paging`): the riscv phys arch layer, so physload and physboot build for riscv. Landed (`d9e591ac` to `dad20290`, eight commits).
4. Lane `fake` (`feat/riscv-fake`): the fake kernel's riscv entry and console, and a riscv boot check in `ours-verify`. Landed (`acec7d8d`, `10f8d4e9`).

Lanes 1 to 4 ran as one Workflow on 2026-10-28 (15 agents): each lane's implementer, then an acceptance-criteria verifier that built and booted, and a diff reviewer, in parallel; lane `phys` needed one fix round for two blocking review findings, the others passed the first round. Merged into `feat/riscv` with no conflict. On the merged tree: x86 `ours-verify` PASS (152 host tests); riscv shim, physload, physboot module and fake-kernel ELF build; the riscv boot check reaches `shim` and fails as expected, since the shim does not load physload yet. Each lane's choices that belong to the user are listed in its change record and were reported to the user.
5. Integration on `feat/riscv`: lanes cherry-picked in order, then the shim loads physload out of the OBI in the initrd, and the fake image boots.

Each lane commits on its own branch, one change per commit, each passing `ours-verify` on x86.

## Files
- Lane `dtb`: `kernel/phys/lib/bootshim/**`, `kernel/arch/riscv/phys/bootshim/**`, `kernel/arch/riscv/phys/BUILD.gn`, `docs/changes/261028/riscv-boot.md`, `docs/changes/261028/phys-bootshim.md`
- Lane `paging`: `kernel/arch/riscv/lib/arch/include/arch/paging/**`, `kernel/lib/arch/test/riscv_paging_smoke.cpp`, `kernel/lib/arch/BUILD.gn`, `kernel/arch/riscv/BUILD.gn`, root `BUILD.gn`, `docs/changes/261028/riscv-paging.md`
- Lane `phys`: `kernel/arch/riscv/phys/**` except `bootshim/`, `kernel/arch/riscv/kernel/include/ours/arch/aspace_layout.hpp`, `kernel/phys/BUILD.gn`, `kernel/lib/ccrt`, riscv `arch::boot_marker`, `docs/changes/261028/riscv-phys.md`
- Lane `fake`: `kernel/tests/fake-kernel/**`, `.agents/skills/ours-verify/**`, `docs/changes/261028/fake-kernel.md`, `docs/changes/261028/ours-verify.md`

## Test plan
- Every lane: `ours-verify` on x86 with a build directory of its own (`--out out/x86-<lane>`), since runs in two worktrees that share a build-directory name share one temp directory.
- `dtb`: the conversion's host test; the shim under QEMU with and without `-initrd`, its printed ranges compared with the runtime tree's `/memory` and `/reserved-memory`.
- `paging`: the Sv39 smoke test on the host.
- `phys`: riscv `physload.bin` and the physboot module link.
- `fake`: the fake kernel's riscv ELF links; the riscv boot check reports which stage it reached.
- Integration: the riscv boot check reaches `[fake-kernel] handoff ok`.
