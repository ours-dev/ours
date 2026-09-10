# 调试内核

## 一键

用 `ours.code-workspace` 打开这个仓库（`File > Open Workspace from File...`，或者
在 Finder 里双击它），按 **F5**，然后依次回答三个下拉框：**产品 → 架构 → 内核**。

> 必须从这个文件打开：调试配置和它依赖的那几个任务都写在 `ours.code-workspace`
> 里，直接 `Open Folder` 打开目录的话 VS Code 读不到它们，调试面板会是空的。

它会：编译 → 用 QEMU 启动镜像并**停在第一条指令之前** → 挂上 lldb。会话开始时机器
停在复位向量 `0xfff0`（固件都还没跑），这时候把断点打上，再按继续（F5）——**最早期的
启动代码也能断下来**，这正是要 `-S` 的原因。

结束调试会话时 QEMU 会被一并杀掉。

按 F5 会依次问三句，一层比一层窄：

| 顺序 | 问题 | 现在能选 | 决定了什么 |
|---|---|---|---|
| 1 | Product? | `bringup` | 这个产品包含哪些镜像 |
| 2 | Architecture? | `x86`、`arm` | 和产品一起决定构建目录 `out/<产品>.<架构>` |
| 3 | Which kernel? | `dbg.1`、`dbg.0` | 从该产品的镜像里挑一个 |

第三层是 **switchset**（`//build/switch/set/<名字>`），不是镜像名——因为镜像名
（`kernel.dev.dbg_1` / `kernel.dev`）并不说明它们的区别，而 switchset 正好就是区别本身：

| switchset | 是什么 | 实际是哪个镜像 |
|---|---|---|
| `dbg.1` | `KERNEL_DEBUG_LEVEL=1`，`DEBUG_ASSERT` 和 trace/debug 日志都在 | `kernel.dev.dbg_1.obi` |
| `dbg.0` | 断言和 trace 都被编译掉、并且**链进了单测** | `kernel.dev.obi` |

一个 bug 在一个镜像里断言、在另一个里静默，所以两个都要能调。

`arm` 列在菜单里是因为工具链认识它；`out/bringup.arm` 要先跑 `tools/set.py bringup.arm`
才存在，没跑的话脚本会直接把这条命令告诉你。

`fake`（`//kernel/tests/fake-kernel` 那条 phys 链）**不属于任何产品**——它是 `//:default`
的直接依赖——所以三层选择够不着它。何况它现在也起不来：physboot 报
`Invalid kernel package: 4`，见 `docs/repairs/260909/products.md`。

调试面板的下拉框里另有一个 `Kernel: attach to a QEMU I started myself`，用于终端里已经
跑着 `./scripts/qemu_debug.py` 的场合，它不重启 QEMU。

## 三层名单从哪来

**从构建里来，没人手写。**

三层各有出处，都不是这份文档或脚本里的常量：

| 层 | 出处 |
|---|---|
| 产品 | `products/*.gni` |
| 构建目录 | `out/<产品>.<架构>`，就是 `tools/set.py <产品>.<架构>` 写的那个 |
| 该产品有哪些镜像 | 该目录里的 `product_info.json` |
| 镜像属于哪个 switchset | 镜像自己的 `<镜像名>.debug.json`，从内核 ELF 的 toolchain label 读出来 |

先按产品收窄再按 switchset 取，顺序是必要的：`out/bringup.x86` 里除了产品的两个镜像还有
`fake.obi`，而 `fake` 和 `kernel.dev` **同属 `dbg.0`**——不先筛产品，`dbg.0` 就是二义的。

`kernel_image()` 每产出一个镜像，就在 `.obi` **旁边**写一份 `<镜像名>.debug.json`
（`generated_file` + 一次 `binaries` 的 GN metadata walk，`gn gen` 期就落盘）。里面两类记录：

```json
[
  { "image": "kernel.dev", "obi": "kernel.dev.obi",
    "label": "//kernel/products/image/dev:dev" },

  { "debug": "kernel_x86.dbg.0/obj/kernel/products/bin/vmours.with-tests",
    "environment": "kernel",
    "label": "//kernel/products/bin:vmours.with-tests(//build/switch/set/dbg.0:kernel_x86.dbg.0)" },
  { "debug": "kernel.phys_x86.phys.dbg_0/obj/kernel/phys/physload", "environment": "kernel.phys", ... },
  { "debug": "user-x86/obj/kernel/lib/userabi/userboot/userboot", "environment": "user", ... }
]
```

