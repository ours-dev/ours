# x86-mp 改动记录

## AP 从跳板进内核的交接：寄存器约定与 online 的发布时机（2026-09-09）

**文件**
- `kernel/arch/x86/kernel/bootstrap16.S` — 64 位段按 SysV 顺序填参，补 `%rbp`／栈对齐
- `kernel/arch/x86/kernel/include/ours/arch/mp.hpp` — `x86_start_nonboot_cpu` 多一个 `Thread *`
- `kernel/arch/x86/kernel/mp.cpp` — 同上；`Cpu::State::Online` 挪走
- `kernel/top/main.cpp` — `start_nonboot_cpu()` 从空壳补成真正的 AP 收尾

**症状**
`SMP=4` 起 QEMU，三个 AP 全都在跳板之后立刻死掉：

```
[... cpu0 trace] Attempt to wake up CPU[1]
[... cpu0 trace] CPU[1]=(s: FFFFFF880400B000)
[... cpu0 error] Assertion failed on cpu < MAX_CPU with message: Invalid CPU Number
```

BSP 的 `x86_bring_aps()` 在 `while (!Cpu::is_online(cpu))` 上空转，整个引导停在这里。
把断言编译掉（`kernel.dev.obi`）则更糟：AP 拿一个野 cpunum 去装 `%gs`，之后每一次
per-CPU 访问都落在别人身上。

**原因**

**1. 跳板和 C 入口对不上寄存器。** 跳板末尾是这样填参的：

```asm
    // Let %rsi=cpunum
    xor %rsi, %rsi
    movl X86_BOOTSTRAP_OFFSET_CPUNUM(%rbx), %esi
    ...
    // Let %rdi point to current thread
    mov X86_BOOTSTRAP_OFFSET_PERCPU(%rbx, %rax, 8), %rdi
    jmp *X86_BOOTSTRAP_OFFSET_LONGMODE_VIRT_ENTRY(%rbx)
```

而被跳进去的是 `x86_start_nonboot_cpu(CpuNum cpunum)` —— 一个参数，SysV 规定它在
`%rdi`。于是 AP 把**内核线程指针的低 32 位**当成了自己的 CPU 号，
`CpuLocal::init_percpu()` 第一行就断言不过。

这个错之所以是致命的而不是"编号打印错了"，在于 cpunum 是 per-CPU 区域的索引：
`attach_to_unit()` 用它去查 `CPU_OFFSETS[cpu]` 并写进 `MSR_GS_BASE`。一个越界的
cpunum 不是让这颗 CPU 少认识自己，而是让它此后所有 `%gs` 相对寻址——当前线程、
调度器、TimerQueue、日志的 CPU 号——全部指到错误的地方去。

**2. 跳板用 `jmp` 进 C，栈是 16 对齐的。** SysV 说的是 `call` 那一刻 `%rsp` 16 对齐，
也就是被调用者入口处 `%rsp % 16 == 8`（返回地址已经压了）。编译器的序言就是照这个
偏移生成的。栈顶来自 `Stack::top()`，页对齐，直接 `jmp` 进去 `%rsp % 16 == 0`，
编译器一旦为了保存 XMM 发一条 `movaps`，就是 #GP。当前代码路径上还没有这样一条指令，
所以这一条是**潜伏**的，改 `become_idle()` 之后的任何东西都可能把它引爆。
顺带把 `%rbp` 清零，栈回溯到 AP 的第一帧就停住，而不是顺着固件留下的垃圾往下走。

**3. `Online` 发布得太早。** 原来的顺序是

```cpp
mm::CpuLocal::init_percpu(cpunum);
Cpu::set_state(Cpu::State::Online);   // <-- 这里
init_local_apic_percpu();
x86_init_percpu(cpunum);              // GDT / IDT / TSS 在这之后才装
start_nonboot_cpu();
```

`Online` 恰恰是 BSP 那个自旋等的东西：它一看见就去发下一颗 CPU 的 SIPI，
而**跳板数据页只有一份**——`bootstrap_data->cpunum` 会被就地改掉。也就是说
BSP 有可能在这颗 AP 还没装 IDT、还在读那一页的时候就把它覆盖了。
`Online` 必须意味着"这颗 CPU 已经能接线程了"。

**4. `start_nonboot_cpu()` 是个空壳。**

```cpp
extern "C" [[noreturn]]
auto start_nonboot_cpu() -> void {

    Thread::Current::become_idle();
}
```

缺的是 `task::init()` 结尾那两行——而那两行只能在**本 CPU 上**执行：

