# x86/kernel 改动记录（2026-09-25）

## 给 #DF、NMI、#MC 配上真正的 IST 栈（2026-09-25）

**文件**
- `kernel/arch/x86/kernel/include/ours/arch/x86/percpu.hpp` — 新增 `ist_slot()`，把从 0 编号的 `IntrStackOrdering` 换成门描述符里从 1 编号的 IST 槽号；`X86Tss` 的 `ist1..ist7` 改成数组 `ist[7]` 并加 `set_ist(slot, top)`；补 `offsetof(X86Tss, ist) == 0x24` 的静态断言
- `kernel/arch/x86/kernel/idt.cpp` — 三个门改用 `ist_slot(...)`；`x86_set_vector_ist()` 断言槽号在 1..7
- `kernel/arch/x86/kernel/descriptor.cpp` — `x86_setup_tss_percpu()` 把本 CPU 的 `intr_stack[]` 顶端填进 TSS 的 IST 槽

（前一个提交 `81a5ab7f` 先把 `InterruptStatck` 改名为 `InterruptStack`，并把它颠倒的 `top()`/`bottom()` 改成和 `mm::Stack` 一致的 `base()`/`top()`，否则这里取到的会是栈的低端。）

**症状**
今天就能触发。启动完成后在 QEMU monitor 里打一次 `nmi`，内核把正在跑的 shell 当成自己出错杀掉：

```
terminating process 'shell' (koid 35) thread koid 39: type=FatalPageFault vector=page fault
    error_code=0x2 rip=0x202e42 sp=0x11fff20 cr2=0xfffffffffffffff8
```

内核栈溢出时（按架构推断，未实测）：#PF 压不上栈，升级成 #DF；#DF 不换栈，还在同一个坏栈上压，三重错误，机器复位，没有任何诊断。

**原因**
x86-64 在门描述符的 `ist` 字段非 0 时，进门前无条件把 %rsp 换成 TSS 里 `IST<n>` 的值。这是给"当前 %rsp 不可信"的向量准备的：
#DF（栈溢出时 #PF 已经压不上去了）、NMI 和 #MC（能落在任何一条指令上，包括 `syscall` 之后、换栈之前，那时 %rsp 还是用户的）。

ours 两头都错了：

1. **门里写的编号差一。** `IntrStackOrdering` 从 0 编号（它是 `intr_stack[]` 的下标），却被原样写进门的 `ist` 字段：

   ```cpp
   enum IntrStackOrdering { DOUBLE_FAULT_STACK, NMI_STACK, DEBUG_BREAK_STACK, MACHINE_CHECK_STACK, MAX_NUM_STACKS };
   ...
   x86_set_vector_ist(arch::IrqVec::DoubleFault, DOUBLE_FAULT_STACK);   // ist = 0：不换栈
   x86_set_vector_ist(arch::IrqVec::Nmi, NMI_STACK);                    // ist = 1
   x86_set_vector_ist(arch::IrqVec::MachineCheck, MACHINE_CHECK_STACK); // ist = 3
   ```

   `ist = 0` 在架构上的意思是"不换栈"，所以 #DF 恰恰是最需要换栈、却唯一不换的那个。

2. **TSS 的槽从来没填。** `x86_setup_tss_percpu()` 只做了 `tss->init()`（I/O 位图），全树没有一处写 `ist1..ist7`；
   `X86PerCpu::intr_stack[]` 声明了 64 KB 却没人用：

   ```cpp
   auto tss = &mm::CpuLocal::access(X86_PCPU, cpunum).tss;
   tss->init();
   // GDT 描述符、ltr —— 没有任何 IST 赋值
   ```

   于是 NMI 进门时 %rsp = 0，CPU 压第一个字就落在 `0xfffffffffffffff8`，缺页。

**为什么表现成"杀掉 shell"，而不是内核 panic**：这个 #PF 发生在 NMI 的*投递过程中*，架构规定它报告的是被打断的现场。
被打断的是 ring 3 的 shell，于是 #PF 帧里 CS 是用户态、rip 是 shell 的 `0x202e42`，`is_from_uspace()` 为真，
缺页地址在内核半区解析不了，`FatalPageFault` 交给用户异常路径，进程被杀。内核自己的配置错误被记到了一个无辜的程序头上，
日志里完全看不出和 NMI 有关——比 panic 更难查。

**改法**
改后：

```cpp
constexpr auto ist_slot(IntrStackOrdering which) -> u8 {
    return static_cast<u8>(which + 1);
}
...
x86_set_vector_ist(arch::IrqVec::DoubleFault, ist_slot(DOUBLE_FAULT_STACK));
x86_set_vector_ist(arch::IrqVec::Nmi, ist_slot(NMI_STACK));
x86_set_vector_ist(arch::IrqVec::MachineCheck, ist_slot(MACHINE_CHECK_STACK));
...
auto &pcpu = mm::CpuLocal::access(X86_PCPU, cpunum);
auto tss = &pcpu.tss;
tss->init();
for (auto i = 0; i < MAX_NUM_STACKS; ++i) {
    auto const which = static_cast<IntrStackOrdering>(i);
    tss->set_ist(ist_slot(which), pcpu.intr_stack[which].top());
}
```

