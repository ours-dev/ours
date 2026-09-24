# x86-syscall 改动记录（2026-09-24）

## SYSRET 之前检查挂起的线程信号（2026-09-24）

**文件**
- `kernel/arch/x86/kernel/syscall.S` — `call syscall_dispatch` 之后、弹帧之前 `call x86_syscall_process_pending_signals`
- `kernel/arch/x86/kernel/faults.cpp` — `x86_syscall_process_pending_signals()` 的定义
- `kernel/lib/syscall/src/table.cpp` — 注释里"还没写的挂起信号检查"改为指向它

**症状**
潜伏。一个线程在系统调用里被置上 Kill（被别的线程，或下一步里被它自己的调用），SYSRET 照样把它
送回 ring 3。信号要等这个线程下一次恰好在 ring 3 挨一个中断才会被处理——对一个在用户态自旋、而
时钟又不落到它身上的线程，可能要很久。

**原因**
回用户态有两条路：异常的 `iretq`（第 19 条之后在尾部处理信号）和系统调用的 `sysret`。只查一条路，
另一条路就是信号的漏洞。检查要在关中断之后做：之后再到来的信号，由发送方随后的 IPI 在 SYSRET
之后立刻打断用户态接住，而不会落在"检查之后、返回之前"那段没人管的空隙里。

**改法**
改前：

```asm
    call syscall_dispatch

    pop_reg %rax
    ...
```

改后：

```asm
    call syscall_dispatch

    call x86_syscall_process_pending_signals

    pop_reg %rax
    ...
```

```cpp
extern "C" auto x86_syscall_process_pending_signals() -> void {
    task::Thread::Current::process_pending_signals();
}
```

`syscall_dispatch()` 返回前已经关了中断，正是 `process_pending_signals()` 入口断言要的状态。
`%rsp` 仍停在帧顶，16 字节对齐，这次 `call` 同样满足 SysV 的对齐要求。

与 Zircon 的两处不同：
- Zircon 只在 `do_syscall()` 的返回值说有信号时才调（`.Lthread_signaled`）；这里无条件调——没有信号时
  是一次读、一次返回，省得把一个标志穿过分发器传回来。
- Zircon 把保存的寄存器传进去，给调试器读挂起线程的状态用；这里还没有人读，所以不传。

**参照**
Zircon `kernel/arch/x86/syscall.S` 的 `.Lthread_signaled` 与 `x86_syscall_process_pending_signals()`
（`kernel/arch/x86/exceptions_c.cc`）。

**验证**
- 构建通过；两个镜像在 `SMP=4` 与 `SMP=1` 下都跑到 `shell: bad-pointer checks passed` 与 `--- end ---`；
  dev 镜像 init↔shell 对话照常（init 阻塞在 `object_wait_one` 里，醒来后正常返回）。
- 每个系统调用都经过这条新路径，dbg_1 下 `process_pending_signals()` 的入口断言（中断必须关）没有触发。
- Kill 在这里的效果要到第 29 条（非 vDSO 陷入杀进程）才能实测。

**故意没做**
无。
