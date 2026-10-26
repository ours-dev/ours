# build-toolchain changes

## Title: a riscv build directory configures: user-riscv toolchain, and a riscv64 compiler runtime (2026-10-26)

**Files**
- `build/toolchain/ours/BUILD.gn` — `ours_toolchain("user-riscv")`, copied from `user-x86` with `cpu = "riscv"`
- `prebuilt/clang/llvm-23/lib/clang/23/lib/riscv64-unknown-none-elf/libclang_rt.builtins.a` — new, not in git (`prebuilt/` is not); its recipe is in the `README.md` beside it and below

**Symptom**
`tools/set.py bringup.riscv` printed 49 `Unresolved dependencies` and `gn gen` failed. Every one of them was a target in `(//build/toolchain/ours:user-riscv)`. With that fixed, `ninja` stopped on `lib/clang/23/lib/riscv64-unknown-none-elf/libclang_rt.builtins.a, missing and no known rule to make it`.

**Cause**
Two separate gaps, both invisible while only x86 was ever built.

The user toolchain is named by pasting the CPU into a label: the root `BUILD.gn`, `system/component_manifest.gni` and `system/lib/component/test/BUILD.gn` all write `//build/toolchain/ours:user-$current_cpu` or `user-${target_cpu}`. A label that names a toolchain nobody declared resolves to nothing, and GN reports the targets that needed it, not the missing `toolchain()`. `build/toolchain/ours/BUILD.gn` declared `kernel-*` and `kernel-phys-*` for all three CPUs but `user-*` for x86 only.

The compiler runtime is a link input (`//kernel/products:static-pic-link` names the archive explicitly, because the link goes through `ld.lld` and never sees `-rtlib=compiler-rt`). The x86 archive was copied out of the official `LLVM-23.1.0-rc3-Linux-X64` release, which carries only the host's own runtime. LLVM publishes no riscv64 build, so there was nothing to copy.

**Change**
`user-riscv` repeats `user-x86` field for field. Only `cpu` differs, because the user-mode policy (`//system:user.config`, `//system:user.executable`) is not architecture-specific.

The runtime is built from the matching source release, `llvmorg-23.1.0-rc3`, with the prebuilt clang, so it is still upstream code at the toolchain's own version:

```text
cmake -G Ninja -S compiler-rt/lib/builtins -DCMAKE_SYSTEM_NAME=Generic
  -DCMAKE_C_COMPILER_TARGET=riscv64-unknown-none-elf
  -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY
  -DCOMPILER_RT_BAREMETAL_BUILD=ON -DCOMPILER_RT_DEFAULT_TARGET_ONLY=ON
  -DCOMPILER_RT_OS_DIR= -DCMAKE_C_FLAGS="-march=rv64ima_zicsr_zifencei -mabi=lp64" ...
```

- `CMAKE_SYSTEM_NAME=Generic` and `CMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY`: there is no libc, crt or OS to link a probe executable against, so cmake's compiler checks must stop at an archive.
- `COMPILER_RT_BAREMETAL_BUILD=ON`: drops the parts that need pthread, files or `abort`.
- `-march/-mabi` are the kernel's (`//kernel/arch/riscv:abi`). Built with the default `rv64gc/lp64d` the archive would hold floating-point instructions the kernel image cannot execute with the FPU off. Checked: ELF64, RISC-V, `e_flags` 0 (soft-float ABI), and `__udivti3`, `__umodti3`, `__muldi3`, `__divdi3` defined.

Rejected: leaving the archive out and adding symbols as the link complains. The compiler emits calls to these with no call site in the source (`unsigned __int128` division inside `std::format`), so the failure would arrive in some later, unrelated change.

**Reference**
None, this project's own build. The `user-$current_cpu` naming is this tree's; for the runtime, the only fact relied on is that `compiler-rt/lib/builtins` builds standalone for a bare-metal target (`COMPILER_RT_BAREMETAL_BUILD`), which its own `CMakeLists.txt` and `README.txt` describe. How Zircon sources its riscv64 runtime was not checked.

