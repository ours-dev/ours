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
