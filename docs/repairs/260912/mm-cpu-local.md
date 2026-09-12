# CpuLocal — 待修

---

## 1. `holds()` 在 debug 构建里会自锁

`CpuLocal::resolve()` 的两个重载都以 `DEBUG_ASSERT(Self::holds(handle), ...)` 开头
（`kernel/mm/include/ours/mm/cpu_local.hpp`），而 `holds()` 对不在静态区的 handle 会走
`DYN->owns(handle)`，后者第一行就是 `Guard guard{m_lock}`（`kernel/mm/cpu_local.cpp:576`）。

于是有这么一条环：

```
Dyn::allocate()                     取 m_lock   (cpu_local.cpp:636)
  → grow()                                      (cpu_local.cpp:639)
    → Mapper::alloc_chunk()
      → alloc_frames()
        → PmZone::alloc_frame()
          → m_frame_cache.with_current(...)     (pm_zone.cpp:59)
            → PerCpu::ref_mut()
              → CpuLocal::resolve(handle)
                → DEBUG_ASSERT(holds(handle))
                  → Dyn::owns()     再取 m_lock  ← 自锁
```

`m_frame_cache` 是 `CpuLocal::PerCpu<PcpuCache>`，所以只要这次分配落在动态区，环就闭合。
`bringup.x86` 当前编的是 `KERNEL_DEBUG_LEVEL=0`，assert 被编掉了，但同一个 out 目录下
`KERNEL_DEBUG_LEVEL=1` 和 `=2` 的产品都在构建，那两个里这条路是活的。

问题不只是自锁。**一个只在 debug 下存在的断言，不该在热路径上取锁**——`resolve()` 压在每一次
per-CPU 访问下面，让它取一把全局自旋锁，等于给 debug 构建换了一套并发行为，而调试构建本来
是用来复现 release 行为的。

Linux 的对应物 `pcpu_chunk_addr_search()` 不加锁，因为 chunk 链表是 append-only 的：
只往尾部加，从不删中间节点，所以读侧可以无锁遍历。ours 的 `m_dynchunks` 目前也只有
`push_back`（`grow()` 里），没有任何地方摘链——也就是说这个性质已经成立，只是没被写出来、
也没人依赖它。

修的方向（未定）：
- 把「chunk 链表 append-only」变成明写的不变量，`owns()` 改成无锁遍历；或者
- `holds()` 只回答静态区，动态区的越界检查换成别的手段（比如让 `PerCpu` 自己记住区间）。

---

## 2. `Dyn::allocate()` 持自旋锁跨 `grow()`

`cpu_local.cpp:636` 取 `m_lock`，`:639` 在锁内调 `grow()`。`grow()` 会
`Mapper::alloc_chunk()` → `alloc_frames()`，并在 paged 布局下对每个 unit 调一次
`ArchVmAspace::map()`，也就是 `MAX_CPU` 次，路上还可能建页表。

`sync::SpinLock` 的默认选项是 `Option::DisablePreempt`
（`kernel/include/ours/sync/spin_lock.hpp:61`），所以这一整段是关着抢占跑的。持自旋锁跨
这么长的操作本身就不对，而且它把每个别的 CPU 都排在一个它们并不需要的慢操作后面。

这一条是 `.agents/tasks/cpu-local/OPEN-grow-serialisation.md` 记过的，那份文件写在
`docs/repairs/` 这套结构之前。里面给了想要的形状（lock-free 快路径 + 单写者的 grow gate +
generation 计数），以及卡住它的原因：`allocate_locked()` 目前不是 lock-free 的，它扫位图找
连续空位再置位，是跨多个字的读-改-写。那份文件还列了三个可选方案（bump pointer 不回收 /
bump + per-size-class Treiber 栈 / 位图上做字级 CAS），倾向 B，未决。

第 1 条和这一条是同一把锁，修的时候应该一起考虑：如果链表变成 append-only 且读侧无锁，
`owns()` 和 `holds()` 就一起解决了。

---

## 3. `for_each` / `fold` 用 online mask，`with_current` 用 has_unit

`cpu_local.hpp:207`、`:216`、`:353` 三处遍历 `Cpu::online_mask()`，而 `with_current()`
在 `:199` 断言的是 `has_unit()`，且头文件里专门写了为什么不能用 online mask：

> has_unit(), not Cpu::online_mask(): what this needs is a valid offset for the calling CPU,
> which is exactly what CpuLocal tracks. The global online mask answers a later question --
> the boot CPU is not in it until the end of init_arch_early()

同一个论证对 `for_each` 成立，但没有跟过去。后果是有一段真实存在的窗口：`CpuLocal::init()`
里的 `bootstrap(CpuMask::of(0))` 给 boot CPU 记上了 unit，而 `Cpu::set_state(Online)` 要等到
`init_arch_early()` 末尾（`kernel/arch/x86/kernel/init.cpp:90`）。在这两者之间，
`CpuLocal::for_each()` 会**一个 CPU 都不访问**，静默返回。`MemCache::init()` 就在这个窗口里。

反过来的窗口不存在：AP 的 `x86_start_nonboot_cpu()` 第一件事就是
`CpuLocal::init_percpu()`（`kernel/arch/x86/kernel/mp.cpp:126`），所以不会出现「在线但没有
unit」。也就是说两个谓词之间是单向包含，选 `has_unit()` 严格更安全。

要决定的是语义：`for_each` 想遍历的是「所有跑着的 CPU」还是「所有有存储的 CPU」。
Linux 的 `for_each_possible_cpu()` 遍历的是后者，理由正是 per-CPU 存储按 possible 分配。

---

## 4. 按 CPU 号解析时不检查该 CPU 有没有 unit

`resolve(handle, cpu)`（`cpu_local.hpp:97`）只断言 `cpu < MAX_CPU`，
`access(t, cpunum)`（`:291`）同样。没有 unit 的 CPU 其 `CPU_OFFSETS[cpu]` 是
`init_early()` 清成的 0，于是 `handle + 0` 解析回模板自身。

模板是 `.init.data.cpu_local`，`reclaim_init_area()` 启动后会把它还给分配器。也就是说这不是
读到过期的默认值那么轻——是往一块已经交给别人的内存里写。而且因为 handle 合法、地址也合法，
`holds()` 查不出来，不会有任何报错。

`has_unit()` 已经存在且正是为此，这两个断言应该用它。

---

## 5. `PerCpu::handle()` 把裸 handle 漏了出去，而且没有使用者

`PerCpu` 的类注释说：

> Linux relies on sparse and a `__percpu` annotation to stop callers dereferencing the
> equivalent value; here the type simply never offers a way to.

但 `handle()`（`cpu_local.hpp:315`）返回 `VirtAddr`，拿到就能 `reinterpret_cast` 解引用，
落点正是第 4 条说的那块模板。全树已经没有调用者（原先 `prof_console.cpp` 用它跟 `NOWHERE`
比较，后来改成了 `operator bool`）。

删掉它，注释所说的性质才真的成立。
