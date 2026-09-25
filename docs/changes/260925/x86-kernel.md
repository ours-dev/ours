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