第一条是镜像自己的身份（用来和 `product_info.json` 的镜像清单对上）；其余是镜像里每个
ELF。**内核就是 `environment == "kernel"` 那条**——一个镜像里还有 physload/physboot
（`kernel.phys`）和 userboot/vDSO（`user`），`environment` 一次就分清了，不需要 join 第二个
key，也不需要从 `.image.elf` 去后缀猜：`debug` 本来就是**未 strip**的那份，调试信息在它里面。

switchset 也从这里读——内核那条 label 的 toolchain 部分就是
`//build/switch/set/dbg.0`，末段即 `dbg.0`。

放在 `.obi` 旁边而不是 `gen/` 深处，是为了"枚举镜像 = 一次 glob"，不需要任何索引，也就
不需要再维护一张"有哪些镜像"的表。

`scripts/qemu_debug.py` 读它，启对应镜像，并把解析出来的 ELF 链到
`out/.debug/kernel.elf`；调试器永远只打开这一个路径。于是"符号和启动的内核
对不上"这件事**没法表达**。

这么做的理由是它已经错过一次。之前每个内核一份配置，要在两处分别写下镜像和 ELF，而这
两者**不是机械对应的**：`kernel.dev.obi` 装的是 `vmours.with-tests` 而不是 `vmours`，
`fake.obi` 装的干脆不是 vmours。那份 `dev` 配置指着 `vmours`，两个二进制里 `start_kernel`
差 `0xE000`（56 KiB）——断点全落在不相干的代码上；更糟的是 dbg.0 的 `vmours` 已经不是
构建目标了（`ninja -t query` 报 unknown），是个不会再被重建的陈旧文件。

三个下拉框的选项是**生成的**。VS Code 没法动态计算 `pickString` 的选项（不装第三方扩展
做不到），所以它们必须是工作区文件里的字面列表——那就让 `tools/gen_debug_menu.py` 来写，
而不是人来写：

| 层 | 它从哪儿读 |
|---|---|
| 产品 | `products/*.gni` |
| 架构 | `tools/set.py` 的 `VALID_CPUS` |
| switchset | `build/switch/set/*/BUILD.gn` 里实例化了 `kernel_switchset` 的那些 |

第三条是**筛**出来的不是 glob 出来的：`//build/switch/set` 下还有 phys 的 switchset
（`phys.dbg_0`），而内核镜像的内核永远不在那里面编译。判据是这个目录实例化的是哪个模板，
和 GN 自己的判据一样。

`tools/set.py` 每次 `gn gen` 成功后会顺手跑一遍，所以**新增一个产品或 switchset，重新
`tools/set.py` 一下菜单就有了**；没变化就不写文件，不制造 git 噪音。手动跑：

```bash
tools/gen_debug_menu.py            # 需要就改写
tools/gen_debug_menu.py --check    # 只检查，过期则退出码 1
```

它**不看任何 `out/` 目录**：菜单是"可以问什么"的列表，而一个构建目录只知道自己被配置成
的那一个产品和架构，拿它生成会把菜单缩成"上次配了什么"。某个具体组合到底存不存在是
`scripts/qemu_debug.py` 的事，它答不上来时会把真的有的列出来：

```
$ ./scripts/qemu_debug.py bringup x86 dbg.2
qemu-debug: product 'bringup' has no image built in switchset 'dbg.2'
qemu-debug: it offers: dbg.0 (kernel.dev), dbg.1 (kernel.dev.dbg_1)

$ ./scripts/qemu_debug.py bringup arm dbg.1
qemu-debug: no build directory at .../out/bringup.arm
qemu-debug: configure it with:  tools/set.py bringup.arm
qemu-debug: products the tree defines: bringup
```

## 新增了一个 kernel_image / kernel_package 会怎样

**镜像清单这一侧全自动**：`kernel_image()` 模板自己写 `<镜像名>.debug.json`，新镜像不需要
碰任何脚本。菜单那一侧跑一次 `tools/set.py` 就跟上。剩下三种情况脚本会明确告诉你，而不是
装作没看见：

**新镜像加进了产品，但和已有镜像撞了 switchset**——第三层不再能唯一确定一个镜像：

```
qemu-debug: product 'bringup' has more than one image in switchset 'dbg.0': one, two
qemu-debug: the switchset stops identifying an image once two of them share it;
qemu-debug: give one of them its own switchset, or select by image name instead
```

**新镜像没加进任何产品**（像 `fake` 那样）——三层选择够不着它，但它会被点名：

