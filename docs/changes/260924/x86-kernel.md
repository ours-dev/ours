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

## 来自 ring 3 的 CPU 异常杀掉进程，而不是停掉内核（2026-09-24）

**文件**
- `kernel/include/ours/task/exception.hpp`（新建，内核私有）— `ExceptionType`（数值同 `ZX_EXCP_*`）、`ExceptionContext`、`arch_dump_exception_context()` 的声明
- `kernel/arch/x86/kernel/faults.cpp` — 分发时识别来自用户态的异常并返回上下文；中断收尾之后 `deliver_user_exception()`；`arch_dump_exception_context()`
- `kernel/arch/x86/mm/fault.cpp`、`include/ours/arch/x86/faults.hpp` — `x86_handle_page_fault()` 改为接收调用者先读出的 CR2
- `kernel/arch/x86/lib/arch/include/arch/x86/interrupt.hpp` — `to_string(IrqVec)` 补上几个用户能触发的向量名
- `kernel/include/ours/task/thread.hpp`、`kernel/kernel/task/thread.cpp` — `Thread::Current::on_user_exception()`
- `kernel/object/thread_dispatcher.cpp`、`.hpp` — `ThreadDispatcher::on_user_exception()`：报告并 `kill(TASK_RETCODE_EXCEPTION_KILL)`
- `kernel/mm/include/ours/mm/stack.hpp` — `Stack::base()`

**症状**
任何用户程序执行一条 `ud2`、读一个未映射的地址、除以零，内核都会 panic 停机——异常分发不看来自哪一环，
一律走 `panic_exception()`。P1-A 完成判据的后一半就是这件事。

**原因**
用户态的故障是程序自己的，它的后果应该落在程序身上。内核停机只该留给内核自己的错误。

**改法**
分三段，各在自己的层里：

1. **架构层认出它、描述它。** `x86_dispatch_exception()` 多一个出参；来自 CPL3、且属于用户能引起的
   那一类向量时，填一个 `ExceptionContext`（类型、向量名、错误码、rip、sp、CR2、帧）并返回 true，
   而不是处理或 panic。映射照 Zircon：#UD → UndefinedInstruction；地址空间解决不了的 #PF →
   FatalPageFault；#DB → HwBreakpoint（主人决定：程序能用 popf 置 TF，没有调试器接手时放它回去
   等于让它随意刷串口）；#BP → SwBreakpoint；#DE、#OF、#BR、#NP、#SS、#GP、#MF、#AC、#XM、#CP →
   General。NMI、#NM、#DF、#CSO、#TS、#MC、#VE、#VC 不论哪一环都仍是致命的。
   CR2 在分发的第一步读出、传给缺页处理——此时中断仍关着，而处理过程可能自己再缺页、覆盖 CR2。

2. **中断收尾之后再交付。** 第 19 条的尾部（postamble → 抢占）之后，来自用户态且有异常时调
   `deliver_user_exception()`：断言帧在本线程的内核栈上；开中断；`Thread::Current::on_user_exception()`；
   关中断；`process_pending_signals()`——Kill 在这里生效，线程退出，不再返回；万一返回，panic。

   ```cpp
    if (from_user) {
        if (raised) {
            deliver_user_exception(frame, user_exception);
        }
        Thread::Current::process_pending_signals();
    }
   ```

3. **对象层决定后果。** `ThreadDispatcher::on_user_exception()` 打一行报告和寄存器，然后
   `m_process->kill(TASK_RETCODE_EXCEPTION_KILL)`。死的是进程而不是线程（Zircon 同）：一个线程出了故障，
   它的进程里没有别的东西可以被信任能收拾残局。报告用 warn 级：程序死于自己的 bug 不是内核错误。
   没有异常通道（`zx_task_create_exception_channel`），所以现在没有人能先接住它。