**Verification**
- `tools/set.py bringup.riscv`: `gn gen` passes, 1001 targets. Before: 49 unresolved dependencies.
- `gn gen out/bringup.x86` passes after the change (1008 targets, as before).
- `clang --target=riscv64-unknown-none-elf -rtlib=compiler-rt -print-libgcc-file-name` prints the path the archive was installed at.
- `ninja -C out/bringup.riscv` over the whole default group still fails, on `kernel/tests/fake-kernel/entry_riscv.S` (missing). Not part of this change.
- `ours-verify`: see the `riscv-boot.md` entry of the same day, which ran it once for this change and the two after it.

**Deliberately not done**
`user-arm`: the same gap exists, and nothing asked for arm. A `user-riscv` toolchain declares the environment; no user program builds for riscv yet (no vDSO, no ort for it), so this makes `gn gen` pass and says nothing about userspace on riscv.

A script that rebuilds the archive. The recipe is in the README beside it; a script is worth writing when a second machine needs it.

**How it was found**
`gn gen out/bringup.riscv` and reading what the 49 errors had in common: `grep -o '(//build/toolchain[^)]*)' | sort | uniq -c` gave only `user-riscv`, `kernel-riscv` and `host`. `grep -rn 'ours:user'` found the label being built from `$current_cpu`, and `grep -n '^ours_toolchain('` showed `user-x86` as the only user toolchain. The missing archive was `ninja`'s own message; `ls prebuilt/clang/llvm-23/lib/clang/23/lib/` showed x86 only, and its `README.md` says where that one came from.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why does GN report 49 targets and not "toolchain user-riscv is not declared"?
- Why must the builtins be built with the kernel's `-march/-mabi` and not the default `rv64gc/lp64d`?
- Why does a link that never calls `__udivti3` in the source still need `libclang_rt.builtins.a`?

## Title: the linker wrapper stops passing -mabi= to ld.lld (2026-10-26)

**Files**
- `build/toolchain/ours/elf_link.py` — `-mabi=` joins `-march=`, `-mtune=` and `-mcmodel=` in `_DRIVER_FLAG_PREFIXES`

**Symptom**
Linking any image in a riscv phys toolchain failed with `ld.lld: error: unknown emulation: abi=lp64d`, after every object had compiled.

**Cause**
`elf_link.py` exists because the Clang driver would hand an ELF link to the host's `gcc`, so the build calls `ld.lld` itself and passes it the link line GN produced. That line still carries compiler flags (they come from the same `ldflags`), which the wrapper strips by name or prefix. The prefix list had `-march=`, `-mtune=` and `-mcmodel=` and no `-mabi=`.

`ld` reads `-m<x>` as "emulation `<x>`", so `-mabi=lp64d` is the request for an emulation called `abi=lp64d`. x86 never passes `-mabi=`, so the list was complete for the only CPU that had linked through it; `//build/config/riscv` is the first to put `-mabi=lp64d` on the link line.

**Change**
```python
# before
_DRIVER_FLAG_PREFIXES = ("-march=", "-mtune=", "-mcmodel=")
# after
_DRIVER_FLAG_PREFIXES = ("-march=", "-mtune=", "-mcmodel=", "-mabi=")
```
Rejected: stripping every `-m*` flag. `ld` has real `-m` options (`-m elf_x86_64`), and a prefix list that fails loudly on an unknown flag is better than one that swallows a real option.

**Reference**
None, this project's own wrapper. The Clang driver (`clang::driver::tools::gnutools::Linker`) is what normally filters these; the wrapper stands in for it.

**Verification**
- `ninja` link of `//kernel/arch/riscv/phys/bootshim:bootshim` fails before, passes after.
- `ours-verify`: see the `riscv-boot.md` entry of the same day.

**Deliberately not done**
Anything for `arm`, which will need `-mabi=`-like flags of its own (`-mbranch-protection=`); they get added when an arm image first links.

**How it was found**
The failing command line printed by `ninja`: the `-march=rv64gc -mabi=lp64d` pair sat among the arguments after `elf_link.py`, and `grep -n "_DRIVER_FLAG_PREFIXES"` showed the list.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why is `-mabi=lp64d` an error for `ld` and not just an ignored unknown option?
- Why is a prefix list safer here than dropping every flag that starts with `-m`?
- The compile line has both `-mabi=lp64d` and, later, `-mabi=lp64`. Which one wins and why does that matter for the link?
