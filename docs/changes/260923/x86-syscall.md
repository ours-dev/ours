## 用户 RSP 存进系统调用帧，不再整段寄存在 per-CPU 槽里（2026-09-23）

**文件**
- `kernel/arch/x86/kernel/syscall.S` — 换栈后立刻把用户 RSP 压进帧；出口 `popq %rsp`
- `kernel/arch/x86/lib/arch/include/arch/x86/syscall.hpp` — `SyscallFrame` 末尾加 `sp`，断言帧大小是 16 的倍数
- `kernel/arch/x86/lib/arch/include/arch/x86/macro/syscallframe.hpp` — 加 `X86_SYSCALLFRAME_OFFSET_SP`，帧从 17 槽变 18 槽
- `kernel/lib/syscall/src/table.cpp` — `syscall_dispatch` 入口 `DEBUG_ASSERT` 帧 16 字节对齐

**症状**
潜伏，当前触发不到。今天的两个系统调用都不会睡眠，所以看不出来。第一个会阻塞的调用
（`object_wait_one`，或者任何在 Mutex 上发生竞争的处理函数）一落地就会触发：SYSRET 回到
一个不属于调用者的用户栈上。

另一半：`call syscall_dispatch` 时 `%rsp ≡ 8 (mod 16)`，违反 SysV 对齐约定
（`docs/repairs/260908/x86.md` 原第 1 条）。

**原因**
SYSCALL 不压任何东西，`%rsp` 进内核时还指着用户栈。桩要先把它存在某处才能换到内核栈，
原来的做法是存进本 CPU 的 `tss.sp2` 槽，**并且整个调用期间都留在那里**，出口再从这个槽
读回来。

这只在「处理函数从不睡眠」时成立：
- 线程 A 在处理函数里阻塞，同一个 CPU 上线程 B 发起系统调用，B 的用户 RSP 覆盖了这个槽，
  A 醒来返回时拿到的是 B 的栈；
- 线程 A 在 CPU 0 阻塞、在 CPU 1 醒来，它读到的是 CPU 1 的槽。

per-CPU 状态只能描述「此刻在这个 CPU 上的东西」，而一次会睡眠的系统调用跨越了调度点，
它的状态必须跟着线程走，也就是放进线程自己的内核栈。

对齐是同一处的另一个后果：17 个槽 = 136 字节，从 16 字节对齐的栈顶往下压，`call` 时
差 8。现在没炸，只是因为内核用 `-mno-sse` 编译，还没有需要 16 字节对齐的溢出。

**改法**
换栈后立即 `pushq %gs:SAVED_USER_SP`，让用户 RSP 成为帧的第一个（地址最高的）成员；
per-CPU 槽从此只做两条指令的中转。出口弹完 RFLAGS 后，栈上剩下的正是这个值，直接
`popq %rsp`。

改前：

```asm
    mov %rsp, CPU_LOCAL_ACCESS(X86_PCPU + X86_PERCPU_SAVED_USER_SP_OFFSET)
    mov CPU_LOCAL_ACCESS(X86_PCPU + X86_PERCPU_SAVED_KERNEL_SP_OFFSET), %rsp
    push_val %r11  // FLAGS
    ...
    pop_reg %r11  // RFLAGS

    // Restore user stack.
    mov CPU_LOCAL_ACCESS(X86_PCPU + X86_PERCPU_SAVED_USER_SP_OFFSET), %rsp
```

改后：

```asm
    mov %rsp, CPU_LOCAL_ACCESS(X86_PCPU + X86_PERCPU_SAVED_USER_SP_OFFSET)
    mov CPU_LOCAL_ACCESS(X86_PCPU + X86_PERCPU_SAVED_KERNEL_SP_OFFSET), %rsp
    pushq CPU_LOCAL_ACCESS(X86_PCPU + X86_PERCPU_SAVED_USER_SP_OFFSET)  // SP
    push_val %r11  // FLAGS
    ...
    pop_reg %r11  // RFLAGS
    pop_reg %rsp
```

多出的这一槽同时把帧凑成 18 × 8 = 144 字节，`call` 时恰好对齐。改成「帧本身是 16 的
倍数」而不是在 `call` 前 `and $-16, %rsp`，是因为出口必须按帧的确切大小弹回去，而
`sizeof(SyscallFrame) % 16 == 0` 这条断言让对齐成为类型自己说明的性质。

这一行写成裸的 `pushq` 而不是 `push_val`：宏按空白切分参数，而 `CPU_LOCAL_ACCESS` 展开
后含空格（第一次构建就因此报 "too many positional arguments"）。

**参照**
Linux `entry_SYSCALL_64`（`arch/x86/entry/entry_64.S`）：`movq %rsp, PER_CPU_VAR(cpu_tss_rw
+ TSS_sp2)` → 换栈 → `pushq PER_CPU_VAR(cpu_tss_rw + TSS_sp2)` 进 `pt_regs->sp`；
快速返回路径以 `popq %rdi; popq %rsp; swapgs; sysretq` 结束。Zircon `x86_syscall`
（`zircon/kernel/arch/x86/syscall.S`）同样把用户 RSP 存进 `x86_syscall_general_regs_t`。

**验证**
构建通过；`kernel.dev.obi` 与 `kernel.dev.dbg_1.obi` 都启动到 shell 打完命令表
（`--- end ---`），dbg_1 带 `DEBUG_ASSERT`，对齐断言未触发。帧对齐由 `static_assert`
在编译期保证。

「睡眠后返回正确的栈」这一半今天无法实测——还没有会睡眠的系统调用。它会在
`object_wait_one` 落地时由用户态验证。

**故意没做**
- 没有顺手核对/改 `exception.S` 的 `interrupt_common`：`IrqFrame` 22 槽 = 176 字节，
  长模式下 CPU 压中断帧前会先把 RSP 对齐到 16，所以本来就是对齐的，不用改。
- FMASK、IOPL、处理函数期间开中断是同一个入口的另外三处问题，各自单独提交。