- 两套编号只在 `ist_slot()` 一处相遇，注释写明为什么差一。被否掉的方案：把枚举改成从 1 开始——那样 `intr_stack[]` 的下标处处要减一，差一错误只是换了个地方。
- `ist1..ist7` 改成数组，是为了能按槽号写；布局不变，用 `offsetof(X86Tss, ist) == 0x24` 钉住。
- 栈取自与 TSS 同一个 `X86_PCPU`，所以 BSP 在 `init_arch()` 里第二次调用 `x86_setup_tss_percpu()`（CpuLocal 搬家之后）时，IST 也随之指向自己的块，而不是模板。
- `DEBUG_BREAK_STACK` 也填进了 TSS，但没有门用它：`deliver_user_exception()` 依赖异常帧在线程内核栈上，#DB 换到 IST 会破坏这一点。Zircon 同样只给 NMI、#MC、#DF 配 IST。
- 注释里写明了 IST 带来的两条限制（都和 Zircon 相同）：没有保护页，溢出会写进相邻的 per-CPU 数据；每次进门都从栈顶开始，所以 NMI/#MC 处理路径不能出错——否则内层 `iretq` 解除 NMI 屏蔽，下一个 NMI 会压在上一个的帧上（Linux `entry_64.S` 为此有一整套嵌套 NMI 协议）。

**参照**
- `Zircon x86_tss_assign_ists()` / `x86_initialize_percpu_tss()`（`kernel/arch/x86/descriptor.cc`）：`tss->ist1 = &percpu->interrupt_stacks.nmi + 1` 等，取的是栈的高端。
- `Zircon x86_init_percpu()`（`kernel/arch/x86/mp.cc`）：`idt_set_ist_index(&_idt, X86_INT_NMI, NMI_IST_INDEX)`，`NMI_IST_INDEX` 等从 1 开始（`include/arch/x86.h`）。
- Intel SDM Vol. 3A 6.14.5 "Interrupt Stack Table"。

**验证**
- `prebuilt/ninja/ninja -C out/bringup.x86` 通过；clang-format 已跑；clang-tidy 对 `descriptor.cpp`、`idt.cpp`（连带 `percpu.hpp`）在改动行上无新告警。
两个镜像启动：`kernel.dev.obi` 有 `init: shell says "pong"`、`shell: channel checks passed`、`shell: bad-pointer checks passed`、`--- end ---`；
  `kernel.dev.dbg_1.obi` 有后两行和 `--- end ---`，无断言、无 panic（`fault_probe` 被杀是它自带的故障自测）。
- 同一个 NMI 实验，改前改后对照（启动完成后在 QEMU monitor 打 `nmi`）：

  | | 改前 | 改后 |
  |---|---|---|
  | dev | `terminating process 'shell' ... FatalPageFault ... cr2=0xfffffffffffffff8` | `[... cpu0 info] #NMI`，无进程被杀 |
  | dbg_1 | 未测 | `[... cpu0 info] #NMI`，无断言、无 panic |

  两次都只在 cpu0 上看到 NMI。改前那次 cpu0 正跑在 shell 的 ring 3 里；改后那次时间更晚、shell 已跑完，cpu0 多半在内核里。
  落点不同，但改前无论哪种落点，NMI 都压不下自己的帧；改后它换到了 IST 栈上，处理完并返回。
- #DF 的栈溢出路径未实测。

**故意没做**
- `syscall.S` 的 swapgs 窗口（NMI/#MC 落在 `syscall` 与 `swapgs` 之间、或出口 `swapgs` 与 `sysretq` 之间时，`interrupt_common` 按 CS 判断不做 swapgs，处理函数拿着用户的 GS 基址去读写 per-CPU 数据）——IST 只解决了栈，没解决 GS。仍留在 `docs/repairs/260923/x86-kernel.md`。
- IST 栈没有保护页、NMI 处理路径没有嵌套保护：见上，都和 Zircon 一致，等真有需要时再做。

## NMI、#DF、#MC 不再按 CS 决定 %gs：走 paranoid_common，由 TR 认出 CPU（2026-09-25）

