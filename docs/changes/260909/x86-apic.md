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

## APIC 寄存器的读-改-写收成一个 `update_reg()`（2026-09-10）

**文件**
- `kernel/arch/x86/lib/arch/include/arch/x86/apic/xapic.hpp`

**症状**
不是 bug。昨天新加的 `enable()` 是这个类里第**五**份同样的读-改-写：

```cpp
auto value = read_reg(XApicRegType::SpuriousVector);
value &= ~VECTOR_MASK;
value |= static_cast<u32>(spurious_vector) & VECTOR_MASK;
value |= SOFTWARE_ENABLE;
write_reg(XApicRegType::SpuriousVector, value);
```

`enable_tsc()`、`disable_tsc()`、`mask()`、`unmask()` 是同一个形状。

**原因**
APIC 寄存器本来就长这样：一条 LVT 表项里，向量号和模式位、屏蔽位挤在同一个 32 位字里，
所以**任何**写入都必须先读回来、保住不该动的位。这是这个类的基本操作，
不是某个方法的实现细节，把它写五遍等于把"先清后置"这个顺序也复述五遍——
而顺序是有意义的：同一位既在 `clear` 又在 `set` 里，含义是"置上"。

**改法**
```cpp
auto update_reg(XApicRegType type, u32 clear, u32 set) noexcept -> void {
    write_reg(type, (read_reg(type) & ~clear) | set);
}
```
五个调用点各缩成一句，语义逐位对照过，没有变化。例如：

```cpp
auto enable_tsc(IrqVec vector, TscMode mode = TscMode::OneShot) noexcept -> void {
    update_reg(XApicRegType::LvtTimer, VECTOR_MASK | TIMER_MODE_MASK | INTERRUPT_MASK,
               (static_cast<u32>(vector) & VECTOR_MASK) | (static_cast<u32>(mode) << 17));
}
```

**参照**
Linux 的 `apic_write(reg, (apic_read(reg) & ~mask) | bits)` 在
`arch/x86/kernel/apic/apic.c` 里到处是同一个形状；这里只是给它一个名字。

**验证**
四核引导：APIC timer 正常标定（1000005061 Hz），三颗 AP 上线，
cpu0 8589 / cpu1 597 / cpu2 85684 / cpu3 85631，无断言失败。
`enable_tsc`／`disable_tsc` 走的是 LVT Timer，标定那行日志能出来就说明它们没坏。

## ICR 的空闲等待挪到写之前，并去掉自旋里那次 ESR 读（2026-09-10）

**文件**
- `kernel/arch/x86/lib/arch/include/arch/x86/apic/xapic.hpp` — `send_ipi()` 与 `wait_icr_idle()`

**症状**
不是 bug，是发送方白等。多核打通之后每一次跨 CPU 唤醒都要走这条路，
所以它从"没人跑"变成了热路径。

**背景：ICR 是什么**
Interrupt Command Register，local APIC 用来**发 IPI** 的寄存器。xAPIC 下它是一对：

| 偏移 | 内容 |
|------|------|
| 0x310 `IcrHigh` | 目标 APIC ID（物理寻址模式下） |
| 0x300 `IcrLow`  | 向量号、投递模式（Fixed / INIT / Startup…）、目标简写 |

**写 `IcrLow` 那一刻 IPI 才真正发出去**，所以顺序必须是先高后低；两次写之间也必须关中断，
否则中断处理里如果自己也发一发 IPI，就会把 `IcrHigh` 覆盖掉，
这一发就寄到别的 CPU 去了。

`IcrLow` 的 bit 12 是 **Delivery Status**（只读）：0 = 空闲，
1 = 本 APIC 上一发 IPI 还没被目标接受。

**原因**
硬件的规则是「**下一次写 ICR 之前**它必须是空闲的」——不是「发送方必须目睹自己这一发送达」。
原来的代码等在写**之后**：

```cpp
auto send_ipi(IpiRequest const &request) noexcept -> void {
    AutoInterruptDisabler guard;
    write_reg(XApicRegType::IcrHigh, request.icr_high());
    write_reg(XApicRegType::IcrLow, request.icr_low());
    wait_for_ipi();                  // <- 在这儿自旋等它送达
}
```

于是发送方替一件与自己无关的事付了延迟：这条消息是给**别的** CPU 的，
本 CPU 却在这儿转圈。而这条路径现在是 `Scheduler::reschedule_mask()` →
`IpiService::send_resched()` → 每一次跨 CPU unpark 都要走的。

