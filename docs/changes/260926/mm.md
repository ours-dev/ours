# kernel/mm 改动记录（2026-09-26）

## 内核镜像的 VMA 改成读 physboot 的映射清单，不再照链接符号手写（2026-09-26）

**文件**
- `kernel/mm/init_vmm.cpp` — 删掉 `PHYS_VMOS` 里描述镜像的七条，剩下的两条改名 `PRESET_VMAS`；新增 `enrol_handoff_mappings()`、`name_of()`、`vma_flags_of()`

**症状**
潜伏。`PHYS_VMOS` 是一张照着内核自己的链接符号手写的表——`IMAGE_START`、
`KERNEL_CODE_START`、`KERNEL_RODATA_START`……——用来声明内核镜像占了哪些地址、
并把已有映射的权限改写成它说的那样（`vma->reserve()`）。原地就挂着一条 TODO：

```
/// TODO(SmallHuaZi) These be provided by PhysBoot through HandOff rather than
/// direct to use linking symbols.
```

两份描述，描述同一件事，而写第二份的一方没有参与那件事。符号说的是「段被链接在哪」，
映射说的是「physboot 实际建立了什么」，中间没有任何东西校验过两者一致。

具体差在哪，这次一比就看出来了：那张表把 `.init*` 当成**一整块 RWX**
（`k:init`，`Read | Write | Exec`），而 physboot 实际映射的是三段——
`.init.text` r-x、`.init.rodata` rw-、`.init.data` rw-。也就是说
`reserve()` 一直在把 `.init.rodata` 和 `.init.data` 改写成**可执行**的。

**原因**
最终答案只有 `fix_mapping()` 知道，两件事内核无法重现：

1. **KASLR**。镜像落在 `KERNEL_LOAD_BASE + kaslr_offset`，偏移是随机的。
   链接符号在重定位之后确实带着运行期地址，所以这一条勉强能对上——但那是巧合，
   不是设计。
2. **RELRO 切分**。`PT_GNU_RELRO` 覆盖的页要从所属段的权限里去掉 Writable，
   而这个切分是在 `SegmentMapper::map_segment()` 的 run 循环里算的。没有哪个链接符号
   描述切分之后的边界——`k:relro` 那一条是照 `KERNEL_RELRO_START/END` 写的，
   而实际有**三个** `GNU_RELRO` 区间（`.dynamic`、`.data.rel.ro`、`.ccrt_init`）。

上一条（`docs/changes/260926/phys.md`）让 physboot 把它建立的每一条映射记下来交过来；
这一条是读的那一半。

**改法**
`init_vmm_preheap()` 先跑 `enrol_handoff_mappings()`：遍历 `boot::handoff().mem.mappings`，
每条按权限推出名字和 `VmaFlags`，然后交给原来那个 `entrol_preset_vma()`，
`Backing::Mapped` 不变。

名字由权限推出（Exec → `k:code`，Write → `k:data`，否则 `k:rodata`），
而不是从 phys 那边搬字符串过来：`k:` 是内核自己的词汇，而且名字本来就跟着权限走。
这样也不再声称知道某一段装的是哪个 **section**——那张表是凭一个符号在声称这件事，
而它描述的那段映射早已被 RELRO 切成了没有任何 section 边界能解释的若干 run。

留在表里的只有两条，各自因为相反的理由 phys 不该描述：

- `k:physmap` —— phys 确实映射了它，但它的基址和大小是 `aspace_layout.hpp` 里的架构常量，
  内核编译进去的和 phys 读的是同一个头文件，告诉它等于告诉它它已经知道的事。
- `k:cpulocal` —— 根本不是 phys 填的。`CpuLocal::Mapper` 在 NUMA 拓扑出现之后一个 slot
  一个 slot 地填，而拓扑是 phys 没有的知识。（这一条是和用户确认过的：CpuLocal 不进清单。）

`ASSERT(!mappings.empty(), ...)`：清单空了意味着 physboot 没有交，
而此时内核已经跑在那些映射上——继续往下走只会在别处炸。