```
qemu-debug: product 'bringup' has no image built in switchset 'dbg.1'
qemu-debug: it offers: dbg.0 (one)
qemu-debug: note: newbie is built in 'dbg.1' but belongs to no product
```

**一个镜像里塞进了第二个 kernel_package，于是有两个内核环境的 ELF**——分不出哪个是内核：

```
qemu-debug: .../B describes no image this script can resolve; every one of them has an ambiguous kernel:
qemu-debug:   one: 2 ELFs in the kernel environment -- //k:a(...dbg.0), //k:b(...dbg.0)
qemu-debug: a bootable image is expected to carry exactly one kernel; a second kernel_package would do this
```

这三条以前的行为分别是：**静默挑一个**、**静默看不见**、**整个镜像被跳过然后怪构建没跑**。

不需要任何安装。调试适配器是 `llvm-vs-code-extensions.lldb-dap` 扩展（已装），它经
`xcrun` 找到系统的 `lldb-dap`；QEMU、GRUB、clang 全部来自 `//prebuilt`。

## 读 CPU_LOCAL 变量：`cl`

**`p X86_PCPU` 是错的**，而且错得不出声：CPU_LOCAL 变量的符号指向的是**链接期模板**
（`.init.data` 里那一份），每个 CPU 真正读写的是 `模板 + 该 CPU 的 offset`
（`kernel/mm/include/ours/mm/cpu_local.hpp`）。`p` 打出来的是模板，通常一片零，
永远不是你正在看的那个 CPU。

`scripts/lldb_ours.py` 提供 `cl`，F5 会自动加载（launch 配置的 `initCommands`）：

```
(lldb) cl CURRENT_CPUNUM
current  (ours::CpuNum) ours::mm::CpuLocal::CURRENT_CPUNUM = 0
        template 0xffffff88400a49a0  offset 0xffffffffbff6e000 -> 0xffffff88000129a0

(lldb) cl CLKDEV_STORAGE 1          # 指定 CPU
cpu1  (ours::ApicTimerEvent) ours::CLKDEV_STORAGE = {
  m_clkevt = { m_name = 0xffffff884004dbbf "LVT"  m_mode = Oneshot ... }

(lldb) cl -a CURRENT_CPUNUM         # 所有有 unit 的 CPU
(lldb) cl -l                        # 列出镜像里全部 46 个 CPU_LOCAL 变量
```

三种用法对应三种取 offset 的方式：

| 写法 | offset 从哪来 |
|---|---|
| `cl <var>` | 当前 CPU：`%gs`（见下） |
| `cl <var> <cpu>` | `CpuLocal::CPU_OFFSETS[cpu]`，即 Linux 的 `__per_cpu_offset[]` |
| `cl -a <var>` | 同上，遍历整张表 |

**QEMU 的 gdbstub 把每个 vCPU 表示成一个 thread**，所以 `cl <var>` 跟着 `thread select` 走：

```
(lldb) thread select 3
(lldb) cl CURRENT_CPUNUM
current  (ours::CpuNum) ours::mm::CpuLocal::CURRENT_CPUNUM = 2
```

### 一个必须知道的坑：停在用户态时 `%gs` 不是内核的

内核入口会 `swapgs`。**CPU 处在 ring 3 的时候**，per-CPU offset 被换到了
`IA32_KERNEL_GS_BASE`（stub 里叫 `k_gs_base`），而 `%gs` 是用户态那份——本内核的 userboot
里就是 0。这时候读 `gs_base` 会得到 0，于是每个 CPU_LOCAL 变量都"解析"回它的模板，
**看起来像数据，其实是零**。

`cl` 按 `cs & 3` 判断特权级，自己挑对寄存器，所以停在 userboot 的自旋循环里也照样是对的。
自己手算的时候记得这条。

### `cpus` —— 每个 CPU 在跑什么

```
(lldb) cpus
cpu  running                                      queued  weight  note
  0  'Idle/0' tid=3 Running [Idle | NoFree]            0       0
  1  'userboot' tid=18 Running [Detached]              1    2549
  2  'Sample1' tid=19 Running [Detached]               1    2549
  3  'Sample2' tid=20 Running [Detached]               1    2549
     (12 more with a unit but no scheduler -- never onlined: 4, 5, ...)
```

