# 内核开发的常见坑

这里收的每一个问题都在 ours 里真实出现过，都有出处（提交、`docs/changes` 里的改动记录、或代码里记下来龙去脉的注释）。
目的不是背结论，而是学会**怎么发现、怎么证明、怎么修对**这类问题——同一类坑会换个样子反复出现，
方法比结论活得久。

读的顺序：

1. 先读下面的**术语速查**，把几个反复出现的词弄清楚。
2. 再读**通用方法**——这是本文的主体。
3. 然后拿着手上的症状去**索引**里找，顺着链接读详细记录。

每个问题的背景、根因、改前改后的代码、Linux / Zircon 的对应函数和验证过程，都写在索引链接到的地方：
`docs/changes` 下的改动记录（格式见 [TEMPLATE](../../changes/TEMPLATE.md)），或代码里记下来龙去脉的注释。

---

## 术语速查

| 术语 | 一句话解释 |
|---|---|
| ring / CPL | CPU 当前的特权级。ring 0 = 内核，ring 3 = 用户。CPL（Current Privilege Level）就是这个数 |
| 系统调用 `syscall` / `sysretq` | 用户进内核、内核回用户的"快速通道"指令对。只切特权级，**不换栈**、**不换 GS** |
| 中断门 / `iretq` | 中断、异常进内核走 IDT 里的"门"；回去用 `iretq`。从 ring 3 进来时 CPU 会自动换到内核栈 |
| `%gs` / GS 基址 | 段寄存器 GS 的基址。内核用它找"本 CPU 的私有数据"（per-CPU）；用户态也有自己的 GS 基址 |
| `swapgs` | 一条指令，把"当前 GS 基址"和 MSR `KERNEL_GS_BASE` 里存的值互换。进出内核各做一次，成对出现 |
| per-CPU 数据 | 每颗 CPU 一份的变量（当前线程、调度队列……）。ours 叫 `CpuLocal`，通过 `%gs` 访问 |
| TSS | 任务状态段。64 位下它只剩两件事：几张栈指针表（RSP0、IST1..7），和 I/O 端口位图 |
| RSP0 | 从 ring 3 进内核时 CPU 自动换上的内核栈指针，存在 TSS 里 |
| IST | Interrupt Stack Table。给个别向量（NMI、#DF、#MC）配的专用栈，**不管从哪里进来都强制换栈** |
| NMI | 不可屏蔽中断。`cli` 挡不住它，能落在任何一条指令上 |
| #DF / #MC / #GP / #PF | 双重故障 / 机器检查 / 一般保护故障 / 缺页 |
| fault 与 trap | fault 压栈的是**出错那条**指令的地址（返回会重执行），trap 压的是**下一条** |
| 规范地址（canonical） | x86-64 只用 48 位地址，第 47–63 位必须全等。中间那段是"空洞"，碰了就 #GP |
| QEMU TCG | QEMU 的纯软件模拟模式。它**不保证**和某款真实 CPU 的每个细节一致 |

---

## 通用方法

下面每条方法都附了"在哪些条目里用过"，读条目时可以回来对照。

### M1 给指令序列画"逐条状态表"

**什么时候用**：一段代码在改一组"必须一起变"的状态（栈、GS、特权级、中断开关……），中途被打断会出事。

**怎么做**：每行一条指令，每列一个状态，把每条指令**执行完之后**的状态填进去。不一致的行就是"窗口"。

```
指令          %rsp      %gs     CPL   IF
pop %rcx      内核栈    内核     0     0
pop %rsp      用户栈    内核     0     0    ← 栈已经是用户的
swapgs        用户栈    用户     0     0    ← GS 也是用户的，但 CPL 还是 0
sysretq       用户栈    用户     3     1
```

表画出来，问题就从"感觉哪里不对"变成"第 2、3 行的 CPL 和栈/GS 不一致"。
用过：[docs/changes/260925/x86-kernel.md](../../changes/260925/x86-kernel.md) 第 2、4 条。

### M2 问"谁能打断这里"

**什么时候用**：找到一个窗口之后，判断它是不是真的危险。

**怎么做**：把所有可能在窗口里发生的事件列成表，逐个看当时的屏蔽条件。

| 事件 | 在 `syscall` 之后的窗口里能发生吗 | 为什么 |
|---|---|---|
| 普通外部中断 | 不能 | IA32_FMASK 在 `syscall` 时清了 IF |
| 单步 #DB | 不能 | FMASK 也清了 TF |
| NMI | **能** | 不可屏蔽 |
| #MC | **能** | 硬件错误，随时 |
| 同步异常（#PF 等） | 看窗口里的指令会不会出错 | 例如 `sysretq` 本身就可能 #GP |

结论不是"有窗口就危险"，而是"只有 NMI、#MC 和 `sysretq` 自己的 #GP 能进来"——修法于是只需针对这三样。
用过：[docs/changes/260925/x86-kernel.md](../../changes/260925/x86-kernel.md) 第 2、4 条。

