# x86/kernel 待修项（2026-09-25）

修 NMI 的 IST 栈与 %gs 时看到的，不是那两处改动引入的。按严重程度排。（原第 1 条"NMI 路径打日志会自锁"已修，见 `docs/changes/260925/x86-kernel.md` 第 3 条。）

## 1. `sysretq` 返回非规范 RCX 时，#GP 落在 ring 0、用户的栈和 %gs 上

**严重程度：低（潜伏，当前触发不到）。**

- Intel CPU 上，`sysretq` 发现 RCX 不是规范地址时，在 CPL 0 触发 #GP——而此时 `syscall.S` 已经 `popq %rsp` 换回用户栈、`swapgs` 换回用户 %gs。
  #GP 没有 IST，于是在用户选的栈上压内核的异常帧（CVE-2012-0217 那一类）。
- 现在触发不到：RCX 只来自 `syscall` 自己存下的返回地址，而那是一条刚执行过的用户指令的下一条，必然规范；没有任何路径改写 SyscallFrame 里的 ip。
- 一旦有了修改线程寄存器的接口（调试器、线程状态写入、信号返回），就必须处理。

改法（二选一或都做）：写入线程状态时拒绝非规范或内核半区的 ip，照 Zircon `x86_is_ip_canonical()`（`kernel/arch/x86/mmu.cc`，
`debugger.cc` 写 RIP 前检查）；或在返回路径上检查 RCX，不合格就改走 `iretq`，照 Linux `do_syscall_64()` 里 "SYSRET with non-canonical RCX/RIP will #GP" 那段判断（`arch/x86/entry/syscall_64.c`）。