**参照**
Zircon 的 `PhysHandoff::vmars` 由 `HandoffEnd` 在启动末尾兑现成真的 VMAR，
是同一条路子。它那边 `PhysMapping` 带 `name`，因为它的清单还要覆盖 MMIO 和交给用户态的东西，
名字不能由权限推出；这里只有内核镜像，所以名字留在消费侧。

**验证**
- `out/bringup.x86`、`out/numa.x86` 两个配置构建通过。
- **这条改动自带一个很硬的测试**：`vma->reserve()` 会按 VMA 说的权限**改写已有页表项**。
  清单只要有一处不对——少一页、多一页、权限反了——内核会在 `init_vmm()` 之后立刻死：
  `.text` 丢掉执行权限就是取指故障，`.data` 变只读就是下一次写故障。两个镜像都跑到
  shell 六组检查全过、进程正常销毁，numa 7 个 AP 全 online。
- `kernel.dev.dbg_1.obi`（trace 级别）打出的内核 VMA 列表，正是 physboot 记录的那 11 条，
  加上两条常量：

```
A k:physmap : [0xFFFFFF8000000000, 0x800000000]
A k:cpulocal: [0xFFFFFF8800000000, 0x4000000]
A k:rodata  : [0xFFFFFF8840000000, 0x1000]     <- .dynamic，被 RELRO 去掉写权限
A k:rodata  : [0xFFFFFF8840001000, 0x5000]
A k:code    : [0xFFFFFF8840006000, 0x59000]    <- .text
A k:rodata  : [0xFFFFFF884005F000, 0x16000]    <- .rodata
A k:data    : [0xFFFFFF8840075000, 0x4000]     <- .data
A k:rodata  : [0xFFFFFF8840079000, 0x1000]     <- .data.rel.ro，RELRO
A k:data    : [0xFFFFFF884007A000, 0x65000]    <- .bss
A k:code    : [0xFFFFFF88400DF000, 0x9000]     <- .init.text
A k:data    : [0xFFFFFF88400E8000, 0x1000]     <- .init.rodata
A k:data    : [0xFFFFFF88400E9000, 0x15000]    <- .init.data
A k:rodata  : [0xFFFFFF88400FE000, 0x1000]     <- .ccrt_init，RELRO
```

地址连续无空洞，这正是原来那张表的注释要求而只能靠自觉保证的性质：镜像里有一页没被
任何 VMA 认领，VMM 就可以在镜像还占着它的时候把那个地址发给别人。

- dbg_1 那次启动里的 6 处 page fault 全部来自 `fault_probe` 那个故意越界的用户程序
  （`terminating process 'fault_probe'`），是预期行为，不是内核故障。

**故意没做**
- 没有顺手修 `.init.rodata` 段是 RW 而不是 R 这件事（`llvm-readelf -l` 里第 7 个 PT_LOAD）。
  那是 `kernel.ld` 的问题，改它要动链接脚本并重新验证整个镜像布局，是另一件事。
  现在的好处是：清单如实反映了它，而不是像原来那样把它连同 `.init.text` 一起说成 RWX。
- 没有动 `Handoff::verify()`。它检查的是 `mem.ranges` 非空；`mappings` 的检查放在
  `enrol_handoff_mappings()` 里，靠近真正依赖它的地方。
- 没有把 `k:physmap`/`k:cpulocal` 也变成某种「内核自己的清单」结构。两条常量用一张
  两行的表表达就够了，再抽象一层只会多一个要读的间接层。

## `INIT_CONST` 改名 `INIT_RODATA`，`const` 由人手写（2026-09-26）

**文件**
- `kernel/include/ours/init.hpp` — `INIT_CONST` → `INIT_RODATA`，`MEM_INIT_CONST` → `MEM_INIT_RODATA`
- `kernel/mm/init_vmm.cpp` — 唯一的使用点补上 `const`

**症状**
潜伏。`INIT_CONST` 的展开只是一个链接段属性：

```c
#define INIT_SECTION(name) LINK_SECTION(".init." name)
#define INIT_CONST         INIT_SECTION("rodata")
```

它不带 `const`。全树唯一的使用点是 `kernel/mm/init_vmm.cpp` 那张预设 VMA 表
（原 `PHYS_VMOS`，现 `PRESET_VMAS`），一直是个**可写数组**，名字是唯一暗示它只读的东西。

