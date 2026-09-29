# kernel/mm 待修项（2026-09-28）

性能项，按 CLAUDE.md「Working with the user」：功能基本完善之后再做，动手前和用户逐项讨论，用基准数据选方案。

## 1. 一棵 VMO 父子树只有一把锁，可能成为争用点（先测量，再决定拆不拆）

`42d8f41f` 把 VMO 的锁从 `VmObject` 挪进了 `VmCowPages`（`kernel/mm/include/ours/mm/vm_cow_pages.hpp` 的 `m_lock`），
为的是让 Slice 和父对象共用同一棵页树时锁住的是同一个 mutex（K20）。于是同一棵树上的所有对象共用这一把锁：
父对象、它的每个 Slice、每个 Slice 的 Slice，以及它们在各个地址空间里的全部映射（`VmObject::m_mappings` 也归这把锁管）。

这把锁的持有者包括：
- 缺页路径：`VmMapping::page_fault()` 在地址空间锁之内取它，查页、必要时分配并插页、装页表项；
- `read_user()`/`write_user()`：整段拷贝期间持有，缺页时放锁再重取（`VmObjectPaged::copy_page_by_page()`）；
- `map_paged()`（`vma_map` 带 MapRange、userboot 装镜像）、映射的建立和拆除。

**担心的场景**：BootFS 是一个整块 VMO，U12 之后 init 会为每个文件发一个 Slice，所有进程读 /boot 的缺页都落在这一把锁上；
vDSO 也是一个 VMO，被每个进程映射。多核上并发缺页、并发 vmo_read 时，它们会不会在这把锁上排队，现在没有数据。

**要先回答的问题**（写进 `benches/`）：
- 单个 VMO 被 N 个线程（N = 1、2、4、8，分布在不同 CPU）并发 `vmo_read` 不同页，吞吐和延迟分布随 N 怎么变；
- 同一个 BootFS 的 N 个 Slice 被并发缺页（经 `vma_map` 映射后按页触碰），和 N 个独立 VMO 对比；
- 持锁时间的分布：缺页路径里分配页（`alloc_pages`）和装页表项各占多少，拷贝路径每次持锁拷多少字节。

**数据出来之后可以讨论的方向**（不预设结论）：
- 不拆：争用不明显就保持现状，一把锁最简单，锁序也最少；
- 缩短持锁：把分配页移到锁外（先分配、再持锁插入、冲突时释放），或者拷贝时按页放锁；
- 拆锁：页树用更细的锁（按区间或按页），映射链表单独一把锁——代价是锁序变复杂，要重新论证不变量；
- 读多写少：已提交且不再变化的页（BootFS、vDSO 都是 Pinned）走无锁查找，例如 RCU 式的页树读取。

正确性不能靠基准证明：任何拆分都要先写清每把锁保护哪条不变量、新的锁序，再用 lockdep 和压力测试佐证。

对照：Zircon 早期同样是整棵层级共用一把锁（`VmHierarchyState`），后来为了多核扩展改成了每个 `VmCowPages` 一把锁、
按层级顺序加锁；可以对照它的演进和当时给出的理由。
