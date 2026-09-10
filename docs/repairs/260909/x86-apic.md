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

## 3. resched IPI 的多播仍是 N 次完整往返

**严重程度**：低，性能。

`apic_send_ipi_mulitcast()`（`kernel/arch/x86/apic/ipi.cpp`）对每个目标调一次
`apic_send_ipi_unicast()`，于是每个目标都要重新构造一份 `IpiRequest`
（其实只有 `IcrHigh` 随目标变）、各自进出一次 `AutoInterruptDisabler`
（pushf/cli/popf），再各自等一次 ICR 空闲。

**怎么修**：把中断关闭和 request 的构造提到多播函数里，循环内只重写 `IcrHigh`
和 `IcrLow`。对照 Linux `default_send_IPI_mask_sequence_phys()`——
`local_irq_save()` 一次，然后在循环里逐个 `__default_send_IPI_dest_field()`。

（等待时机和 ESR 那两条已于 2026-09-10 修掉，见
`docs/changes/260909/x86-apic.md`。）

## 4. 没有带上界的 ICR 等待

**严重程度**：低。

`XApic::wait_icr_idle()` 是无上界自旋，对应 Linux 的 `__xapic_wait_icr_idle()`。
Linux 另有 `safe_apic_wait_icr_idle()`（1000 次上限，返回剩余次数），
用在 NMI 投递和 CPU 上线路径上——那两处如果 ICR 卡死，不该把发送方一起拖死。
本仓库还没有这个变体，`x86_bring_aps()` 的 INIT/SIPI 用的是无上界那份。