**原因**
这是从「`.init.rodata` 段为什么是 `WA` 而不是只读」查出来的。那个输出段有三个可写贡献者，
这张表是其中之一，也是唯一纯属漏掉的一个（另两个见下）。

宏改名而不是让宏发出 `const`，理由不是风格，是**宏做不到**：宏只能前置，而前置的 `const`
绑到被指物。对指针声明它产生的是

```c
const int *p    // 可变的指针，指向常量数据
```

而作者要的是

```c
int *const p    // 只读的指针
```

也就是说，这个宏恰好会在「constness 最要紧」的那些声明上静默地给出错的东西，
只在「手写 `const` 本来就不费事」的简单情形下是对的。所以关键字留在声明处，
由写它的人放到正确的那一侧。`INIT_CODE`/`INIT_DATA` 本来也只是段名，`INIT_RODATA`
和它们对齐。

**改法**
宏改名，使用点写成 `PhysVma const PRESET_VMAS[]`。`MEM_INIT_CONST` 一起改名
（它当前没有使用点，但留一个旧名字就是留一个下次被复制的坏名字）。

**参照**
无，本项目自有约定。同一条约定的前一次是 `FORCE_INLINE` 不许把 `inline` 打包进去
（见 `system/public/ours/compiler.hpp`）：属性宏只展开成属性。

**验证**
- `out/bringup.x86`、`out/numa.x86` 两个配置构建通过，两个镜像启动到 shell 全部检查通过，
  numa 7 个 AP 全 online，无 fault / panic / 断言失败。
- **段权限没有变，仍是 `WA`**，这一点要说清楚不要误以为修好了。查了 `init_vmm.cpp.o`：

```
[271] .init.rodata      PROGBITS  ...  WA
[272] .rela.init.rodata RELA      ...
      R_X86_64_64  .rodata.str1.1 + 741
      R_X86_64_64  .rodata.str1.1 + 74b
```

表里的 `char const *name` 需要重定位，而内核是 DYN 镜像，所以工具链必须把段标成可写。
`const` 加的是**编译期**保证（谁写它就编译不过），不是硬件保证。

**故意没做**
- 没有让 `.init.rodata` 真的只读。试过两条，这个 lld 都不行：输出段属性 `(READONLY)`
  报 `symbol not found: READONLY`；把输出段改名 `.data.rel.ro.init` 想蹭 RELRO 也没用
  （`.ccrt_init` 能进 RELRO 靠的是 `INIT_ARRAY` 段类型，不是名字）。
  剩下的办法是内核自己把 `[__init_rodata_start, __init_rodata_end)` 重新保护成只读——
  这里**不需要** RELRO，因为 `fix_mapping()` 的重定位走的是镜像物理页的恒等映射，
  最终映射从来不需要可写。这是单独一条，留在待修项里。
- 没有动 `.gktl.init_hook` 和 `.task.schedules` 这两个可写贡献者。它们的条目里有函数指针，
  同样是重定位的缘故，不是漏加 `const`（两者本来就是 `const`/`constexpr`）。

## 内核把 `.init.rodata` 的映射改成只读（2026-09-26）

**文件**
- `kernel/mm/include/ours/mm/vmm.hpp` — 新增 `KERNEL_INIT_RODATA_START/END` 两个链接符号声明
- `kernel/mm/init_vmm.cpp` — 新增 `tighten_init_rodata()`，enrol 每条 handoff 映射时去掉 Write
- `docs/repairs/260925/mm.md` — 删掉第 3 条

**症状**
潜伏。`.init.rodata` 这个段名承诺只读，而映射是可写的——physboot 按 PT_LOAD 的 flags 建映射，
而那个段在 ELF 里是 `WA`。内核的 init hook 表（`.gktl.init_hook`）、计数器描述符和
调度类注册表（`.task.schedules`）都在里面，运行期可写。没有人写它们，所以今天没有故障；
值在于把「只读」从命名约定变成硬件保证。