挪到写之前，两种情形都变好：

- **单发**（常见情形，`reschedule_mask(CpuMask::of(target))`）：发完就走，
  等下一次再来时 ICR 早就空了，那次检查是免费的；
- **连发**：该等的时候照样等——而这正是规则真正针对的唯一情形。

等待必须**放在关中断的作用域里面**，不能放到 `AutoInterruptDisabler` 之前：
否则中断可以插在「等完」和「开始写」之间发它自己的 IPI，把 ICR 重新弄忙。

**顺带：自旋循环里那次 ESR 读**

```cpp
while ((read_reg(IcrLow) & (1 << 12)) != 0 && read_reg(Esr) == 0) {}
```

这个「兼作错误逃生口」的写法两头不讨好：

1. **它发现不了错误**。ESR（Error Status Register）只有在**写一次之后**才会把新的
   错误锁存进去，裸读拿到的是上一次错误留下的陈旧值，跟这一发无关。
2. **它会读到陈旧的非零值**，于是提前跳出循环——让下一次写去覆盖一个**还忙着**的 ICR，
   而这恰恰是这段等待存在的唯一目的。

再加上每轮多一次非缓存 MMIO 读，把自旋的总线流量翻倍。去掉之后循环只读
delivery-status 一个位，并且加上 `arch::spin_hint()`（PAUSE）。

**改法**
```cpp
auto send_ipi(IpiRequest const &request) noexcept -> void {
    AutoInterruptDisabler guard;
    wait_icr_idle();                                        // 先等
    write_reg(XApicRegType::IcrHigh, request.icr_high());
    write_reg(XApicRegType::IcrLow, request.icr_low());     // 再发
}

auto wait_icr_idle() const noexcept -> void {
    while ((read_reg(XApicRegType::IcrLow) & ICR_DELIVERY_STATUS_PENDING) != 0) {
        spin_hint();
    }
}
```

`wait_for_ipi` 改名成 `wait_icr_idle`——它等的是 ICR 空闲，不是「某一发 IPI」；
`1 << 12` 给了名字 `ICR_DELIVERY_STATUS_PENDING`。

**参照**
- Linux `__default_send_IPI_dest_field()`（`arch/x86/kernel/apic/ipi.c`）：
  注释就写着 `/* Wait for idle. */`，然后才写 ICR2、写 ICR。
- Linux `__xapic_wait_icr_idle()`：`while (native_apic_mem_read(APIC_ICR) & APIC_ICR_BUSY) cpu_relax();`
  ——同样只看 busy 位，同样带 PAUSE，同样不看 ESR。
- Intel SDM Vol.3 §11.6.1，ICR 的 Delivery Status 位定义。

**验证**
三种配置都实测过：

| 配置 | 结果 |
|------|------|
| `kernel.dev.dbg_1.obi` 四核 | cpu0 8589 / cpu1 601 / cpu2 85293 / cpu3 85290，零断言失败 |
| `kernel.dev.obi` 四核 | 三颗 AP 上线，userboot 在 cpu1 进 ring 3 |
| `kernel.dev.dbg_1.obi` 单核 | 260694 行，零断言失败 |

AP 唤醒本身（INIT / Startup IPI）走的就是这个 `send_ipi()`，三颗 AP 照常上线，
说明 INIT-SIPI 序列没有被影响。

**故意没做**
- **没有改 `x86_bring_aps()` 的时序**。Linux 在 AP 唤醒路径上是发完再等
  （`safe_apic_wait_icr_idle()`），因为 INIT→10 ms→SIPI 的间隔是从**送达**开始算的。
  这里不需要：`x86_bring_aps()` 自己用 `sleep_for(Milliseconds(10))` 和
  `sleep_for(Milliseconds(1))` 显式定时，比 ICR 的忙时间长几个数量级。
- **没有加带上界的等待**。Linux 有 `safe_apic_wait_icr_idle()`（1000 次上限），
  给 NMI 和 CPU 上线路径用，免得一个卡死的 ICR 把发送方一起拖死。
  这里还没有这个变体，记在 `docs/repairs/260909/x86-apic.md`。
- **没有做多播的批处理**。`apic_send_ipi_mulitcast()` 仍然是逐个目标调
  `apic_send_ipi_unicast()`，每次重建一份 `IpiRequest`、各自关一次中断，
  而其实只有 `IcrHigh` 随目标变。那是 `ipi.cpp` 的事，单独一条留在待修里。
