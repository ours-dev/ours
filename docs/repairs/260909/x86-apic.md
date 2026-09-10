# x86-apic —— 待修项（2026-09-09）

## 1. AP 的 LINT0 / LINT1 没有屏蔽

**严重程度**：低（QEMU 上不触发），真机上要补。

`init_local_apic_percpu()`（`kernel/arch/x86/apic/local_apic.cpp`）给每颗 CPU 配了
LVT Error 和 LVT PMI，没有碰 LINT0 / LINT1。

Linux 的 `setup_local_APIC()` 只让 **BSP** 的 LINT0 走 ExtINT（好接 8259 的遗留中断），
AP 上两条都设成 masked；不这么做的话，一颗 AP 可能会接到本来只该 BSP 处理的
遗留中断。QEMU 的 q35 + IOAPIC 配置下观察不到，真机固件千奇百怪。

**参照**：Linux `setup_local_APIC()` 里的 `value = APIC_DM_EXTINT; if (!cpu) ... else value |= APIC_LVT_MASKED;`

## 2. `XApic` 是一个全局单例，字段里存着 MMIO 基址

`kernel/arch/x86/lib/arch/include/arch/x86/apic/xapic.hpp` 的 `XApic` 只有一个实例
（`XAPIC_STORAGE`），所有 CPU 共用。这在 xAPIC 下**是对的**——每颗 CPU 的 local APIC
都映射在同一个物理地址 0xFEE00000 上，读写自然落到自己那一份（`init_local_apic()` 的
注释已经说明了）。

记在这里只是因为它容易被误读成 per-CPU 状态：`XApic` 里除了 `mmio_base_` 不该再长出
任何 per-CPU 字段，否则四颗 CPU 会共享它。x2APIC 打开之后（`X2APIC_ENABLED`）
访问方式变成 MSR，这个类还没有对应的分支。

## 3. resched IPI 的多播是 N 次完整往返，而且每次都带一段用不上的尾部自旋

**严重程度**：中，性能——而且是这次多核打通之后才真正跑起来的路径
（以前 `IpiService::send_resched()` 把所有目标都过滤掉了）。

`apic_send_ipi_mulitcast()`（`kernel/arch/x86/apic/ipi.cpp`）对每个目标调一次
`apic_send_ipi_unicast()`，于是每个目标都要：重新构造一个 `IpiRequest`
（其实只有 IcrHigh 随目标变）、自己一次 `AutoInterruptDisabler`（pushf/cli/popf）、
两次 MMIO 写，然后 `wait_for_ipi()` 自旋——而那个循环每轮读**两个**非缓存 MMIO
寄存器（`IcrLow` 和 `Esr`）。

三处可改：
1. 把 disabler 和 request 的构造提到多播函数里，每个目标只重写 IcrHigh；
2. 把等待挪到**写之前**而不是之后。xAPIC 要求的是"下一次写 ICR 之前它是空闲的"，
   写完再等纯粹是给唤醒方加延迟——而 `reschedule_mask(CpuMask::of(target))`
   这种单目标唤醒正是常见情形。Linux 的 `__xapic_wait_icr_idle` 就是这个顺序。
3. 自旋循环里那次 `Esr` 读可以去掉，它让每轮的 MMIO 流量翻倍。

**没有顺手改的原因**：第 2 条改的是 IPI 发送的时序协议，值得单独一轮，配合真机验证。