**原因**
段之所以是 `WA`，是因为里面的条目含指针（init hook 的函数指针、discipline 的 `&INSTANCE`），
内核是 DYN 镜像，这些是 `R_X86_64_64` 动态重定位，clang 无论对象多 `const` 都必须把段标可写。
标准的解法是 `PT_GNU_RELRO`，而 lld 只把它认得的段放进 RELRO——这正是 `kernel.ld` 第 98 行
那条注释说的，所以其它 RELRO 段全被并进了 `.data.rel.ro`。`.init.rodata` 并不进去，
因为它必须留在 `.init*` 尾巴里让 `reclaim_init_area()` 整段回收。

**但这里不需要 RELRO，这是整条改动的支点。** RELRO 存在的理由是动态链接器要
**通过它正在修补的那个映射**写重定位。physboot 不是这样：`ElfImage::fix_mapping()` 的契约
写得很清楚——`relocate()` 已经通过镜像物理页的**恒等映射**跑完了，内核映射是之后才建的。
所以没有任何人会通过这个地址写这些页，内核完全可以去掉 Write。而且「这个段本该只读」
这件事只有内核知道。

**改法**
不是事后 `protect()`，而是在 enrol 每条 handoff 映射时就把权限收紧：

```cpp
auto const perms = tighten_init_rodata(mapping.perms, mapping.base, mapping.size);
entrol_preset_vma({
    .name = name_of(perms),
    ...
    .vmaf = vma_flags_of(perms),
```

因为 `entrol_preset_vma()` 正是用这些 flags 去 `reserve()`（`extract_permissions(vmaf)`），
所以在这里收紧能让 **VMA 和硬件说同一句话**。事后再 `arch_aspace().protect()` 会留下
「VMA 说可写、页表说只读」的分裂，那是下一个人要查半天的东西。名字也跟着变成 `k:rodata`。

`ASSERT` 要求映射**整条**落在 `[__init_rodata_start, __init_rodata_end)` 内：两端都被
`kernel.ld` 的 `ALIGN(MAXPAGESIZE)` 对齐过，physboot 又只映射整页，所以部分重叠意味着这两条
前提之一不成立了——而「从半条映射里去掉 Write」不是这里能做的事。

**这不是内核在质疑 handoff 清单**：清单如实报告了 physboot 建立了什么，这是内核对**自己的
数据**做的决定，做在知识所在的地方。改动记录里写明这一点，否则下一个人会以为清单又被绕过了。

**参照**
无直接对应物。Zircon 的内核镜像保护走
`VmAspace::ProtectRegion` / `vm_init()` 里对 `__rodata_start` 一类符号的重新保护，
思路相同（内核按自己知道的语义收紧启动时建立的映射），但它没有「init 尾巴要整段回收」
这个额外约束，所以不需要在 RELRO 之外另找办法。

**验证**
- 两个配置构建通过，`out/bringup.x86`、`out/numa.x86` 都启动到 shell 全部检查通过，
  numa 7 个 AP 全 online，无 fault / panic / 断言失败。
- `kernel.dev.dbg_1.obi`（断言全开 + trace）也干净，而且那条 VMA 的名字从
  `k:data` 变成了 `k:rodata`：

```
A k:code    : [0xFFFFFF88400E0000, 0x9000]
A k:rodata  : [0xFFFFFF88400E9000, 0x1000]   <- .init.rodata，原来是 k:data
A k:data    : [0xFFFFFF88400EA000, 0x15000]
```

- **硬件层面直接验证**：在 QEMU 里用 QMP 读 `CR3`，顺着页表走到 physboot 那一条
  `.init.rodata` 映射的虚拟地址（同一次启动的 debug 日志里报的是
  `[0xFFFFFF88400D0000, 0xFFFFFF88400D1000) <- 0x1FDC8000 rw-`）：

```
CR3=0x1FEDB000
PML4[0x1FF] = 0x000000001FEBF027  P=1 RW=1
PDPT[0x21]  = 0x000000001FEBE027  P=1 RW=1
PD[0x0]     = 0x000000001FEBD027  P=1 RW=1
PT[0xD0]    = 0x800000001FDC8161  P=1 RW=0
```

