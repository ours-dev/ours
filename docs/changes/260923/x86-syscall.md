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

## SYSCALL 入口屏蔽 TF、DF、AC（2026-09-23）

**文件**
- `kernel/arch/x86/kernel/init.cpp` — `x86_enable_syscall()` 的 IA32_FMASK 加上 TF、DF、AC 三位

**症状**
潜伏，当前没有用户程序去设这三位。但任何一个用户程序只要一条 `popf` 就能设上，
设上之后就是内核漏洞，而不是用户程序自己的问题。

**原因**
SYSCALL 进入 ring 0 时，RFLAGS 除了被 FMASK 屏蔽的位以外原样带进来。原来的掩码只有
状态位、IF、IOPL、NT，漏了三位，每一位都各自构成一个问题：

- **TF**：单步陷阱会在内核第一条指令之后触发，这时桩还没离开用户栈。CPL 0 下 #DB 不换栈，
  CPU 会沿着用户选定的栈指针压异常帧——这等于让用户态替内核挑写入地址。
- **DF**：SysV ABI 保证每个函数入口 DF=0，编译器给 memcpy 生成的 `rep movs` 依赖这一点。
  用户带进来 DF=1，内核里每一次拷贝都会从起点往回倒着写。
- **AC**：在支持 SMAP 的 CPU 上，AC=1 正是「允许 ring 0 访问用户页」的开关。从用户态带进来，
  等于整个系统调用期间 SMAP 都是关着的。

**改法**
改前：

```cpp
    constexpr usize DISABLED_FLAG_BIT_MASK =
        RFLAGS_STATUS_MASK | RFLAGS_INTERRUPT_FLAG | RFLAGS_IOPL_MASK | RFLAGS_NESTED_TASK;
```

改后：

```cpp
    constexpr usize RFLAGS_TRAP_FLAG = usize{1} << 8;
    constexpr usize RFLAGS_DIRECTION_FLAG = usize{1} << 10;
    constexpr usize RFLAGS_ALIGNMENT_CHECK = usize{1} << 18;
    constexpr usize DISABLED_FLAG_BIT_MASK =
        RFLAGS_STATUS_MASK | RFLAGS_TRAP_FLAG | RFLAGS_INTERRUPT_FLAG | RFLAGS_DIRECTION_FLAG |
        RFLAGS_IOPL_MASK | RFLAGS_NESTED_TASK | RFLAGS_ALIGNMENT_CHECK;
```

每一位的理由写在注释里，挨着它的定义。

**参照**
Linux `syscall_init()`（`arch/x86/kernel/cpu/common.c`）的 SYSCALL_MASK；Zircon
`x86_init_percpu()`（`kernel/arch/x86/mp.cc`）：`X86_FLAGS_STATUS_MASK`（`0xfff`，
包含 TF、IF、DF）加上单独列出的 `X86_FLAGS_AC`。

**验证**
构建通过；两个镜像都启动到 shell 打完命令表。三位的反向测试（用户程序故意 `popf`
设 TF/DF/AC 再调用 vDSO）没有做——要写一个专门的测试程序，而且它在修复前的症状是
内核崩溃或静默的内存损坏，不是能从日志里干净读出来的结果。

**故意没做**
没有照 Zircon 在设置 FMASK 后把同一个掩码应用到当前 CPU 的 RFLAGS
（`x86_restore_flags(x86_save_flags() & ~mask)`，防固件留下的脏位）。那是另一件事，
和系统调用入口无关。

## 用户线程以 IOPL 0 启动（2026-09-23）

**文件**
- `kernel/arch/x86/kernel/include/ours/arch/entry.hpp` — `arch_build_uspace_irqframe()` 的 IOPL 由 1 改为 0

**症状**
无可观察的症状。这个值本身不给 ring 3 任何权限，但写法让人以为它给了。

**原因**
IOPL 表示「CPL 不高于它的代码可以不陷入地执行 in/out/cli/sti」。用户态是 CPL 3，IOPL=1
对它什么都不放开。原来的注释写的是 "Enable pesmission level"，把一个无效的数字说成了一项
特权。而且这个值会跟着每次 SYSRET（从 R11 恢复 RFLAGS）一直留在用户态的 RFLAGS 里。

用户态驱动如果需要端口访问，按设计是走每个任务自己的 TSS I/O 位图（方案里的 IoPorts 对象），
而不是一个所有进程都继承的标志位。