**文件**
- `kernel/arch/x86/kernel/idt.S` — 汇编期把向量 2、8、18 的 stub 改为跳 `paranoid_common`，其余照旧跳 `interrupt_common`
- `kernel/arch/x86/kernel/exception.S` — 新增 `paranoid_common`：保存寄存器后直接调 C++，不做按 CS 的 swapgs
- `kernel/arch/x86/kernel/faults.cpp` — 新增 `x86_handle_paranoid_exception()`；`arch_handle_exception()` 里原来给这三个向量的早退分支改为断言它们不会来
- `kernel/arch/x86/kernel/descriptor.cpp` — `Gdt::cpu_of_tss_selector()`（`tss_selector()` 的逆）与 `x86_cpu_num_from_tr()`
- `kernel/arch/x86/lib/arch/include/arch/x86/descriptor.hpp` — `arch::str()`
- `kernel/arch/x86/kernel/include/ours/arch/x86/descriptor.hpp` — 声明 `x86_cpu_num_from_tr()`

**症状**
潜伏，但能确定地复现。窗口本身只有几条指令，靠 QEMU monitor 的 `nmi` 撞不上，所以用了一个临时实验（未提交，已还原）：
在一个 `InitLevel::Platform` 钩子里关中断，把 `IA32_GS_BASE` 换成 `0xdead0000`，给自己发一个 NMI（ICR，delivery mode NMI），
自旋等它到达，再把 GS_BASE 写回。这正是窗口里的状态：CS 是 ring 0，%gs.base 不是内核的。

改前，两个镜像都停在这一行之后，再无任何输出，没有 panic、没有诊断：

```
[3614891000 cpu0 info] EXPERIMENT: self-NMI with a foreign %gs.base (kernel's is 0xffffffffbff3c000)
```

**原因**
`interrupt_common` 只看压栈的 CS 决定要不要 swapgs：

```asm
    testb $3, X86_IRQFRAME_OFFSET_CS(%rsp)
    jz 1f
    xorl %ebp, %ebp
    swapgs
1:
    lfence
    call arch_handle_exception
```

这对"在 swapgs 成对指令中间不可能发生"的向量是对的。IA32_FMASK 清了 IF 和 TF，所以普通中断和单步确实进不了那些窗口；
但 NMI 和 #MC 屏蔽不了。窗口有这几处：
- `syscall.S` 进门：`syscall` 已把 CPL 变成 0，到第一条 `swapgs` 之前 GS 还是用户的；
- `syscall.S` 出门：`swapgs` 之后到 `sysretq` 完成之前；
- `interrupt_common` 自己：从 ring 3 进中断到它那条 `swapgs` 之间，以及出口 `swapgs` 到 `iretq` 之间；`uspace.S` 第一次进 ring 3 同理。

落进窗口的 NMI 看到 CS 是内核，就不 swapgs，于是处理函数里所有 `%gs:` 相对访问——`CpuLocal`、`Thread::Current`、logz4 的 CPU 号——
都落在用户选的地址上。CR4.FSGSBASE 在支持的 CPU 上是开的，用户态一条 `wrgsbase` 就能把它设成任意值：
没有 SMAP 时内核会把用户页当成自己的 per-CPU 块读（伪造的"当前线程"指针），有 SMAP 或 GS 为 0 时是一次 ring 0 碰用户地址的 panic；
实验里的 `0xdead0000` 落在未映射的内核地址上，第一次访问就缺页，缺页处理本身又要用 %gs，于是一路错下去，连 panic 都打不出来。

窗口是关不掉的：`syscall` 与 `swapgs` 不能合成一条，NMI 又屏蔽不了。所以要改的是这三个向量的入口：不去猜 %gs，而是自己确定。

**改法**
- `idt.S` 在汇编期分流，与它判断"哪些向量带错误码"用的是同一种写法：

  ```asm
        isr.current.is_paranoid = isr.current == 2 || isr.current == 8 || \
                                  isr.current == 18
     .if isr.current.is_paranoid
        jmp paranoid_common
     .else
        jmp interrupt_common
     .endif
  ```

  其余 253 个 stub 运行时不多付任何代价。选这三个，是因为它们恰好就是配了 IST 的三个，理由相同：被打断的上下文什么都不能假设。
- `paranoid_common` 保存寄存器后直接 `call x86_handle_paranoid_exception`，没有按 CS 的 swapgs，也没有 `lfence`
  （那条 lfence 防的是条件 swapgs 的投机执行；这里没有条件 swapgs，改 %gs 的 `wrmsr` 本身是串行化指令）。
