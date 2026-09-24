# x86/kernel 待修项（2026-09-23）

对 P0 系统调用系列做对抗式审查时发现的，不是那个系列引入的。（原第 2 条"vDSO 入口校验的下边界差一个指令长度"已由 P1-A 第 28 条修掉，见 `docs/changes/260924/syscall.md`。）

## 1. IST 从来没配置：NMI / #MC 压栈到地址 0 附近，内核栈溢出直接三重错误

**严重程度：中。** 今天就能触发（QEMU monitor 的 `nmi`、硬件 #MC、任何一次内核栈溢出），只是少见。

- `IntrStackOrdering`（`kernel/arch/x86/kernel/include/ours/arch/x86/percpu.hpp:49-55`）从 0 编号：
  `DOUBLE_FAULT_STACK = 0`、`NMI_STACK = 1`、`DEBUG_BREAK_STACK = 2`、`MACHINE_CHECK_STACK = 3`。
  `x86_init_idt_early()`（`idt.cpp:120-122`）把这些数直接写进门的 `ist` 字段——#DF 于是是 IST 0，也就是
  **不换栈**。
- `tss.ist1..ist7` 从来没有被写过：`x86_setup_tss_percpu()`（`descriptor.cpp:91-106`）只调 `tss->init()`，
  全树 grep 不到任何对 `.istN` 的赋值；`X86PerCpu::intr_stack[]` 声明了却没人用。
- 结果：NMI / #MC 从 `tss.ist1` / `tss.ist3` 取到 RSP = 0，第一次压栈在 `0xffff_ffff_ffff_fff8`，缺页，
  panic 成「kernel 地址上的 Unhandled page fault」；内核栈溢出时 #PF 压不了栈，#DF 在同一个坏栈上也压不了，
  三重错误、没有任何诊断。

改法：
1. IST 编号改成从 1 开始（或写门时 +1），让 #DF、NMI、#MC 各有真的 IST 槽；
2. `x86_setup_tss_percpu()` 里 `tss->ist{n} = intr_stack[n-1]` 的高端，照 Zircon
   `x86_initialize_percpu_tss()`；
3. 在依赖它之前，给 NMI / #MC / #DB 的桩加「偏执」的 GS 处理：读 `MSR_GS_BASE` 或与本 CPU 的 per-CPU
   基址比较，而不是信 CS 的 RPL。`syscall.S` 在 swapgs 前后各有一个两条指令的窗口，CR4.FSGSBASE 开着，
   用户态可以 `wrgsbase` 任意值——落在窗口里的 NMI 会拿着用户选的 GS 去访问 per-CPU 数据。
