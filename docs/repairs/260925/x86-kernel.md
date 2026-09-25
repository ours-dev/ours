# x86/kernel 待修项（2026-09-25）

修 NMI 的 IST 栈与 %gs 时看到的，不是那两处改动引入的。按严重程度排。

## 1. NMI 路径打日志，会在同一颗 CPU 上自锁

**严重程度：中。** 需要 NMI 恰好打在持有串口锁的 CPU 上；真机上 NMI 一多（watchdog、PMU 的 NMI 模式）就不罕见。

- `x86_dispatch_exception()` 对 NMI 做的唯一一件事是 `log::info("#NMI")`（`faults.cpp`）。
- logz4 的串口输出拿一把 TAS 锁（`kernel/lib/logz4/kernel.cpp` 里 `m_lock.test_and_set`）。NMI 打断的如果正是持锁的那段代码，
  NMI 处理函数会在同一颗 CPU 上永远等这把锁：持锁者要等 NMI 返回才能继续。
- 同理，NMI 路径上任何拿锁的代码都有这个问题；IST 栈的注释里"NMI/#MC 路径不能出错"是同一类约束。

改法：NMI 路径不同步打日志。Zircon 的 `x86_nmi_handler()` 只做 `kcounter_add(exceptions_nmi, 1)` 和 CPU 上下文交换请求，
不打印；Linux 让 NMI 里的 printk 写进 per-CPU 缓冲，稍后在安全的上下文里刷出（`printk_nmi` / `printk_safe`，新版是 nbcon 的延后刷出）。
最小的一步是把那行日志换成一个 per-CPU 计数。

## 2. `sysretq` 返回非规范 RCX 时，#GP 落在 ring 0、用户的栈和 %gs 上

**严重程度：低（潜伏，当前触发不到）。**

- Intel CPU 上，`sysretq` 发现 RCX 不是规范地址时，在 CPL 0 触发 #GP——而此时 `syscall.S` 已经 `popq %rsp` 换回用户栈、`swapgs` 换回用户 %gs。
  #GP 没有 IST，于是在用户选的栈上压内核的异常帧（CVE-2012-0217 那一类）。
- 现在触发不到：RCX 只来自 `syscall` 自己存下的返回地址，而那是一条刚执行过的用户指令的下一条，必然规范；没有任何路径改写 SyscallFrame 里的 ip。
- 一旦有了修改线程寄存器的接口（调试器、线程状态写入、信号返回），就必须处理。

改法（二选一或都做）：写入线程状态时拒绝非规范或内核半区的 ip，照 Zircon `x86_is_ip_canonical()`（`kernel/arch/x86/mmu.cc`，
`debugger.cc` 写 RIP 前检查）；或在返回路径上检查 RCX，不合格就改走 `iretq`，照 Linux `do_syscall_64()` 里 "SYSRET with non-canonical RCX/RIP will #GP" 那段判断（`arch/x86/entry/syscall_64.c`）。
