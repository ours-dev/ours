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

## QEMU 默认起四核（2026-09-09）

**文件**
- `scripts/qemu-boot.sh` — `-smp` 的默认值从 1 改成 4

**症状**
不是 bug。今天为止本地跑的每一次引导都是单核（`-smp "${SMP:-1}"`），
于是多核路径上的四个缺陷——跳板的寄存器约定、local APIC 的软件使能、
调度器的 `which_cpu` 与负载指标、跨 CPU 入队——一个都碰不到。
它们全都是"单核下恰好也对"的那一类。

**原因**
默认值就是覆盖率。一个默认单核的验证脚本，等于把内核里唯一一段真正区分
"这颗 CPU"和"某颗 CPU"的代码永久排除在日常回归之外。

**改法**
```sh
SMP=${SMP:-4}
```
放在 NUMA 那一段**之后**：`NUMA=1` 的拓扑是照着 8 颗 CPU 写的（`cpus=0-1,2-3,4-6,7`），
它自己那句 `SMP=${SMP:-8}` 必须先生效。放在前面会把 NUMA 布局悄悄缩成四核，
剩下四颗 CPU 没有节点归属。

**参照**
无，本项目自有设计。

**验证**
不带任何环境变量直接跑 `./scripts/qemu-boot.sh`，30 秒窗口：
cpu0 8589 行、cpu1 644 行（userboot）、cpu2 100135 行、cpu3 100187 行。
`SMP=1` 仍可用来做单核对照。

**故意没做**
`NUMA=1` 那条路径没有实测（它要一个 `mm_numa = true` 的单独构建）。

## 跳板的交接改回单槽，去掉 `percpu[2 * MP_MAX_CPUS]`（2026-09-10）

**文件**
- `kernel/arch/x86/kernel/include/ours/arch/x86/bootstrap.hpp` — 数组换成一个 `thread` 标量
- `kernel/arch/x86/kernel/bootstrap16.S` — 两条定偏移 load，不再算下标
- `kernel/arch/x86/kernel/mp.cpp` — 只写一组值

**症状**
不是 bug。昨天修寄存器约定时保留了 `percpu[]` 这张表，同时留下一个**没有读者**的
`stack_top`：`mp.cpp` 仍然在写它，`X86_BOOTSTRAP_OFFSET_STACK_TOP` 和它的
`static_assert` 成了墓碑。

**原因**
这张表的前提是"可能有多颗 AP 同时在跳板里"。而 `x86_bring_aps()` 是**严格串行**的：

```cpp
apic_send_ipi_unicast(cpu, vec, ApicDeliveryMode::Startup);
...
while (!Cpu::is_online(cpu))
    ;
```

发一颗、等它上线、再发下一颗。任何时刻只有一组值是活的。

多出来的这张表要的代价：

- 一个 `cpunum - 1`、两项一组的下标约定，要**三个文件同时同意**
  （bootstrap.hpp 的宏注释、mp.cpp 的写入、bootstrap16.S 的读取），
  三处各写了一段注释解释它；
- 在 16 位跳板搬进来的那一页里，一个大小随 `mp_max_cpus` 增长的数组——
  而 `bootstrap.cpp` 还要从同一片 `MIN_BOOTSTRAP16_BUFFER_SIZE`（3 页）里
  挤出 GDT 和临时 PGD；
- 64 位段里 `lea -1(%rdi), %rax; shl $1, %rax` 加一个 scratch 寄存器，
  在一段**没有符号、只能靠 `__debug_symbol*` 下断点**的代码里做指针算术。

昨天那个把 cpunum 和线程指针放反的 bug，就出在这段下标代码旁边。

**改法**
```cpp
    VirtAddr stack_top;
    CpuNum cpunum;
    u32 padding3_;
    VirtAddr thread;       // 原 percpu[] 的位置
```
```asm
    movl X86_BOOTSTRAP_OFFSET_CPUNUM(%rbx), %edi
    mov X86_BOOTSTRAP_OFFSET_STACK_TOP(%rbx), %rsp
    mov X86_BOOTSTRAP_OFFSET_THREAD(%rbx), %rsi
```
```cpp
    bootstrap_data->cpunum = cpu;
    bootstrap_data->stack_top = initial_stack;
    bootstrap_data->thread = reinterpret_cast<VirtAddr>(current_thread);
```

三条定偏移访存，没有下标、没有 scratch 寄存器、没有 `MP_MAX_CPUS` 进跳板页。
`DEBUG_ASSERT(MAX_CPU < 1000'000'000'000'000'000)` 也一并删了——它是给那个下标
算法写的，本身也没有意义。

**参照**
Zircon `x86_bootstrap16_acquire()` / `x86_bootstrap16_data`：同样是单槽，
同样靠"一次只起一颗"来保证安全。

**验证**
四核引导：三颗 AP 依次 online，userboot 在 CPU1 跑到 ring 3，Sample1/Sample2
分别在 CPU2/CPU3，无断言失败。

**故意没做**
- 没有借机改成"一次全发"。并行唤醒要 AP 自己从 APIC ID 反推 cpunum
  （`apic_id_to_cpunum()` 已经有了），是另一件事，记在
  `docs/repairs/260909/x86-mp.md`。
- `__debug_symbol0..4` 留着。它们是给这段无符号代码下断点用的，不是死代码。