**参照**
- Zircon `x86_exception_handler()` / `handle_exception_types()` / `x86_unhandled_exception()` /
  `x86_pfe_handler()`（`kernel/arch/x86/exceptions_c.cc`）、`dispatch_user_exception()`、
  `arch_dump_exception_context()`（无帧时打 "no frame to dump"）。
- `ZX_EXCP_*`（`zircon/syscalls/exception.h`）、`ZX_TASK_RETCODE_EXCEPTION_KILL`。

**验证**
- 构建通过；两个镜像在 `SMP=4` 与 `SMP=1` 下照常跑到 `shell: bad-pointer checks passed` 与 `--- end ---`。
- 临时在 shell 的 `--- end ---` 之后加一条 `ud2`（未提交），两个镜像都依次出现：

  ```
  terminating process 'shell' (koid 23) thread koid 27: type=UndefinedInstruction vector=invalid opcode error_code=0x0 rip=0x202a7a sp=0x11fff40 cr2=0x0
    rax=0x0  rbx=0x0  rcx=0xa01447  rdx=0x3
    ...
  object: thread (koid 27) of process 'shell' destroyed
  object: process 'shell' (koid 23) destroyed
  ```

  之后内核继续运行（dbg_1 一直跑到 30 秒观察窗口结束，别的内核线程照常打日志），没有断言。
- 换成读地址 `0x10`：`type=FatalPageFault vector=page fault error_code=0x4 ... cr2=0x10`，同样两行 destroyed。
- 下一条把这些做成常驻的探针测试。

**故意没做**
- 把 3 号门（#BP）设成 DPL 3、让 `int3` 在用户态成为断点：没有调试器需要它，不在 P1-A。
- 被杀进程的地址空间、页表、用户页和内核栈目前都泄漏（`~VmAspace` 为空；`VmAspace::destroy` 在 P1-B），
  数量以被杀的进程数为上限。
- 退出码只能在内核日志里看到（`process_exit`、get_info 在 P1-B）。

## TSS 的 I/O 位图关掉全部端口（2026-09-24）

**文件**
- `kernel/arch/x86/kernel/include/ours/arch/x86/percpu.hpp` — `X86Tss::init()` 把整张 I/O 位图置全 1

**症状**
第 17–29 条的对抗审查发现（早于 P1-A）：ring 3 可以对 0..0xFFFF 任何端口执行 `in`/`out`，不产生任何异常。
用户程序往 0x64 或 0xCF9 写一个字节就能让机器复位；经 0xCF8/0xCFC 能改 PCI 配置空间（打开 bus master、
重映射 BAR，再用 DMA 写内核内存）；能直接编程 PIT、PIC、COM1。P1-A 的"#GP 杀进程"在端口 I/O 上从来不会触发。

**原因**
用户线程运行在 IOPL 0（`entry.hpp`），这时 CPL 3 的 `in/out/ins/outs` 由 TSS 的 I/O 权限位图决定：位为 1
拒绝（#GP），为 0 放行。`X86Tss::init()` 只设置了位图基址和末尾的终止字节，8192 字节的位图本身保持零初始化，
即**全部放行**；TSS 描述符的 limit（`sizeof(X86Tss) - 1`）又覆盖了整张位图，处理器不会因越界而拒绝。
`entry.hpp` 里"端口访问归 TSS I/O 位图管"的注释说的没错，只是这张位图什么都没管。

**改法**
改前：

```cpp
    auto init() -> void {
        io_port_bitmap_base = static_cast<u16>(ustl::mem::offset_of(&Self::io_bitmap));
        io_bitmap[arch::MAX_IO_BITMAP_BYTES] = 0xff;
    }
```

改后：

```cpp
    auto init() -> void {
        io_port_bitmap_base = static_cast<u16>(ustl::mem::offset_of(&Self::io_bitmap));
        for (auto &byte: io_bitmap) {
            byte = 0xff;
        }
    }
```

位图之后的那个字节也仍是全 1（架构要求：跨越最后一个字节的访问会读到它）。将来需要端口的驱动进程，
由它自己的线程打开它需要的那几个端口，而不是对所有程序敞开。

