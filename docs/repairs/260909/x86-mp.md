# x86-mp —— 待修项（2026-09-09）

AP 已经能起来并接线程了（见 `docs/changes/260909/x86-mp.md`），下面是没修的。

## 1. INIT-SIPI-SIPI 只发了一次 SIPI

**严重程度**：中，真机上才会咬人。

`kernel/arch/x86/kernel/mp.cpp` 的 `x86_bring_aps()`：先给所有目标发 INIT，
睡 10 ms，然后每颗 CPU 发**一次** Startup IPI。Intel SDM（§9.4.4 的 MP 初始化协议）
要求的是 INIT → 10 ms → SIPI → 200 µs → **第二次 SIPI**，第二次是给第一次没被接住的
处理器兜底的。QEMU 一次就起来，真机不保证。

同时该看的：INIT 之后没有等 ICR 的 Delivery Status 清零就往下走
（`XApic::wait_for_ipi()` 只在 `send_ipi()` 里做，看着是够的，但值得对一遍）。

**参照**：Linux `wakeup_secondary_cpu_via_init()` / `wakeup_cpu_via_init_nmi()`。

## 2. BSP 在关中断的情况下自旋等 AP 上线

```cpp
apic_send_ipi_unicast(cpu, vec, ApicDeliveryMode::Startup);
arch::local_irq_disable();
Thread::Current::sleep_for(Milliseconds(1), task::Interruptible::Yes);
while (!Cpu::is_online(cpu))
    ;
```

**严重程度**：中。

`sleep_for()` 内部的 Guard 会保存/恢复中断状态，所以它返回之后中断仍然是关的，
那个 `while` 是**关中断自旋**。AP 起不来（第 1 条那种情况）就是死机，没有超时、
没有诊断。而且 `Cpu::is_online()` 的读是非原子的（见
`docs/repairs/260909/task-sched.md` 第 6 条）。

**怎么修**：给自旋加一个基于 `time::SteadyClock` 的超时，超时就 `log::error` 并把这颗
CPU 从 possible/present 掩码里去掉，然后继续下一颗；`local_irq_disable()` 那一行
先确认它到底想保护什么——看起来是想让 `sleep_for` 之后不被抢占，但 `sleep_for` 自己
已经处理了。

**参照**：Linux `do_boot_cpu()` 的 `cpu_initialized_mask` 轮询有 5 秒超时。

## 3. `BootstrapData` 里 `stack_top`/`cpunum` 和 `percpu[]` 是重复的

**严重程度**：低，接口冗余。

`kernel/arch/x86/kernel/include/ours/arch/x86/bootstrap.hpp`：`percpu[2 * MP_MAX_CPUS]`
按 `cpunum - 1` 存 (栈顶, 线程) 对，而 `stack_top` 字段 `x86_bring_aps()` 也在写、
汇编却不读。BSP 是**一颗一颗串行**唤醒的（发完 SIPI 就等 online），所以一对就够，
16 颗 CPU 的数组是多余的，还多一次"索引要不要减一"的机会犯错——今天那个寄存器错位
就是在这段索引代码旁边。

删掉 `percpu[]`，让汇编读 `stack_top` / `long_mode_virt_entry` 那几个单值字段。
（Zircon 的 `x86_bootstrap16_acquire()` 就是这么做的。）

## 4. `arch_activate_cpu()` / `arch_deactivate_cpu()` 返回 `Unimplemented`

**严重程度**：低（没有调用者）。CPU 热插拔还不存在，留着是脚手架。

## 5. 跳板里 `mov %ds, %ebx` 依赖 16 位下写 32 位寄存器会零扩展

`kernel/arch/x86/kernel/bootstrap16.S`：

```asm
    mov %ds, %ebx
    shl $4, %ebx
```

`MOV r32, Sreg` 的高 16 位在 Intel 手册里是 undefined（部分处理器补零，部分保留原值），
而进入长模式时 `%rbx` 的高 32 位也不是架构保证为零的。QEMU 上没问题。
稳妥写法是 `movzwl %ds, %ebx`，并在长模式段开头 `mov %ebx, %ebx` 把高位显式清掉。