`running` 那一列来自 per-CPU 的 `Current<Thread*>`，`queued`/`weight` 来自该 CPU 的
`Scheduler`。**这是两个独立更新的地方**，所以命令会交叉核对：调度器自己的 `curr_thread`
和 per-CPU 指针不一致时，note 列会写出来（可能是正好停在上下文切换中间，也可能是 bug，
两种都值得先看这一条）；调度器记录的 `which_cpu` 和我们用来找到它的 offset 对不上时同理。

MAX_CPU 是 16，每个可能的 CPU 都有 unit，但只有上线过的才构造过 `Scheduler`。
把那十二行都当错误列出来会把真正要看的四行埋掉，所以它们被折成一行。

### `kthread` —— 一个线程为什么没在跑

```
(lldb) kthread 2                 # CPU 2 上正在跑的
(lldb) kthread                   # 当前选中 CPU 上的
(lldb) kthread 0xffffff8000a2c000

'Sample1' tid=19 @ 0xffffff8000a2c000
  state    Running   signals None   flags Detached
  cpu      curr=0 last=0 wake=0
  sched    weight=2549  queue=Enqueued/Queueing  schedule=0xffffff884005c428
  blocked  waitqueue=0x0  status=Ok  interruptible=Yes
  locks    resource=free  pi=free
  stack    [0xffffff880402d000, 0xffffff8804031000) 16384 bytes  sp=0xffffff8804030fc0
  aspace   0xffffff8000248000
```

（叫 `kthread` 不叫 `thread`，因为 lldb 已经有 `thread` 了，脚本不让覆盖。）

**`blocked` 那一行是重点**：`m_waiter_state.m_waitqueue` 指向阻塞它的队列。
**`state == Parked` 而 `waitqueue == 0x0` 就是丢了唤醒路径**——没有任何东西会再叫醒它。
这比"队列里有几个"直接得多。

**`locks` 那一行也是**：`sync::SpinLock` 的字有语义（`spin_lock.hpp:120-124`），
**0 = 空闲，否则 = 持有者 CPU 号 + 1**，所以能直接打出 `pi=held by cpu2`。
一串线程互相卡住的时候，这一行直接点名该去看哪个 CPU。

`!!` 开头的是告警，只报**确实错了**的：

| 告警 | 判据 |
|---|---|
| `Parked with no wait queue` | 没人会唤醒它 |
| `effective weight is 0` | `avg_vruntime()` 会除以它（`profiles.hpp:20-27`） |
| `sp is outside its own kernel stack` | 栈溢出/损坏（运行中的线程不查，真值在寄存器里） |
| `curr/last/wake_cpu all 0` | 见 `docs/repairs/260910/task-sched.md` |

**故意不报的**：`m_runqueue_state.m_transient_state` 长期是 `Rescheduling`。
`context.hpp:129-136` 自己记着这是已知缺陷（`Rescheduling` 永远不会被清、`DequeueDelayed`
从不被写入），报它等于每一行都报，告警就没人看了。

### `nodes` —— 物理内存节点

```
(lldb) nodes
node 0 @ 0xffffff801febd978  start_pfn=0
    spanned    130780 frames     510 MiB
    present    129451 frames     505 MiB
    reserved        0 frames       0 MiB
    zone Dma      @ 0xffffff801febd6c8  present=3752   managed=2507
    zone Dma32    @ 0xffffff801febd418  present=125699 managed=123334
```

走的是 `PmNode::NODES[0..NUM_NODES)`，不是 per-CPU 的 `Current<PmNode*>`——要看的是
所有节点，不是当前这个 CPU 的那个。zone 队列是一张 fallback 顺序表，同一个 zone 会出现
多次，命令按首次出现去重。

**没有打印 `num_online_nodes()`**，因为它返回垃圾——见 `docs/repairs/260910/mm.md`。

### 指针会被验真

内核里长期存在的对象开头大多有一个 `gktl::Canary`，magic 是**类型全名的 consteval 哈希**
（`kernel/lib/gktl/include/gktl/canary.hpp`）。脚本把同一个哈希用 Python 重算了一遍，
于是可以回答"这个指针到底是不是一个 Thread"：

```
0xffffff8800012xxx <canary 0xdeadbeef -- not a Thread>
```

而且 **0 和"不对"是两回事**：`Canary` 的默认构造把 magic 置 0，所以 magic 为 0 表示
**这个对象从来没被构造过**（比如没上线的 CPU），非 0 但不匹配才是"这片内存是别人的"。
两者混成一句"canary bad"会把"正常没上线"报成故障。

### 不是 CPU_LOCAL 的变量会被拒绝

