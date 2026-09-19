# userabi 待修项（2026-09-19）

## 1. 间歇性：用户态跳进未映射地址，像是没被绑定的 vDSO 桩

**现象**（`kernel.dev.obi` 与 `kernel.dev.dbg_1.obi` 都复现过一次）

```
[3596249000 cpu0 info] userboot: mapped [200000, 204000) into its own aspace, entry at 201400, stack 1200000
[3596931000 cpu0 info] userboot: vdso mapped [A00000, A03000)
[3599831000 cpu1 error] Unhandled page fault: vector=14, error_code=0x14
[3601156000 cpu1 error]   rip=0x15c6 (+0x77c00015c6)  cs=0x33  rsp=0x11fffa0
[3601676000 cpu1 error]   rax=0x0  rbx=0x3b  rcx=0x0  rdx=0x0
[3601916000 cpu1 error]   rsi=0x3b  rdi=0x2003b5  rbp=0x11fffc8
[3602304000 cpu1 error]   r12=0x0  r13=0x0  r14=0x2003b5  r15=0x3b
```

**读数**

- `error_code=0x14` = `0b10100`：P=0（页不存在）、U/S=1（用户态）、I/D=1（取指）。
  所以是**用户态取指落在未映射页上**，不是数据访问。
- `cs=0x33` 确认在 ring 3。
- `rip=0x15c6` **既不在 shell 的 `[200000, 204000)` 里，也不在 vDSO 的
  `[A00000, A03000)` 里**。它是个低地址，形状像**未重定位的文件内偏移**，
  而不是映射后的虚拟地址。
- `rdi = r14 = 0x2003b5` 落在 shell 正文里（`0x200000 + 0x3b5`），`rbx = rsi = r15 = 0x3b`。
  看起来是带着参数去调一个桩，而桩的地址是错的。

**猜测（未证实）**：用户程序在加载时把 syscall 绑定到 vDSO 的那一步，有一条目标被
留成了链接期/文件内的值而没有加上 vDSO 的映射基址。

**间歇性，而且不常见**：同一镜像连跑 3 次未复现，对照组（不含当时在改的调度器改动）
连跑 3 次也未复现；只在最初那一次各命中一次。所以**它不是调度器改动引入的**——
对照实验已经排除。

**排查建议**

1. `rip=0x15c6` 去 `shell` 和 `vdso` 的 ELF 里查：它落在哪个节、对应哪个符号。
   如果它等于某个 vDSO 导出符号的**文件偏移**，那就坐实了「少加基址」。
2. 既然是间歇的，先看绑定与映射之间有没有顺序/可见性问题——
   多核下另一个 CPU 是否可能在绑定完成前就进了 ring 3。
3. 复现时把窗口拉长、多跑几次；只跑一次很容易漏掉。

**归属**：`kernel/lib/userabi`、`kernel/lib/elfload`。这两处在同一天有活跃改动
（`9c1929f` 把 syscall 绑定接到 vDSO，`4f5c209` / `966704b` 把 binding 和 placement
拆成组件），所以大概率与那条线相关。