**参照**
Zircon `x86_reset_tss_io_bitmap()`（`kernel/arch/x86/ioport.cc`，`bitmap_set` 整张位图），在
`x86_initialize_percpu_tss()`（`descriptor.cc`）里于 `ltr` 之前调用。

**验证**
- 构建通过；两个镜像启动正常。
- 临时在 shell 的 `--- end ---` 之后 `inb $0x80` 再打一行 "SURVIVED"（未提交）：
  - 改前：打出 `shell: SURVIVED a port read`——端口读直接成功；
  - 改后：`terminating process 'shell' ... type=General vector=general protection error_code=0x0`，随后两行 destroyed，
    没有 "SURVIVED"。

**故意没做**
每进程的 I/O 端口授权（Zircon 的 `zx_ioports_request`）：还没有需要端口的用户驱动。

## 勘误：P1-A 第 17–29 条对抗审查查出的记录错误（2026-09-24）

本文件只增不改，更正写在这里。

- 「来自 ring 3 的 CPU 异常杀掉进程」一节把 NMI 列为"不论哪一环都仍是致命的"。错：NMI 只打日志然后返回，不论打断的是哪一环；
  真正致命的是 #NM、#DF、#CSO、#TS、#MC、#VE、#VC。
- 同一节把 "#CP → General" 算作照 Zircon 的映射。错：Zircon 对 #CP 在任何一环都 `exception_die`；这里把 ring 3 的 #CP 当作
  程序的错杀掉进程，是本内核的偏离。CET 没有开启，今天不会有 #CP。
- 同一节说"地址空间解决不了的 #PF → FatalPageFault"。有一个缺口：`x86_handle_page_fault()` 丢掉了错误码的取指位，
  从已映射但不可执行的用户页取指会被地址空间当成伪缺页"解决"，无限重试而不是杀进程。已交接（mm 的取指原因与权限检查）。

## 重调度 IPI 只发一次 EOI，由分发器发（2026-09-24）

**文件**
- `kernel/arch/x86/kernel/mp.cpp` — `x86_handle_ipi_resched()` 去掉自己发的 `apic_issue_eoi()`，补上为什么不在这里发的注释

**症状**
潜伏，当前触发不到。每个 `IrqVec::IpiResched` 都发两次 EOI：`x86_handle_ipi_resched()` 开头一次，
`x86_dispatch_exception()` 的 `case IrqVec::IpiResched:` 在它返回后又一次。今天中断处理全程关中断、不嵌套，
第二次 EOI 时 ISR 里没有别的位，写 EOI 寄存器什么也不发生，所以看不出来。
一旦某个中断处理里重新开中断（比如平台 IRQ 处理为了等慢设备而开中断），
而重调度 IPI 正好嵌在里面，外层那个中断就会被提前退役。

**原因**
EOI 不带向量号。LAPIC 收到 EOI 时清掉的是 ISR 里**当前优先级最高**的那一位——它不知道、也不关心是哪个处理函数写的。
嵌套时 ISR 里同时有两位：外层（比如一个平台 IRQ）和内层的 IpiResched（向量更高，所以优先级更高）。
第一次 EOI 清掉 IpiResched，这是对的；第二次 EOI 此时看到的最高位已经是**外层那个**，于是把它也清了，
而外层的处理函数还在跑。后果：
- 外层向量所在的优先级类不再被 ISR 屏蔽，同一个源（或同类更低的源）可以在它的处理函数返回前再次打进来，
  处理函数被自己重入；
- 对电平触发的 IOAPIC 源，EOI 还会经 LAPIC 广播给 IOAPIC 清 Remote-IRR，设备还没被服务完就重新投递；
- 外层处理函数最后自己那次 EOI 又会去清"当时最高的那位"，错位一路传下去——每多一次 EOI，就有一个中断被错误地退役。