### M3 找写入点：一个状态只该有一个来源

**什么时候用**：一个标志"好像不起作用"，或者两处代码都在回答同一个问题。

**怎么做**：

```bash
grep -rn "IrqNestedCounter" kernel --include='*.hpp' --include='*.cpp' --include='*.S'
```

把结果分成"读"和"写"两堆。**只有读没有写**的字段就是死的，读它的逻辑全部是空转；
**两个地方都能回答同一个问题**（比如"是否在中断里"）时，要么删掉一个，要么让一个从另一个推出来——否则迟早漂移。
用过：[changes/260924/kernel-task.md](../../changes/260924/kernel-task.md) 末两条（`IrqNestedCounter`、`NmiNestedCounter`）。

### M4 在唯一的收口处设防

**什么时候用**：一个值必须满足某个条件（入口地址必须是用户地址……），而它有好几个来源。

**怎么做**：找所有来源都必须经过的那个函数（"收口"），在那里检查；再在"最后一刻"放一个全构建都开的 `ASSERT` 兜底。
不要在每个调用者里各查一遍——新来的调用者会忘。
用过：[docs/changes/260925/x86-kernel.md](../../changes/260925/x86-kernel.md) 第 4 条与同日 `object.md`（`ThreadDispatcher::start()` + `arch_enter_uspace()`）。

### M5 把"罕见的时机"变成"必然的状态"：确定性实验

**什么时候用**：bug 依赖一个很难撞上的时机（NMI 恰好落在两条指令之间）。

**怎么做**：不去撞时机，而是**直接把机器置于那个时机下的状态**，再触发事件。例如：
"NMI 落在 swapgs 窗口里"的本质是"CS 是内核的，GS 不是内核的"，那就在内核里关中断、把 GS_BASE 写成一个外来值、给自己发一个 NMI。

同一个实验**改前跑一次、改后跑一次**，两次的日志并排放进改动记录。实验代码不提交，改动记录里写明"临时实验，未提交，已还原"。
用过：[docs/changes/260925/x86-kernel.md](../../changes/260925/x86-kernel.md) 第 2、4 条。

### M6 故障注入：让模拟器替你制造事件

QEMU 的 monitor 能直接注入 NMI、查看每颗 CPU 的寄存器。一条 `nmi` 就发现了 IST 从没配置过。
用过：[changes/260925/x86-kernel.md](../../changes/260925/x86-kernel.md) 第 1 条。

### M7 读故障报告时，先怀疑"被指控的一方"

**什么时候用**：内核说"某个用户进程出错了"，但那个进程什么都没做错。

**怎么做**：逐个字段检查报告是否自相矛盾。例如：

```
terminating process 'shell' ... FatalPageFault error_code=0x2 rip=0x202e42 cr2=0xfffffffffffffff8
```

- `error_code=0x2`：第 2 位（U/S）是 0，说明这次访问是**内核态**发起的——用户程序的错不会是这样；
- `cr2=0xfffffffffffffff8`：等于 `0 - 8`，是"有人在 `%rsp = 0` 的时候压了一个 8 字节的值"的指纹；
- 两者合起来：是 CPU 在给某个事件压栈时出错了，报告里的 `rip` 只是被打断的那个用户程序。

用过：[changes/260925/x86-kernel.md](../../changes/260925/x86-kernel.md) 第 1 条。

### M8 对照参考实现

ours 大体是 Zircon 的重新实现，线程锁等少数部分照 Linux。本机有两份源码：
`../fuchsia/zircon/kernel` 和 `../linux`。遇到问题先找对应物：

```bash
# 找 Zircon 在中断入口做了什么
grep -rn "int_handler_start\|arch_blocking_disallowed" ../fuchsia/zircon/kernel --include='*.h' --include='*.cc'
# 找 Linux 在 sysret 前检查了什么
grep -n "canonical" ../linux/arch/x86/entry/syscall_64.c
```

对照不是照抄：要看清楚对方**为什么**那样做（Linux 的"GS 为负就是内核的"约定在 ours 不成立，因为 ours 的 GS 装的是偏移不是地址）。
Zircon 的 `docs/concepts/kernel/` 下有不少这类设计说明，比代码更好读。几乎每条都用过。

### M9 知道模拟器在哪里会骗你

QEMU TCG 按 AMD 的方式处理 `iretq` 到非规范地址（在 ring 3 报 #GP），Intel 真机则在 ring 0 报。
同一个 bug，在 QEMU 上只是"杀了一个进程"，在 Intel 真机上是"用户接管内核"。
**实验结果只能证明模拟器上的行为**；依赖具体 CPU 行为的结论，要么在真机上验证，要么在改动记录里明写"未实测"。
用过：[docs/changes/260925/x86-kernel.md](../../changes/260925/x86-kernel.md) 第 4 条。

