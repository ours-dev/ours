# x86-kernel 待修项（2026-09-24）

## 1. 每个叶子页表项都被强制加上 Writable 与 Global（已交接给独立会话）

`kernel/arch/x86/lib/arch/include/arch/paging/x86_pagings.hpp` 的 `X86Pte<Level>::make()` 把
`X86_KERNEL_PAGE_FLAGS`（Present|Accessed|Writable|Global|Dirty）OR 进每一个叶子项，
非叶子项 OR `X86_KERNEL_PD_FLAGS`。`MmuFlagsConv<X86MmuFlags>::to_arch()` 本身是对的，
但算出来的权限随后被这组默认值覆盖。

证据：`SMP=1` 启动 dev 镜像，在 shell 处于 ring 3 时用 QEMU monitor `info tlb` 看：程序
（0x200000..）和 vDSO（0xa00000..，按 `User|Readable|Executable` 映射）都是 `-G-DA--UW`。
表现：`ours_channel_read` 往 `vdso_base` 读出消息返回 `Ok`——内核直接写进了只读的 vDSO。

后果：
- 全系统没有只读保护：内核 `.text`/`.rodata`、所有只读用户映射，ring 0 都能写；
- 用户页是全局页，CR3 切换后 TLB 项仍在，init 与 shell 都在 0x200000 与 0xa00000 映射，
  一个进程可能用上另一个进程的陈旧翻译。

改法照 Zircon `X86PageTableMmu::terminal_flags()`（`kernel/arch/x86/mmu.cc`）：叶子只带请求的权限，
Global 只给内核地址空间（`use_global_mappings_`）；非叶子 `intermediate_flags()` = RW|U。
预计会暴露一批依赖"默认可写/全局"的内核映射，逐个修。

完成后要补的验证：`examples/shell/shell.cpp` 的坏指针检查里，往 `vdso_base` 写的三项
（`channel_read(into the vDSO)`、`wait_one(observed = vDSO, poll)`、`channel_create(out0 = vDSO)`）
应当都回 `InvArgs`，那时把它们加回去（见 `docs/changes/260924/examples-shell.md`）。