叶级 PTE 是 `0x...161`——P、A、G、NX 都在，**RW 位是 0**。physboot 映射成 `rw-` 的那一页，
内核起来之后是只读的。

- `reclaim_init_area()` 之后这些帧会还给 buddy（`kernel/top/main.cpp:64`，在
  `init_platform()` 之后无条件跑），而内核虚拟映射仍在。两个镜像都跑过了这一步并继续启动
  userboot、跑完 shell，说明重用那些帧的人走 physmap 别名写，不受这个只读映射影响——
  这条原来在待修项里标着「要实测确认」，现在确认了。

**故意没做**
- 没有对 `.init.text` 做对应的事。它已经是 `r-x`，physboot 映射出来就是对的。
- 没有去动 `.init.rodata` 在 ELF 里仍是 `WA` 这件事。那要么需要 lld 支持
  `(READONLY)`/`INPUT_SECTION_FLAGS`（都不支持），要么需要把段挪出 `.init*` 尾巴
  （和整段回收冲突）。现在的状态是：ELF 说可写，映射是只读的，理由写在
  `tighten_init_rodata()` 的注释里。
- 没有给 `.init.rodata` 之外的段加同类收紧。清单里其它 `rw-` 的段（`.data`、`.bss`、
  `.init.data`）本来就该可写。

## `CpuLocal::Pinned`：先关抢占/中断、再取本 CPU 那份，两步绑成一个对象（2026-09-26）

**文件**
- `kernel/mm/include/ours/mm/cpu_local.hpp` — 新增 `CpuLocal::Pinned<T, Options...>`，
  以及它每个选项对应的 saver（私有的 `CpuLocal::PinnedSaver`）。

**症状**
潜伏，而且是一类问题而不是一个 bug：`CpuLocal` 取「当前 CPU 那份」的接口
（`access()`、`with_current()`、`current_cpu()`、`PerCpu::ref_mut()`/`ptr_mut()`）
自己既不关抢占也不关中断，只在被调用的那一刻算一次「现在是哪个 CPU」，把答案交出去。
调用者如果没在取之前先把抢占或中断关掉，拿到的引用在下一条指令就可能已经不是
「本 CPU 的」了。现在没有观察到的故障，这次也不改任何调用点。

**原因**
内核在内核态会被抢占：`arch_handle_exception()`（`kernel/arch/x86/kernel/faults.cpp`）
在中断收尾时，只要 `irq::handle_irq_postamble()` 说 `PreemptionState` 允许、而且有挂着
的本地调度请求，就调 `Thread::Current::preempt()`。于是「取本 CPU 那份」和「最后一次用它」
之间只要开着抢占，就有两种坏法：

1. 线程被换下，之后在**另一个 CPU** 上接着跑，手里还拿着原来那个 CPU 的那份，
   和那个 CPU 自己的代码同时改它；
2. 线程没被迁走，但中间跑过的**同一个 CPU 上的另一个线程**改了同一份，
   回来时这边的读-改-写基于的是旧值。

关键在于「取」这个动作本身就必须处在保护里。先取后保护是没用的——保护开始之前那几条
指令就是窗口。`MemCache::allocate()` 现在是手写对的（先 `AutoPreemptDisabler`，再
`ptr_mut()`），但只是靠写的人小心，把两行写反，编译器和运行时都不会说什么。

单独的 `CpuLocal::read()`/`write()` 不在此列：它们各是一条 `%gs` 相对寻址指令，
执行中不会被切走。但 `read()` 接 `write()` 是两条指令，那是非原子的读-改-写。

**改法**
加一个 RAII 对象，让「保护」和「取」无法分开，顺序也无法写反：

```cpp
// 改前：顺序靠调用者自觉（MemCache::allocate() 的写法）
task::AutoPreemptDisabler preempt_disabler{};
...
return allocate(gaf, nid, m_cpu_caches.ptr_mut());
```

```cpp
// 改后：构造时先关，再取；析构时先放引用，再开
CpuLocal::Pinned<CpuCache, PreemptDisable> cache{m_cpu_caches};
cache->...;
```

核心是成员声明顺序：

