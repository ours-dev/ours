# x86-apic 改动记录

## 每颗 CPU 都要自己把 local APIC 软件使能（SVR bit 8）（2026-09-09）

**文件**
- `kernel/arch/x86/lib/arch/include/arch/x86/apic/xapic.hpp` — 新增 `XApic::enable()`
- `kernel/arch/x86/apic/local_apic.cpp` — `init_local_apic_percpu()` 里调它

**症状**
AP 全部起来了、进了 idle 循环、调度器也把线程排进了它们的运行队列，可它们**一个中断
都收不到**：没有 IPI，没有 APIC timer，`sti; hlt` 之后再也没醒过来。BSP 一切正常。

```
[... cpu0 info] XXX send_resched targets=2      <- IPI 确实发出去了
[... cpu0 info] XXX send_resched targets=4
[... cpu0 info] XXX send_resched targets=8
                                                <- CPU1/2/3 没有任何 "IPI-Resched"
```

**原因**
把 SVR 打出来，一眼就清楚了：

```
[3940678000 cpu0 info] apicbase=FEE00900 svr=10F     <- BSP，bit 8 = 1
[4200339000 cpu1 info] apicbase=FEE00800 svr=FF      <- AP，bit 8 = 0
[4209881000 cpu2 info] apicbase=FEE00800 svr=FF
[4222474000 cpu3 info] apicbase=FEE00800 svr=FF
```

local APIC 有**两道**开关，代码只拨了一道：

1. `IA32_APIC_BASE[11]` —— **硬件使能**，`init_local_apic_percpu()` 一直在写；
2. `SVR[8]`（Spurious Interrupt Vector Register 的 APIC Software Enable）——
   **软件使能**，谁也没写过。

SDM 说得很直白：软件禁用状态下的 local APIC **什么都不投递**——IPI 不投递，LVT 不投递，
timer 不投递。而 x86 核心复位（以及 INIT/SIPI 之后）的 SVR 初值就是 `0x0000'00FF`，
bit 8 是 0。

那 BSP 为什么能收中断？**因为固件替它开了**。OVMF 在把控制权交出来之前已经使能了
BSP 的 local APIC，留下 `SVR = 0x10F`。内核一直是在吃这个免费午餐，单核下永远发现不了；
AP 没有固件替它做这件事，于是 SVR 还是 `0xFF`。

这也解释了为什么现象如此"安静"：不是中断被丢了、也不是向量没挂上，是 local APIC
压根不接受投递，CPU 就在 `hlt` 上睡到天荒地老，队列里躺着一个 runnable 线程。

顺带一提，固件挑的 spurious 向量是 `0x0F`，而本内核的是 `IrqVec::ApicSpurious = 0xF0`。
现在两边统一由内核写，不再依赖固件留下什么。

**改法**
```cpp
// xapic.hpp
auto enable(IrqVec spurious_vector) noexcept -> void {
    auto value = read_reg(XApicRegType::SpuriousVector);
    value &= ~VECTOR_MASK;
    value |= static_cast<u32>(spurious_vector) & VECTOR_MASK;
    value |= SOFTWARE_ENABLE;                       // bit 8
    write_reg(XApicRegType::SpuriousVector, value);
}
```
```cpp
// init_local_apic_percpu()，紧跟在写 IA32_APIC_BASE 之后
XAPIC_STORAGE.enable(IrqVec::ApicSpurious);
```

放在 `init_local_apic_percpu()` 而不是只给 AP 一条特判：BSP 现在这一位是对的纯属侥幸，
内核不该把自己的中断能不能来这件事托付给固件。Linux 的 `setup_local_APIC()` 也是每颗
CPU 都写，不区分 BSP。

**参照**
- Linux `setup_local_APIC()`（`arch/x86/kernel/apic/apic.c`）：
  `value = APIC_SPIV_APIC_ENABLED | SPURIOUS_APIC_VECTOR; apic_write(APIC_SPIV, value);`
- Intel SDM Vol.3 §12.4.3「Enabling or Disabling the Local APIC」——软件禁用时
  「the local APIC will respond to no interrupts」。

**验证**
`SMP=4` 实测，两个镜像都跑：

| CPU | 修改前日志行数 | 修改后日志行数 |
|-----|---------------|---------------|
| 0   | 8600          | 8589          |
| 1   | 1（只有 "is online"）| 648（userboot 在这上面跑） |
| 2   | 1             | 98552（Sample1） |
| 3   | 1             | 98500（Sample2） |

`kernel.dev.obi`（断言编译掉）同样起到 `userboot: hello from ring 3`，而且这一行来自
`cpu1`。`SMP=1` 行为不变。

**故意没做**
- 没有屏蔽 AP 的 LINT0/LINT1。Linux 只让 BSP 的 LINT0 走 ExtINT，AP 上两条都屏蔽；
  QEMU 的 q35 + IOAPIC 下不构成问题，真机上要补。记在
  `docs/repairs/260909/x86-apic.md`。
- 没有写 TPR。复位值是 0（接受全部优先级），够用。
