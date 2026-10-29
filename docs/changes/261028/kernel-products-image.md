# kernel-products-image changes

## Title: the fake image carries only the fake kernel, so it builds without user space (2026-10-28)

**Files**
- `kernel/products/image/fake/BUILD.gn` — the `fake-kernel` package no longer depends on `..:common_package_items`, and says why
- `kernel/products/image/BUILD.gn` — `common_package_items` is the payload of every image that starts user space, not of every image

**Symptom**
`fake.obi` did not build for riscv, so the riscv boot check booted the shim without an initrd:

```text
fake.obi   not built, so the shim boots without an initrd (0.5s): FAILED: [code=1] user-riscv/obj/system/lib/launch/src/launch.bootfs.cpp.o / clang++: error: argument unused during compilation: '-fno-plt' [-Werror,-Wunused-command-line-argument]
```

**Cause**
`common_package_items` puts userboot and the vDSO into an image's kernel package, built in the user toolchain (`user-${target_cpu}`). The fake image took it as every image does, but the fake kernel never reads either one: it checks the handoff, prints its report and halts. Nothing in physload or physboot reads them either; they travel in the BootFS for the real kernel (`kernel_package_handoff.hpp`). So the fake image built all of user space for nothing, and on riscv, whose user space does not build yet (x86-only flags in `system/BUILD.gn`, an x86 `syscall` in the vDSO, an x86 `crt0`, an x86-64-only ELF loader), that unused dependency stopped the image from building at all. A test of the phys chain was blocked by a part of the system the chain never reaches.

**Change**
Before:

```text
  deps = [
    "..:common_package_items",
    "//kernel/tests/fake-kernel:fake-kernel(//build/switch/set/dbg.0:kernel_${target_cpu}.dbg.0)",
  ]
```

After:

```text
  deps = [ "//kernel/tests/fake-kernel:fake-kernel(//build/switch/set/dbg.0:kernel_${target_cpu}.dbg.0)" ]
```

The kernel package's BootFS now holds `physboot` and `fake-kernel/{image.elf,code-patches}` and nothing else, on both architectures. The option ruled out was porting user space to riscv now (flags, a riscv vDSO and syscall ABI, `crt0`, the ELF loader) as a compile-only lane: it is real work the riscv phys chain does not need, and user space on riscv starts when the riscv kernel does. The decision was the user's, 2026-10-28.

**Reference**
Zircon does the opposite, and the reason is the difference between the two designs. Its physboot test (`kernel_package("physboot-test-data")` in `zircon/kernel/phys/test/BUILD.gn`) packages `trivial-test-userboot` and the vDSO beside the test kernel, with the comment that the test kernel does not use them but physboot panics if it cannot hand off loadable ELF files for them: Zircon's physboot loads userboot and the vDSO itself and passes them in the handoff. Here physboot does not touch them; the kernel reads them out of the kernel package (`KernelPackageHandoff`, and `Handoff`'s own comment: no vDSO or userboot handoff). So this image can leave them out, where Zircon's cannot.

**Verification**
- `obit tree` of both `fake.obi`: three items (physload, cmdline, kernel storage), the storage BootFS three files, no userboot, no vDSO.
- riscv: `fake.obi` builds for the first time (247672 bytes); the boot check boots with it as the initrd, the shim reports `initrd [0x88200000, 0x8823c778), 247672 bytes` and parks, `reached: shim`, as expected until the shim loads physload.
- x86 fake image booted under QEMU through `scripts/qemu_boot.py`: `[fake-kernel] handoff ok`, as before.
- x86 `ours-verify`: build ok, dbg PASS (44 passed, 4 skipped), rel PASS (41 passed, 8 skipped, 6/6 required), 152 host tests pass.
- `format_gn.py --check` clean on both files.

**Deliberately not done**
- Porting user space to riscv. The user's choice; it starts with the riscv kernel.
- Moving the fake image's packaging off `kernel_package`. It is still a kernel package with a kernel in it, only a shorter one.

**How it was found**
The riscv boot check reported the `fake.obi` build failure in its summary. The failing object was in `user-riscv`, which the fake image reached only through `common_package_items`; `grep` found no reader of userboot or the vDSO in physload, physboot or the fake kernel, and the x86 fake image booting to `handoff ok` without them confirmed it.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why must Zircon's physboot test image carry userboot and the vDSO when this one need not?
- Why did a flag problem in user space stop a test of code that only runs before the kernel?
- Where does the real kernel find userboot and the vDSO, and who keeps that memory alive for it?