```cpp
template <typename T, typename... Options>
class CpuLocal::Pinned {
    ...
    explicit Pinned(PerCpu<T> &percpu)
        : m_savers(), m_cpu(CpuLocal::current_cpu()), m_object(&percpu.ref_mut(m_cpu)) {}

    ~Pinned() {
        DEBUG_ASSERT(CpuLocal::current_cpu() == m_cpu, ...);
    }
    ...
  private:
    [[no_unique_address]]
    std::tuple<typename CpuLocal::PinnedSaver<Options>::Type...> m_savers;  // 必须第一个
    CpuNum m_cpu;
    T *m_object;
};
```

成员按声明顺序构造、逆序析构，所以 saver 一定在 `current_cpu()` 之前生效、在一切之后
才撤。反汇编确认过：`PreemptDisable` 版本在读 `CURRENT_CPUNUM` 之前先把当前线程的
抢占计数加一；`IrqSave` 版本是 `pushf; cli` 之后才读，`push; popf` 在最后。

选项沿用 `Guard` 的标签（`<ours/lockdep.hpp>` 里的 `PreemptDisable`、`IrqSave`、
`IrqSaveAndPreemptDisable`），含义与它们在自旋锁上一样；每个标签对应的 saver 也和
`spin_lock.hpp` 里 `Guard<SpinLock, ...>` 用的是同样的类型：

| 选项 | saver | 用在 |
|---|---|---|
| `PreemptDisable` | `task::AutoPreemptDisabler` | 只有线程会碰的数据 |
| `IrqSave` | `arch::AutoInterruptDisabler` | 中断处理函数也会碰；第一个线程存在之前唯一能用的 |
| `IrqSaveAndPreemptDisable` | 两者的 `std::tuple` | 作用域里可能唤醒线程，要把调度推迟到作用域结束 |

至少要一个选项，和 `Guard<SpinLock>` 必须带选项同理：不带选项就等于 `access()`。
不认识的选项撞上 `PinnedSaver` 未定义的主模板，编译失败，而不是悄悄什么都不存。

被否掉的做法：

- **通过 `lockdep::StateSaver` 去查 saver**（第一版就是这么写的，在 `ours::mm` 里为
  `Pinned` 手写 `lockdep_get_state_saver()` 声明，靠 ADL 查到）。按开发者的意见改掉了：
  `StateSaver` 是 lockdep 给它自己的 `Guard` 用的内部机制，不是让别的模块搭在上面的接口。
  现在在 `CpuLocal` 里私有地列出三条映射。
- **在 `sync::LocalLock` 上套一个 `Guard` 再取引用**。`LocalLock` 就是 Linux `local_lock`
  的对应物，语义很贴，但 lockdep 目前按锁的类型分类（`DefaultLockClass`），所有
  `LocalLock` 是同一个类：嵌套两个 `Pinned` 会被报成重复获取，不同 `Pinned` 与别的锁之间
  的先后也会被混成一条依赖边，报出假的环。关抢占本身不会死锁，没必要让 lockdep 管它。
- **不要求选项、默认关抢占**。第一个线程建起来之前关抢占用不了（要经过当前线程的
  `PreemptionState`），而中断里也会碰的数据只关抢占又不够，默认值总会对一部分调用者是错的。

不是所有 per-CPU 读都要 `Pinned`：对调用线程来说「在哪个 CPU 上读都一样」的值——典型的
是当前线程指针，每个 CPU 的槽里放的都是正在那里跑的线程——只要一条指令读完
（`CpuLocal::read()`）就行。`Pinned` 是给「拿到之后还要接着用、而且属于哪个 CPU 很要紧」
的那份用的。这一点写进了 `Pinned` 的注释。

**参照**
- Linux `get_cpu_ptr()`/`put_cpu_ptr()`（`include/linux/percpu-defs.h`）：
  `preempt_disable()` 之后才 `this_cpu_ptr()`，顺序和这里一样；中断场景对应
  `local_lock_irqsave()`（`include/linux/local_lock.h`）。
- Linux `current`：x86 的 `get_current()`（`arch/x86/include/asm/current.h`）是一条
  `%gs` load（`this_cpu_read_stable()`，字段名随版本变过），不关抢占——就是上面
  「不需要 Pinned」那一类。