判据是链接脚本自己的 `[__cpu_local_start, __cpu_local_end)`（`kernel/kernel.ld:175,204`），
不是节名——`.init.data.cpu_local*` 会被并进 `.init.data`，而那里面大部分不是 per-CPU 的。

```
(lldb) cl LAPIC_TIMER_EVENT
error: cl: 'ours::LAPIC_TIMER_EVENT' is not CPU_LOCAL (outside __cpu_local_start..__cpu_local_end) -- use `p`
```

对一个不是 per-CPU 的变量套 offset 只会打出一堆看着挺像回事的垃圾，所以宁可拒绝。

## 为什么是「停在第一条指令之前」

QEMU 用 `-S` 启动。这不是保守，是必要的：等到一个**正在运行**的内核可以被挂上时，
`start_kernel` 早就过去了，早期启动的一切都没法下断点。

代价是会话一开始停在固件之前，要自己按一下继续。**不要**用 `postRunCommands` 里加
`continue` 来省这一下：那串命令跑在 DAP 的 attach 请求里，早于编辑器把断点下发给适配器，
省掉的一下换来的是早期断点被跑过去。

## 为什么是 attach 而不是 launch

`lldb-dap` 的 `launch` 要求 `program`，并且要把它当本地进程拉起来——QEMU 里的内核不是
本地进程。只有 `attach` 提供 `attachCommands`：一串"代替按 pid 附加"执行、并且**允许自己
建 target** 的命令。配置里那三条（`target create` / `target modules load --slide` /
`gdb-remote`）就挂在这个钩子上。

## 命令行

同一套东西不进 VS Code 也能用：

```bash
./scripts/qemu_debug.py                    # bringup x86 dbg.1
./scripts/qemu_debug.py bringup x86 dbg.0
```

它清掉上一次会话残留的 QEMU/lldb（**两个进程抢同一个端口会互相打死**，而活下来的不一定
是新的那个），启动 QEMU，**轮询到端口真的接受连接**才打印 ready ——不是 sleep 一个猜的
秒数：QEMU 被 exec 之前还要重建 ESP（grub-mkimage、mformat、mcopy）。

另开一个终端：

```bash
lldb -o 'target create out/.debug/kernel.elf' \
     -o 'target modules load --file kernel.elf --slide 0xffffff8840000000' \
     -o 'gdb-remote localhost:1234'
```

这里不用管刚才启的是哪个产品/架构/内核——脚本已经把配套的 ELF 链到 `out/.debug/kernel.elf`
了，它也会把真实路径打在 `qemu-debug: symbols ...` 那一行上。**这个路径里没有产品和架构**，
是固定的一处：一个 checkout、一个 gdbstub 端口、一个正在调的内核。

换端口：`PORT=1235`。启一个构建没描述的镜像：`IMAGE=... ELF=... ./scripts/qemu_debug.py`
——**两个必须一起给**，只给镜像不给符号正是这一整套要防的那个错。

## 那个 slide 是什么

内核链接在地址 0，而 physboot 把它加载到 `KERNEL_LOAD_BASE`。不滑动符号的话每个地址都差
64 GiB，断点会打在虚空里。

`0xffffff8840000000` = `KERNEL_ASPACE_BASE + ARCH_PHYSMAP_SIZE + GB(1)`
（见 `kernel/arch/x86/kernel/include/ours/arch/aspace_layout.hpp`）。

**它不是永远不变的**：一旦启用 KASLR，镜像会带一个最多 GB(1) 的偏移。真实值在启动日志里：

```
Reserve area k:head: [FFFFFF8840000000, FFFFFF8840007000)
```

左边那个数就是 slide。

## 不用调试器往往更快

内核不退出，所以看日志是：

```bash
( IMAGE=$PWD/out/bringup.x86/kernel.dev.dbg_1.obi ./scripts/qemu_boot.py > /tmp/boot.log 2>&1 & \
  pid=$!; sleep 30; kill $pid 2>/dev/null; wait $pid 2>/dev/null )
grep -a "..." /tmp/boot.log
```

两点值得记住：

- **`grep` 要带 `-a`**。启动日志里有终端转义和 NUL 字节，`grep` 会把它当二进制文件**静默
  跳过**——你会以为什么都没匹配上。
- **日志末尾如果没有 `terminating on signal 15`，说明 QEMU 是自己退的**，也就是三重故障，
  不是挂死。

追一处静默停止时，临时插几行标了 `// PROBE` 的 `log::trace` 再删掉，常常比 gdbstub 快
——gdbstub 下的启动要慢上不少。