### M10 两个镜像都要跑

`kernel.dev.dbg_1.obi` 开着 `DEBUG_ASSERT`，遇到第一个断言就停；`kernel.dev.obi` 断言被编译掉，会继续跑下去然后在别处崩。
两个停在同一个地方，才说明没有什么被断言掩盖。细节见 [../debugging.md](../debugging.md)。

### M11 注意"第一次走到"的路径

很多 bug 潜伏到某条路径**第一次被走到**才暴露：第一次进 ring 3、第一次有第二颗 CPU、第一次来 NMI。
在那之前"一切正常"不说明代码对，只说明没人走过。写完一条新路径，要主动构造它第一次被走到的情况。
例子：用户段选择子少了 RPL 位、`iret` 写成 32 位形式、BSP 的 TSS 指向模板——都是第一次进 ring 3 才炸。

### M12 一次只改一件事

内核里一个 bug 经常掩盖另一个（`clac` 在无 SMAP 的 CPU 上 #UD，把每个异常都变成三重错误，真正的 #GP 被盖住）。
一次改一件、改完就构建启动验证、马上提交，才分得清哪个改动起了什么作用。

---

## 索引

| 症状关键词 | 问题 | 背景、根因与修法在哪里 |
|---|---|---|
| NMI 后用户进程被莫名杀掉，cr2 = `0xff…f8` | IST 从未配置 | [changes/260925/x86-kernel.md](../../changes/260925/x86-kernel.md) 第 1 条 |
| 栈溢出时直接复位，没有任何输出 | IST 编号差一，#DF 不换栈 | [changes/260925/x86-kernel.md](../../changes/260925/x86-kernel.md) 第 1 条 |
| 栈指针指到栈的底部 | `top()`/`bottom()` 语义颠倒 | [changes/260925/x86-kernel.md](../../changes/260925/x86-kernel.md) 第 1 条；提交 `81a5ab7f` |
| 第一次进 ring 3 后的第一个中断三重错误 | BSP 的 TSS 指向 per-CPU 模板 | `init_arch()` 的注释，[kernel/arch/x86/kernel/init.cpp](../../../kernel/arch/x86/kernel/init.cpp) |
| 用户程序能直接读写 I/O 端口 | TSS 的 I/O 位图全零 | `X86Tss::init()` 的注释，[percpu.hpp](../../../kernel/arch/x86/kernel/include/ours/arch/x86/percpu.hpp) |
| 偶发整机卡死，NMI 相关 | NMI 里打日志拿锁 | [changes/260925/x86-kernel.md](../../changes/260925/x86-kernel.md) 第 3 条 |
| 某个"保护"好像从来没生效 | 只读不写的状态字段 | [changes/260924/kernel-task.md](../../changes/260924/kernel-task.md) 末两条 |
| 中断处理中途"忘了"自己在中断里 | 嵌套时把共享状态清零而不是还原 | `x86_handle_paranoid_exception()` 的注释，[faults.cpp](../../../kernel/arch/x86/kernel/faults.cpp) |
| 每个 tick 都重新调度 / 抢占两次 | 在中断胶水里直接兑现抢占 | `handle_irq_postamble()` 的注释，[kernel/kernel/irq/mod.cpp](../../../kernel/kernel/irq/mod.cpp) |
| 抢占从来不发生 | "有请求"和"允许抢占"混在一个判断里 | `is_preemption_enabled()` 的注释，[preemption_state.hpp](../../../kernel/include/ours/task/preemption_state.hpp) |
| 同一行异常日志无限刷屏 | fault 返回会重执行 | `x86_dispatch_exception()` 的注释，[faults.cpp](../../../kernel/arch/x86/kernel/faults.cpp) |
| 缺页地址不对 | 没有第一时间读 CR2 | `x86_dispatch_exception()` 的注释，[faults.cpp](../../../kernel/arch/x86/kernel/faults.cpp) |
| per-CPU 变量读出来永远是 0/空指针 | 编译器把地址运算折叠回模板 | `CpuLocal::relocate()` 的注释，[cpu_local.hpp](../../../kernel/mm/include/ours/mm/cpu_local.hpp) |
| 写 per-CPU 变量写坏了别人的内存 | 偏移为 0 落到已回收的模板 | `CpuLocal::offset_of()` 的注释，[cpu_local.hpp](../../../kernel/mm/include/ours/mm/cpu_local.hpp) |
| 计数器的值像垃圾 | 链接器回收了没被 `KEEP` 的段 | [repairs/260903/scheduler.md](../../repairs/260903/scheduler.md) 第 3 条 |
| 内核解引用用户指针却不报错 | 没有 SMAP 时越界访问静默成功 | `x86_handle_page_fault()` 的注释，[kernel/arch/x86/mm/fault.cpp](../../../kernel/arch/x86/mm/fault.cpp) |