- `Scheduler::Current::set_active(true)`。`select_target_cpu()` 第一顺位查的就是
  Active 掩码；AP 不在里面，就算它活得好好的也永远拿不到线程。这正是修完前三条之后
  观察到的现象：三颗 CPU online，所有线程仍然挤在 CPU 0。
- `preemption_state().disable_preemption()`。`become_idle()` 收尾会调
  `enable_preemption()`，而它是**无条件的减一**（`m_counter.sub<...>(1)`），
  没有配对的 disable 就会把计数回绕到最大值，`is_preemption_enabled()` 从此恒假——
  这颗 CPU 会抱着 runnable 队列在 idle 循环里转到天亮。

**改法**
```asm
    movl X86_BOOTSTRAP_OFFSET_CPUNUM(%rbx), %edi     // 第一参数
    lea -1(%rdi), %rax
    shl $1, %rax
    mov X86_BOOTSTRAP_OFFSET_PERCPU(%rbx, %rax, 8), %rsp
    mov X86_BOOTSTRAP_OFFSET_PERCPU + 8(%rbx, %rax, 8), %rsi   // 第二参数
    xor %ebp, %ebp
    pushq $0
    jmp *X86_BOOTSTRAP_OFFSET_LONGMODE_VIRT_ENTRY(%rbx)
```
```cpp
auto x86_start_nonboot_cpu(CpuNum cpunum, task::Thread *thread) -> void {
    mm::CpuLocal::init_percpu(cpunum);
    DEBUG_ASSERT(task::Thread::Current::get() == thread,
                 "CPU[{}] attached to the wrong per-CPU unit", cpunum);
    init_local_apic_percpu();
    x86_init_percpu(cpunum);
    start_nonboot_cpu();     // Online 由它发布
}
```
```cpp
auto start_nonboot_cpu() -> void {
    task::Scheduler::Current::set_active(true);
    Thread::Current::preemption_state().disable_preemption();

    Cpu::set_state(Cpu::State::Online);
    log::info("CPU[{}] is online", Cpu::id());

    Thread::Current::become_idle();
}
```

第二个参数不是运行所必需的（`init_percpu()` 之后 `Thread::Current::get()` 自己就答得
出来），留着是因为它是跳板和 per-CPU 区域**各自独立算出来的同一个值**：一比就知道这颗
CPU 有没有挂到该挂的 unit 上，而挂错 unit 的后果是静默的数据损坏，不是崩溃。

被否掉的方案：改 C 侧签名成 `(Thread *, CpuNum)` 去迁就汇编。否掉是因为 CPU 号才是
这个入口的主语，`cpunum` 排第一读起来才对，而且 `mm::CpuLocal::init_percpu(cpunum)`
是它做的第一件事。

**参照**
- Linux `secondary_startup_64`（`arch/x86/kernel/head_64.S`）—— 同样在跳进 C 之前
  清 `%rbp` 并把栈摆成 `call` 之后的样子。
- Linux `start_secondary()`（`arch/x86/kernel/smpboot.c`）—— AP 收尾的形状：本 CPU
  自己做的事做完，**最后**才 `set_cpu_online()`，然后进 `cpu_startup_entry()`。
  `start_nonboot_cpu()` 是它的对应物。

**验证**
`prebuilt/ninja/ninja -C out/bringup.x86` + `SMP=4 ./scripts/qemu-boot.sh`
（`kernel.dev.dbg_1.obi`）：

```
[4172186000 cpu1 info] CPU[1] is online
[4199622000 cpu2 info] CPU[2] is online
[4269697000 cpu3 info] CPU[3] is online
```

三颗 AP 都走完了跳板、拿到自己的 per-CPU 区域、装上了自己的 GDT/IDT/TSS 和 local APIC，
BSP 的 `x86_bring_aps()` 循环正常走完。`SMP=1` 的行为不变。

**故意没做**
- AP 随后仍会在 `Scheduler::reschedule_common()` 的
  `m_runtime.which_cpu == Cpu::id()` 断言上停住——那是调度器侧的另一个缺陷，
  单独记在 `docs/repairs/260909/task-sched.md`。
- `BootstrapData::stack_top` / `cpunum` 与 `percpu[]` 数组是重复的：BSP 是一颗一颗
  串行唤醒的，一对 (栈顶, 线程) 就够了。没有在这一改里合并，因为那是接口精简，
  不是这个 bug。
- INIT-SIPI-SIPI 只发了一次 SIPI（Intel SDM 建议 INIT → 10ms → SIPI → 200µs → SIPI）。
  QEMU 一次就起来，真机上未必。
