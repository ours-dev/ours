# x86-kernel 改动记录（2026-09-24）

## 异常尾部的次序改为"收尾 → 抢占 → 信号"；NMI/#DF/#MC 跳过整套簿记（2026-09-24）

**文件**
- `kernel/arch/x86/kernel/faults.cpp` — `arch_handle_exception()`

**症状**
潜伏，两处：
1. NMI 走完整套 preamble/postamble。一个嵌在 IRQ 里的 NMI，它的 postamble 会把每 CPU 的
   `CURRENT_FRAME` 清成空指针，而栈上那个 IRQ 还没处理完，之后它按"有帧"继续执行（prof 采样就读
   `current_frame()`）。NMI 的 postamble 还可能要求抢占，于是在 NMI 屏蔽尚未解除时切换线程。
2. 来自 ring 3 的异常，信号处理发生在 postamble **之前**。下一条要让 Kill 信号在这里直接
   `exit` 线程；若仍在 postamble 之前，退出的线程会留下 preamble 抬高的抢占计数和指向它已死的
   栈的 `CURRENT_FRAME`。

**原因**
- NMI 可以在任何地方到来，包括另一个中断的 preamble 和 postamble 之间、每 CPU 结构更新到一半时；
  而 preamble/postamble 做的恰恰是改这些每 CPU 状态。NMI 之后在下一次 `iretq` 之前一直被屏蔽，
  在那之前切换线程，那次 `iretq` 就属于另一个线程的返回路径了。#DF 与 #MC 在分发里直接停机、
  不会返回；跳过簿记是因为这两条路径上不该依赖每 CPU 状态是一致的。
- 信号处理可能让线程就此结束，所以它必须在中断完全收尾之后才能发生。

**改法**
改前：

```cpp
extern "C" auto arch_handle_exception(arch::IrqFrame *frame) -> void {
    auto const irqnum = HIrqNum(frame->vector);
    platform_start_handling_irq(irqnum, frame);

    x86_dispatch_exception(frame->vector, frame);

    using task::Thread;
    if (is_from_uspace(frame)) {
        log::trace("Exception[{}] from user space", to_string(frame->vector));
        Thread::Current::process_pending_signals();
    }

    auto const need_preempt = platform_finish_handling_irq(irqnum, frame);
    if (need_preempt) {
        Thread::Current::preempt();
    }
}
```

改后：

```cpp
extern "C" auto arch_handle_exception(arch::IrqFrame *frame) -> void {
    using task::Thread;
    auto const vector = frame->vector;

    if (vector == arch::IrqVec::Nmi || vector == arch::IrqVec::DoubleFault ||
        vector == arch::IrqVec::MachineCheck) {
        x86_dispatch_exception(vector, frame);
        return;
    }

    auto const irqnum = HIrqNum(vector);
    auto const from_user = is_from_uspace(frame);

    platform_start_handling_irq(irqnum, frame);
    x86_dispatch_exception(vector, frame);
    auto const need_preempt = platform_finish_handling_irq(irqnum, frame);

    if (need_preempt) {
        Thread::Current::preempt();
    }
    if (from_user) {
        log::trace("Exception[{}] from user space", to_string(vector));
        Thread::Current::process_pending_signals();
    }

    DEBUG_ASSERT(arch::irqs_disabled(), "interrupts enabled on the way out of exception {}",
                 to_string(vector));
}
```

`from_user` 在分发之前取：分发可能改帧（缺页恢复会改 `ip`），判断"从哪来"要用进来时的样子。
抢占放在信号之前：线程被切走期间收到的信号，在它回 ring 3 之前就能看到，而不是再等一个中断。
出口断言中断仍是关的——存根的 `iretq` 路径要求如此，任何一步把中断留开都会在这里现形。

**参照**
Zircon `x86_exception_handler()`（`kernel/arch/x86/exceptions_c.cc`）：`int_handler_start` →
`handle_exception_types` → `int_handler_finish` → `Preempt()` → `arch_iframe_process_pending_signals()` →
`DEBUG_ASSERT_MSG(arch_ints_disabled(), ...)`；开头 `DEBUG_ASSERT(frame->vector != X86_INT_NMI)`，
NMI 另有 `x86_nmi_handler()`。

**验证**
- 构建通过；两个镜像在 `SMP=4` 与 `SMP=1` 下都跑到 `shell: bad-pointer checks passed` 与 `--- end ---`。
- `SMP=1` 的 dbg_1 日志里有 446 次 `Exception[interrupt] from user space`：shell 在 ring 3 自旋时，
  时钟中断的抢占必然落在 ring 3，每一次都走了新的尾部，出口断言一次也没有触发。

**故意没做**
- NMI 的处理本身（`#NMI` 那行 `log::info`）没动：在 NMI 里打日志，若 NMI 恰好打断了持有日志锁的代码
  会死锁，这是另一件事。
- IST 仍未配置（`docs/repairs/260923/x86-kernel.md` #1）：NMI/#DF/#MC 在坏栈上仍然是致命的，
  这一条只保证它们不进入簿记、抢占和信号路径。
