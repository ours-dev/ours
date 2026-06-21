# 06 — 中断处理子系统 (IRQ)

## 1. 概述

中断处理子系统管理硬件中断的注册、分发和处理。设计遵循 Zircon 的 `IrqDispatcher` / `IrqObject` 模式。

整体架构：

```
硬件中断 → IDT (x86 vector)
              │
              ▼
         exception.S: interrupt_common
              │
              ▼
         arch_handle_exception()          ← 架构层分发
              │
              ├── #PF → x86_handle_page_fault() → VmAspace::page_fault()
              ├── Platform IRQ → platform_handle_irq() + apic_issue_eoi()
              ├── ApicTimer → apic_handle_timer_irq()
              └── IPI vectors → IPI handler + apic_issue_eoi()
              │
              ▼ (Platform IRQ path)
         irq::IrqDispatcher::handle_irq(virq)
              │
              ▼
         IrqObject::notify()
              │
              ▼
         IrqObserver::on_event()          ← 注册的回调
```

## 2. IRQ 调度器 (IrqDispatcher)

```cpp
class IrqDispatcher {  // 全局单例
    IrqObject** m_objects;  // 按 virq 索引的 IrqObject 数组

    void init(usize default_count);  // 预分配 default_count 个 IrqObject
    Status request_irq(virq, handler, flags);  // 注册中断处理
    Status release_irq(virq);                  // 释放中断 (桩代码)
    void   handle_irq(virq);                   // 分发中断
};
```

**初始化**：通过 `GKTL_INIT_HOOK(Temporary, init_early, gktl::InitLevel::Vmm)` 注册，在 VMM 初始化阶段被调用。

## 3. IRQ 对象 (IrqObject)

```cpp
class IrqObject {  // 每个 IRQ 线一个
    HIrqNum hirq;              // 硬件 IRQ 号
    VIrqNum virq;              // 虚拟 IRQ 号
    IrqChip* chip;             // 关联的中断控制器 (IO-APIC)
    IrqFlags flags;            // 共享/独占等标志
    Mutex request_mutex;       // 保护观察者列表的互斥锁
    ObserverList observers;    // 侵入式观察者链表

    Status attach(IrqObserver*);   // 注册观察者 (检查共享约束)
    Status detach(IrqObserver*);   // 移除观察者
    void   notify();               // 遍历观察者，调用 on_event()
};
```

**观察者模式**：每个注册的处理函数对应一个 `IrqObserver`，存储在 `IrqObject` 的侵入式链表中。这支持多个驱动共享同一 IRQ 线（共享中断）。

## 4. IRQ 处理流程

### 4.1 Preamble / Postamble

```cpp
void handle_irq_preamble() {
    AutoPreemptDisabler disable;       // 禁用抢占
    AutoEagerReschedDisabler disable;  // 禁用急切重调度
}

bool handle_irq_postamble() {
    // 恢复抢占和急切重调度
    // 返回: 抢占是否变为启用状态
}
```

在 `x86_dispatch_exception()` 中使用：
```
handle_irq_preamble();
platform_handle_irq(vector);
apic_issue_eoi();
if (handle_irq_postamble()) {
    Thread::Current::preempt();  // IRQ 期间有抢占需求
}
```

### 4.2 平台 IRQ 处理 (x86)

```
platform_handle_irq(vector):
    virq = ioapic_irq_to_virq(vector)
    irq::handle_irq_generic(virq)
        → IrqDispatcher::handle_irq(virq)
            → IrqObject::notify()
                → for each observer: observer.on_event()
```

## 5. API (公共接口)

```cpp
namespace irq {
    Status init_early(usize default_irqs);  // 预分配 IrqObject
    Status request_irq(virq, handler, flags, name);
    Status release_irq(virq);               // 桩: 返回 Unimplemented
    Status set_affinity(virq, cpumask);     // 桩: 无操作返回 Ok
    void   handle_irq_generic(virq);        // 分发入口
}
```

## 6. 架构层中断初始化

x86 架构中：
- **IDT** (`x86_init_idt_early`) — 初始化 256 个门描述符，绑定 IST 栈
- **IO-APIC** (`init_io_apic`) — 映射 MMIO，配置 ISA 重定向
- **Local APIC** (`init_local_apic_percpu`) — 启用 xAPIC/x2APIC，配置 LVT
- **APIC Timer** (`setup_timer`) — 配置定时器 LVT 条目
- **中断栈** (`X86PerCpu::intr_stack`) — 5 个 IST 栈 (DoubleFault, NMI, DebugBreak, MachineCheck)

## 7. 实现状态一览

| 组件 | 状态 | 备注 |
|------|------|------|
| IrqDispatcher::init | **完整实现** | 预分配 IrqObject 数组 |
| IrqDispatcher::request_irq | **完整实现** | 创建 IrqObserver + attach |
| IrqDispatcher::release_irq | **桩代码** | 返回 Unimplemented |
| IrqDispatcher::handle_irq | **完整实现** | 查找 + notify |
| IrqObject 生命周期 | **完整实现** | create/attach/detach/notify |
| irq::set_affinity | **桩代码** | 无操作返回 Ok |
| irq::init (正式初始化) | **桩代码** | 仅注释了 arch::local_irq_enable() |
| handle_irq_preamble/postamble | **完整实现** | 抢占状态管理 |
| 中断上下文栈安全 | **完整实现** | IST 每 CPU 专用栈 |

**严重问题**：IRQ 头文件 (`ours/irq/*.hpp`) 均不存在于仓库中。`irq_*.cpp` 文件引用了 `<ours/irq/init.hpp>`、`<ours/irq/irq_dispatcher.hpp>` 等路径，但这些文件未被创建。这意味着 IRQ 子系统**当前无法编译**。

## 8. 设计要点与改进建议

### 8.1 当前设计优点
1. **观察者模式**：`IrqObject` → `IrqObserver` 的一对多关系支持中断共享
2. **Preamble/Postamble 模式**：清晰的中断进入/退出逻辑，确保抢占状态正确管理
3. **IST (Interrupt Stack Table)**：关键异常（DoubleFault, NMI, MCE）使用专用栈，防止栈溢出
4. **虚拟 IRQ 抽象**：`VIrqNum` 屏蔽了硬件 IRQ 号的差异

### 8.2 可改进之处
1. **IRQ 头文件缺失**：必须创建 `kernel/main/include/ours/irq/` 下的所有头文件
2. **release_irq 未实现**：驱动卸载时无法释放已注册的中断
3. **中断亲和性未实现**：`set_affinity()` 不做任何事，无法将中断绑定到特定 CPU
4. **无中断优先级**：所有平台 IRQ 一视同仁，无优先级抢占
5. **init() 未完成**：`arch::local_irq_enable()` 被注释掉，正式初始化不开启中断
6. **无 MSI/MSI-X 支持**：缺少对 PCI MSI 中断的抽象

### 8.3 从 Linux/Zircon 可借鉴的设计
- **Linux irqchip** 抽象：`struct irq_chip` 提供 mask/unmask/ack/eoi 等通用接口，当前 `IrqChip` 仅有骨架
- **Linux genirq**：通用中断子系统，支持中断线程化 (threaded IRQ handler)、中断均衡 (irqbalance)
- **Zircon InterruptManager**：每 CPU 的中断管理器，支持虚拟中断对象
- **Zircon MSI**：`MsiAllocation` + `MsiDispatcher` 的 MSI 分配框架