**改法**
改前：

```cpp
        frame->flags
            .set<ArchCpuState::IOPL>(1) // Enable pesmission level
            .set<ArchCpuState::IF>(1);  // Enable interrupt
```

改后：

```cpp
        frame->flags.set<ArchCpuState::IOPL>(0).set<ArchCpuState::IF>(1);
```

帧本来就是清零的，但仍然显式写出 0，并在注释里说明原来为什么是错的，免得有人照着旧写法
再把它改回去。

**参照**
Linux `start_thread_common()`（`arch/x86/kernel/process_64.c`）的 `regs->flags = X86_EFLAGS_IF`；
Zircon `arch_enter_uspace()` 的 `X86_FLAGS_IF`（外加 IOPL 为 0）。

**验证**
构建通过；两个镜像都启动到 shell 打完命令表。

**故意没做**
无。

## 系统调用处理函数在开中断的状态下运行（2026-09-23）

**文件**
- `kernel/lib/syscall/src/table.cpp` — `syscall_dispatch()` 进入后开中断、返回前关中断；顺手把取调用号的三目运算改成 if

**症状**
潜伏。IA32_FMASK 在进入时清掉了 IF，而之后再没有地方把它打开，于是整个处理函数都在关中断
状态下运行：不能被抢占；处理函数里的任何等待都是带着关中断进去的；一个耗时长的调用会让这个
CPU 在整个调用期间都收不到时钟中断。今天的两个调用都很短，所以看不出来。

**原因**
入口关中断是必要的：桩在换栈、把用户 RSP 从 per-CPU 槽搬进帧的那几条指令期间，一次中断就可能
让同一个 CPU 上的别的线程覆盖那个槽。但这个窗口在帧建好之后就结束了——用户状态已经全部在本线程
自己的内核栈上。此后再关着中断就只剩坏处。

出口同样需要关中断：桩从第一次弹栈开始就在恢复用户状态，接着装入用户 RSP、换回用户 GS。
这个窗口里来一次中断，处理程序看到的是内核 CS 配用户栈，或者一个它不会再换回来的用户 GS。

所以开关中断的时机必须**晚于**「用户 RSP 进帧」——这就是它排在那一步之后单独提交的原因。

**改法**
改前：

```cpp
extern "C" auto syscall_dispatch(arch::SyscallFrame *const frame) -> void {
    if (!trapped_from_vdso(frame)) [[unlikely]] {
        return sys_not_from_vdso(frame);
    }
    auto const number = frame->ax < SYSCALL_COUNT ? frame->ax : SYSCALL_COUNT;
    SYSCALL_TABLE[number](frame);
}
```

改后：

```cpp
extern "C" auto syscall_dispatch(arch::SyscallFrame *const frame) -> void {
    DEBUG_ASSERT(reinterpret_cast<usize>(frame) % 16 == 0, "misaligned syscall frame");

    arch::local_irq_enable();

    if (!trapped_from_vdso(frame)) [[unlikely]] {
        sys_not_from_vdso(frame);
    } else {
        auto number = SYSCALL_COUNT;
        if (frame->ax < SYSCALL_COUNT) {
            number = frame->ax;
        }
        SYSCALL_TABLE[number](frame);
    }

    arch::local_irq_disable();
}
```

开关放在 C++ 里而不是在桩里写 `sti`/`cli`，与 Zircon 相同：以后出口要检查挂起信号（被 kill 的
线程不应带着返回值回到用户态），那个检查也在 C++ 这一侧，必须在关中断之后、SYSRET 之前做，
否则会丢唤醒。现在所有要求都集中在同一个函数里。

提前 `return` 改成了单一出口，否则关中断那一行在拒绝路径上会被跳过。

**参照**
Zircon `do_syscall_pre()` / `do_syscall_post()`（`kernel/lib/syscalls/syscalls.cc`）：前者
`arch_enable_ints()`，后者 `arch_disable_ints()` 并在关中断后检查线程信号。Linux
`do_syscall_64()` 中的 `local_irq_enable()`（`syscall_enter_from_user_mode` 内部）与
`syscall_exit_to_user_mode` 中的关中断。

**验证**
构建通过；两个镜像各启动两次，都跑到 shell 打完命令表，无断言。

**故意没做**
出口的挂起信号检查（Kill/Suspend）属于 P1-A，要等线程退出路径能用，才有检查到之后该做的事。
