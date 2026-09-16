# CpuLocal — 待修

---

## 1. `Dyn::allocate()` 持自旋锁跨 `grow()`

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

读侧已经不参与这把锁了（见 `docs/changes/260915/mm-cpu-local.md`），所以剩下的纯粹是
写侧：`allocate_locked()` 扫位图的那段读-改-写。

---

## 2. `for_each` / `fold` 用 online mask，`with_current` 用 has_unit

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

## 3. `PerCpu::handle()` 把裸 handle 漏了出去，而且没有使用者

`PerCpu` 的类注释说：

> Linux relies on sparse and a `__percpu` annotation to stop callers dereferencing the
> equivalent value; here the type simply never offers a way to.

但 `handle()`（`cpu_local.hpp:315`）返回 `VirtAddr`，拿到就能 `reinterpret_cast` 解引用，
落点正是链接器那份模板。全树已经没有调用者（原先 `prof_console.cpp` 用它跟 `NOWHERE`
比较，后来改成了 `operator bool`）。

删掉它，注释所说的性质才真的成立。