- Zircon `percpu::WithCurrentPreemptDisable()`（`zircon/kernel/include/kernel/percpu.h`），
  回调形式；这里用作用域对象，是因为调用点往往要在同一作用域里多次使用那份数据。

**验证**
- 构建通过；`clang-format` 干净；`clang-tidy` 在新增行上无诊断（`cpu()` 按提示加了
  `[[nodiscard]]`）。
- 仓库里还没有调用者，所以写了一个临时 TU（不入库），用 `object_cache.cpp` 在
  `kernel_x86.dbg.0`/`dbg.1` 下的真实编译参数编译：三种选项、`CPU_LOCAL` 变量和
  `PerCpu<T>` 两种来源都实例化通过；故意传一个非标签类型报
  `implicit instantiation of undefined template 'ours::mm::CpuLocal::PinnedSaver<NotATag>'`，
  不传选项报 `static_assert` 的那句说明。反汇编核对了上面说的保护先于取值。
- 两个镜像 QEMU 实测都跑到 `--- end ---`：dev 上 `init: shell closed its channel`、
  `512 channels made before the quota`；dbg_1 上 `probes done (11 killed, 0 survived)`、
  `bootfs checks passed`。dbg_1 在 25 秒窗口里没跑完、放到 50 秒跑完；这次改动只加了一个
  未实例化的模板，不改变生成的代码。
- 运行时行为（包括析构里「没有被迁走」的断言）要等第一个调用点换过来才真正跑到，**未实测**。

**故意没做**
- 没改任何调用点。哪些地方真的需要换成 `Pinned`，要看逐个调用点的审计结果，一处一处改。
- 没给 `access()`/`current_cpu()` 加 Linux `debug_smp_processor_id()` 那种「抢占开着就报警」
  的调试检查。调用点没换完之前加，会在大量现有路径上直接触发。
- 没管别的 CPU 通过 `access(t, cpu)`/`for_each()`/`fold()` 访问这份数据的情况：那需要锁或
  原子操作，`Pinned` 只解决「哪一份是本 CPU 的」。
- 没加 Linux `this_cpu_add()` 那种单指令的 per-CPU 读-改-写原语。

## `CpuLocal::Current::get()` 对放得进寄存器的 T 改成一条 `%gs` load，`Thread::Current::get()` 不再分两步读（2026-09-26）

**文件**
- `kernel/mm/include/ours/mm/cpu_local.hpp` — `CpuLocal::Current<T>::get()` 按 `sizeof(T)` 分两种：
  放得进一个寄存器就经 `CpuLocal::read()` 返回值，否则照旧经 `access()` 返回引用；
  `Pinned` 注释里「只要一条指令读完」那句补上指向它。
- `docs/repairs/260926/mm.md` — 删掉已修的第 1 条，其余顺延。

**症状**
潜伏，今天触发不到。一旦就绪线程可以跨 CPU 迁移（负载均衡、work stealing），`Thread::Current::get()`
可能返回**别的线程**，调用者接着对它的抢占计数、地址空间、锁上下文动手。

**原因**
`Thread::Current` 继承 `CpuLocal::Current<Thread *>`，`get()` 原来是 `access(CURRENT_INSTANCE)`，
反汇编是三条：

```
movq %gs:CURRENT_CPU_OFFSET(%rip), %rcx   ; 1. 读本 CPU 的偏移
leaq CURRENT_INSTANCE(%rip), %rax          ; 2. 模板地址
addq %rax, %rcx
movq (%rcx), %rcx                          ; 3. 读槽
```

第 1 条和第 3 条之间如果被抢占、并在另一个 CPU 上恢复，第 3 条读的是**第一个 CPU 的槽**，
而那个槽此时已经被上下文切换改成了第一个 CPU 换上去的线程。当前线程指针的特殊之处就在这里：
每个 CPU 的槽在切换时都会被改写，所以「读的是哪个 CPU 的槽」与「读的是在哪一刻」必须是同一条指令。