- C++ 入口：

  ```cpp
  extern "C" auto x86_handle_paranoid_exception(arch::IrqFrame *frame) -> void {
      mm::CpuLocal::ScopedInstall const kernel_gs{x86_cpu_num_from_tr()};

      task::ExceptionContext unused{};
      static_cast<void>(x86_dispatch_exception(frame->vector, frame, &unused));
  }
  ```

  "我是哪颗 CPU"从 TR 得到：每颗 CPU 在 `x86_setup_tss_percpu()` 里 `ltr` 过自己的 TSS 选择子，`ltr` 只有 ring 0 能执行，用户改不了。
  选择子就是描述符在 GDT 里的字节偏移，TSS 描述符按 CPU 号排在 7 个固定描述符之后，每个 16 字节，
  所以 `cpu = (选择子 - 0x38) / 16`——这是 `tss_selector()` 倒过来，两者放在一起，布局只写在一处。
  "这颗 CPU 该装什么"交给 `CpuLocal::ScopedInstall`（见 `mm-cpu-local.md` 同日条目），汇编与 x86 都不碰 `CPU_OFFSETS`。
- `ScopedInstall` 构造完成之前，这个函数不能碰任何 per-CPU 数据。这之所以成立，是因为内核以 `-fno-stack-protector` 编译、UBSan 用 trap 模式，
  编译器不会在背后插入 `%gs` 访问。将来若加入基于 %gs 的栈保护或函数入口跟踪钩子，这里和它先调用的两个函数都要豁免——Linux 为此把对应代码标为 `noinstr`。注释里写明了。
- 原来 `arch_handle_exception()` 里给这三个向量的早退分支（及其说明"为什么不做簿记"）整体挪到了新函数上；原处改为断言它们不会来，
  对应 Zircon `x86_exception_handler()` 里的 `DEBUG_ASSERT(frame->vector != X86_INT_NMI)`。
- 被否掉的方案：
  - 在 `interrupt_common` 里运行时比较向量号：每个中断都多付一次比较；汇编期分流是零成本的。
  - 从 IST 栈地址反推 per-CPU 基址（Zircon 的做法），或把 GS 值缓存在 IST 栈顶：依赖栈布局；缓存还是 `CPU_OFFSETS` 的副本，会过期。
  - RDPID：要 CPU 支持，默认的 Nehalem 没有，而且还得有人把 CPU 号写进 `IA32_TSC_AUX`。

**参照**
- `Zircon interrupt_common` 的 `.Lnmi` 分支与 `x86_nmi_handler()`（`kernel/arch/x86/exceptions.S`、`exceptions_c.cc`）：NMI 不按 CS 判断，rdmsr/wrmsr `MSR_IA32_GS_BASE` 存—设—还原。
- `Linux paranoid_entry` / `paranoid_exit`（`arch/x86/entry/entry_64.S`）；CPU 号取法 `LOAD_CPU_AND_NODE_SEG_LIMIT` / `GET_PERCPU_BASE`（`arch/x86/entry/calling.h`）——同样是从一个用户改不了的描述符里读出 CPU 号。

**验证**
- `prebuilt/ninja/ninja -C out/bringup.x86` 通过；clang-format 已跑；clang-tidy 对 `faults.cpp`、`descriptor.cpp`、`cpu_local.cpp`（连带各头文件）在改动行上无新告警
  （`cpu_of_tss_selector()` 起初缺 `[[nodiscard]]`，已补）。
- 同一个实验，改后两个镜像都活下来：

  ```
  [3528542000 cpu0 info] EXPERIMENT: self-NMI with a foreign %gs.base (kernel's is 0xffffffffbff3c000)
  [3529295000 cpu0 info] #NMI
  [3545827000 cpu0 info] EXPERIMENT: back on the kernel's %gs.base
  ```

  `#NMI` 这一行本身就要经 %gs 读 CPU 号和当前线程，它打得出来、并且标的是 cpu0，说明 TR 认对了 CPU、`ScopedInstall` 装对了值；
  最后一行说明返回时外来的 GS 被原样还原（实验随后自己把内核的值写回）。之后照常跑完：dev 有 `init: shell says "pong"`、`shell: channel checks passed`、
  `shell: bad-pointer checks passed`、`--- end ---`；dbg_1 有后两行和 `--- end ---`，无断言。
- 撤掉实验后：两个镜像正常启动到 `--- end ---`；启动完成后 QEMU monitor 打 `nmi`，两个镜像都打出 `[... cpu0 info] #NMI` 并继续运行。
- 未实测：NMI 落在 ring 3 时经新路径返回（存下的是用户的 GS、返回时写回）。机制与实验相同，但这次 monitor 的 NMI 落点在内核里。#MC、#DF 未实测。

**故意没做**
- NMI 处理路径里还会打日志，日志要拿锁；NMI 打断一个正持有日志锁的 CPU 会死锁。这是另一个问题（Linux 用 NMI-safe 的 printk 缓冲解决），没有一并改。
- `sysretq` 返回非规范 RCX 时在 ring 0 触发 #GP（此时栈和 GS 都已是用户的，CVE-2012-0217）：ours 目前 RCX 只可能是 `syscall` 自己存下的返回地址，触发不到；等有了修改线程寄存器的接口再加检查，已记入 repairs。
