# x86/kernel 待修项（2026-09-23）

对 P0 系统调用系列做对抗式审查时发现的，不是那个系列引入的。（原第 2 条"vDSO 入口校验的下边界差一个指令长度"已由 P1-A 第 28 条修掉，见 `docs/changes/260924/syscall.md`。原第 1 条的 IST 部分已在 `docs/changes/260925/x86-kernel.md` 修掉，下面是它剩下的一半。）

## 1. NMI / #MC 按 CS 判断要不要 swapgs，syscall 出入口的窗口里会拿着用户的 GS 基址访问 per-CPU 数据

**严重程度：中。** 窗口只有几条指令，触发靠 NMI 的时机；但有 FSGSBASE 的 CPU 上 GS 基址由用户任意设定。

- `interrupt_common`（`exception.S`）只看压栈的 CS：CPL 为 3 才 `swapgs`。
- `x86_syscall`（`syscall.S`）进门时，`syscall` 已把 CPL 变成 0，但到第一条 `swapgs` 之前 GS 基址还是用户的；
  出门时，`swapgs` 之后到 `sysretq` 完成之前也一样。IA32_FMASK 清了 IF 和 TF，普通中断和单步落不进这两个窗口，
  NMI 和 #MC 可以。
- 落进去的 NMI 看到 CS 是内核，就不 `swapgs`，`arch_handle_exception()` 里所有 `%gs:` 相对访问（`CpuLocal`、
  `Thread::Current::get()`、logz4 的 CPU 号和域槽）都落在用户选的地址上。CR4.FSGSBASE 在支持的 CPU 上是开的，
  用户态可以 `wrgsbase` 任意值再 `syscall`：没有 SMAP 时内核会把用户页当成自己的 per-CPU 块读（伪造的当前线程指针），
  有 SMAP 或 GS 为 0（没有 FSGSBASE 时用户改不了它）时，是 ring 0 碰用户地址，`x86_handle_page_fault()` 拒绝后内核 panic。

改法：NMI（和 #MC）不信 CS。照 `Zircon interrupt_common` 的 `.Lnmi` 分支：`rdmsr MSR_IA32_GS_BASE` 存进被调用者保存的寄存器，
用 IST 栈的地址反推本 CPU 的 per-CPU 基址（IST 栈就嵌在 `X86PerCpu` 里）写进 GS_BASE，处理完再写回原值。
ours 的 %gs 装的是"本 CPU 单元相对模板的偏移"（`percpu.hpp` 顶部），反推时要换算成这个值，而不是 `X86PerCpu` 的地址。
Linux 的对应物是 `paranoid_entry` / `paranoid_exit`（`arch/x86/entry/entry_64.S`），有 FSGSBASE 时同样是存—写—还原，
而不是看 GS_BASE 的符号位。