三个 IPI 向量里只有 IpiResched 这样：IpiGeneric、IpiInterrupt 的处理函数都不发 EOI，统一由分发器发。
两种约定混在一起本身就是隐患——读分发器的人会以为每个 case 各发一次，读处理函数的人会以为 EOI 已经在里面了。

**改法**
改前（`mp.cpp`）：

```cpp
auto x86_handle_ipi_resched() -> void {
    apic_issue_eoi();

    auto const this_cpu = Cpu::id();
    log::trace("IPI-Resched on CPU[{}]", this_cpu);

    task::Thread::Current::preemption_state().add_pending();
}
```

改后：

```cpp
/// No EOI here: x86_dispatch_exception() issues it after this returns, as it
/// does for every other IPI vector. An EOI retires whichever vector is highest
/// in service, not "this one", so a second EOI from here would retire the
/// interrupt this IPI nested in -- letting its source fire again while its
/// handler is still running. Zircon's mp_mbx_reschedule_irq() likewise leaves
/// the EOI to the X86_INT_IPI_RESCHEDULE case in x86_exception_handler().
auto x86_handle_ipi_resched() -> void {
    auto const this_cpu = Cpu::id();
    log::trace("IPI-Resched on CPU[{}]", this_cpu);

    task::Thread::Current::preemption_state().add_pending();
}
```

分发器（`faults.cpp`）不动，三个 IPI case 都是"调处理函数，再 `apic_issue_eoi()`"：

```cpp
            case IrqVec::IpiResched:
                x86_handle_ipi_resched();
                apic_issue_eoi();
                break;
```

被否掉的方案：反过来，删掉分发器里的那次、让每个处理函数自己发。否掉是因为 EOI 属于"中断控制器"这一层，
处理函数属于"这个 IPI 要做什么"这一层（`x86_handle_ipi_generic()` 转手就交给了与架构无关的 `IpiService`），
把 EOI 放进去等于让每个新 IPI 的作者都记得一件和他的逻辑无关的事；而且 Zircon 也是分发器发。

EOI 从 `add_pending()` 之前挪到了之后，这不改变行为：此时 IF=0，EOI 早一点晚一点，下一个 IPI 都要等 `iretq` 开中断才能进来。

**参照**
Zircon `x86_exception_handler()`（`kernel/arch/x86/exceptions_c.cc`）里的 `case X86_INT_IPI_RESCHEDULE:`：
先调 `mp_mbx_reschedule_irq()`，再 `apic_issue_eoi()`；`mp_mbx_reschedule_irq()`（`kernel/kernel/mp.cc`）本身只置
`PreemptSetPending()`，不碰 APIC。`X86_INT_IPI_GENERIC` / `X86_INT_IPI_INTERRUPT` 同一写法。

**验证**
- `prebuilt/ninja/ninja -C out/bringup.x86` 构建通过；`clang-format` 无改动；`clang-tidy -p out/bringup.x86` 在改动行上无新诊断。
- `kernel.dev.obi` 启动约 16 s：`init: shell says "pong"`、`shell: bad-pointer checks passed` 都在，无 panic。
- `kernel.dev.dbg_1.obi` 启动约 16 s：`shell: bad-pointer checks passed` 在；日志里 `IPI-Resched` /
  `reschedule ipi` 共 12449 行，四个 CPU 上的两个测试线程持续交替运行，没有挂死或丢调度。
  （这个镜像由内核直接起 shell，没有 init，所以本来就不打 "pong"。）
- 嵌套场景本身未实测：今天没有会在中断处理里开中断的路径，造不出 ISR 里两位同时在的状态。

**故意没做**
- 没把分发器里各 case 的 `apic_issue_eoi()` 收成 switch 之后的一处：ApicTimer 的 EOI 在 `apic_handle_timer_irq()`
  里面（只发一次，没有重复），ApicSpurious 按架构不能 EOI，IpiSuspend 永不返回，收拢要另起一次改动并逐个核对。
- `PlatformIrqMin...Max` 那一支的 `TODO: Replace it with IrqChip::issue_eoi()` 不在本次范围。