今天没事，只因为没有任何东西会移动一个被抢占的线程：`Scheduler::steal_work_from()` 是
`return 0;`，`migrate_thread()` 是空分支，被抢占的线程由 `put_prev_thread()` 放回本 CPU 的队列。
第一个负载均衡落地时它就会坏。

**改法**

```cpp
// 改前
FORCE_INLINE
static auto get() -> T & {
    return CpuLocal::access(CURRENT_INSTANCE);
}
```

```cpp
// 改后
FORCE_INLINE
static auto get() -> decltype(auto) {
    if constexpr (sizeof(T) <= sizeof(usize)) {
        return CpuLocal::read(CURRENT_INSTANCE);
    } else {
        return CpuLocal::access(CURRENT_INSTANCE);
    }
}
```

改后同一处只剩一条 `movq %gs:CURRENT_INSTANCE(%rip), %rax`：抢占只能落在它之前或之后，
读到的都是调用者此刻所在 CPU 的槽，对 `Thread::Current` 来说就是调用者自己。

改在 `CpuLocal::Current` 而不是只给 `Thread::Current` 加一个遮蔽的 `get()`：病根是这个接口对
「指向当前对象的指针」这种槽也返回 `T &`（槽的引用），而引用只有在调用者被钉在这个 CPU 上时才有意义；
按值读才是这类槽该有的形状。树里放得进寄存器的 `Current<T>` 只有两个，`Thread *` 和 `PmNode *`，
调用者全是按值用（`->`、比较、存进局部变量），没有人绑定引用或经它写入（写有 `set()`）。
大小判断沿用 `set()` 选 `write()` 的那一个，读写对称。对象型的 `Current<T>`（`Scheduler`、
`TickDevice`、`TimerQueue` 等）照旧返回引用，持续使用时该由调用者用 `CpuLocal::Pinned` 钉住。

`PmNode::Current` 跟着变成单指令读，但它原来的两步读并不会出错：PmNode 槽在上下文切换时不变，
读到「刚才那个 CPU 的节点」与「读完再被迁走」是同一种过时。

**参照**
- Zircon `arch_get_current_thread()`（`zircon/kernel/arch/x86/include/arch/current_thread.h`）：
  `READ_PERCPU_FIELD(current_thread)` 直接从 gs 读，注释写明不经 `x86_get_percpu()->current_thread`
  分两步读，就是为了避免两步之间上下文切换、读到别的 CPU 的当前线程。
- Linux x86 `get_current()`（`arch/x86/include/asm/current.h`）：一条 `%gs` load
  （`this_cpu_read_stable()`）。

**验证**
- 构建通过；`clang-format` 干净；`clang-tidy` 在改动行上无新诊断。
- 反汇编：用 `object_cache.cpp` 的 `kernel_x86.dbg.0` 编译参数编临时 TU，`Pinned<u64, PreemptDisable>`
  里取当前线程的序列从上面三条变成一条 `movq %gs:...CURRENT_INSTANCE...(%rip), %rax`。
- QEMU：两个镜像都跑到 `--- end ---`，dbg_1 上 `probes done (11 killed, 0 survived)`、
  `512 channels made before the quota`、`bootfs checks passed`。
  **这一轮是在临时反向应用 faa117f7（LAPIC 定时器按 CPU 取设备）的工作区上跑的**：
  以 HEAD（含 faa117f7）为底时，无论带不带本改动，dbg_1 都会在 fault_probe 阶段卡住——某个新启动的
  探针线程始终没被调度，shell 一直等它；反向应用 faa117f7 后带本改动两个镜像都通过。
  也就是说那个卡住来自 faa117f7，与本改动无关，已另行告知。

**故意没做**
- 没修 `Thread::Current::tid()` 和 `PmNode::Current::hard_affinity()` 里的 `CURRENT_INSTANCE->field`
  （读模板副本的指针）。前者已记在 `docs/repairs/260924/kernel-task.md` 第 4 条并交接给别的会话，
  后者没有调用者。两处都是把 `CURRENT_INSTANCE` 当普通对象用；这次的改动之后正确写法是
  `Current::get()->field`。
- `get(CpuNum)` 没改：它读的是指定 CPU 的槽，不存在「读哪个 CPU」的竞争，返回引用也仍然合理。
