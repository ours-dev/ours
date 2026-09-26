# kernel/mm 待修项（2026-09-25）

## 1. `do_handoff()` 之后 EarlyMem 还在发内存，页表和 buddy 抢同一批物理页（NUMA 开启时必挂）

**现象**：`out/numa.x86`（`mm_numa = true`，QEMU `NUMA=1`、8 CPU、1 GiB、四节点）启动到
`Released 16 regions, 255669 frames (998 MiB) to the buddy allocator` 之后立刻死在一次写缺页：

```
[error] Unhandled page fault: vector=14, error_code=0x2
[error]   cr2=0xffffff8800016fff
[error]   rip=0xffffff8840024525 (+0x24525)  cs=0x10  rsp=0xffffff884008c3b8
[error]   rax=0xffffff8800000000  rcx=0x17000  rdx=0x17000  rsi=0xffffff803fe60000
```

`error_code=0x2` 是「不存在 + 写 + ring 0」。`+0x24525` 在 `memmove` 里
（`movb %dil, -0x1(%rax,%rcx)`）：`rax` 是目的地 `0xffffff8800000000`，正好是
`ARCH_CPU_LOCAL_VIRT_BASE`（`KERNEL_ASPACE_BASE + ARCH_PHYSMAP_SIZE`），`rcx = 0x17000` 是
`CpuLocal::Mapper::unit_size()`，`rsi` 是 physmap 里那份 replica。所以挂在
`CpuLocal::init()` 的

```cpp
std::copy_n(reinterpret_cast<u8 const *>(replica), Mapper::unit_size(), base);
```

第一条存储指令上——`memmove` 见 `dst >= src` 就走倒序拷贝，所以先写最后一个字节
`base + 0x17000 - 1`。`out/bringup.x86`（单节点）不复现。

**原因**：CpuLocal 那个窗口的叶级页表被清零了。用 QMP 在 fault 现场顺着 `CR3=0x3febb000` 走下去：

```
PML4[511] @0x3febbff8 = 0x000000003fea0027   -> PDPT 0x3fea0000
PDPT[0x20] @0x3fea0100 = 0x000000003fe9c027  -> PD   0x3fe9c000
PD[0]      @0x3fe9c000 = 0x000000003fe9b027  -> PT   0x3fe9b000
PT[0..31]  @0x3fe9b000 = 全 0
```

上面三级都在，叶级整页是 0——页表建起来过（`build_dense()` 把 23 个页分 16+4+2+1 四次映射，
每次都返回 Ok），之后被人整页清零了。

清零的人是 buddy 分配器，因为这块物理页被发了两次：

1. `init_pmm()` 最后调 `EarlyMem::do_handoff()` → `free_unused_memory()`，把 bootmem 里
   **还标着空闲**的区间全部 `free_frames()` 交给 buddy。注意它**没有**在 bootmem 里把这些区间
   标成已用，bootmem 的位图原封不动。
2. 紧接着 `start_kernel` 调 `mm::CpuLocal::init()`（`kernel/top/main.cpp:151`）。它经
   `ArchVmAspace::map` → `PageTable::map_pages` → `alloc_table` → `alloc_page_table()`
   要页表页，而 `alloc_page_table()` 的分支条件是 `pmm_enabled()`：

   ```cpp
   auto alloc_page_table(usize size, usize alignment, NnId node) -> PhysAddr {
       if (pmm_enabled()) { ... alloc_frames(...) ... }
       auto *table = EarlyMem::allocate<u8>(size, alignment, node);
       ...
   }
   ```

   `pmm_enabled()` 是 `get_init_stage() >= InitStage::Pmm`，而 `set_init_stage(InitStage::Pmm)`
   在 `init_cache()` 里（`kernel/mm/init_pmm.cpp:109`），**排在 `CpuLocal::init()` 之后**。
   所以这里走的是 EarlyMem 分支，TopDown 从内存顶端发出 `0x3fe9c000`、`0x3fe9b000`——紧贴 physload
   自己那几张表（`0x3fe9e000`/`0x3fe9f000`/`0x3fea0000`）下面。
3. 这两页在第 1 步已经进了 buddy 的空闲链。`build_dense()` 接着为 unit 1..15 调
   `alloc_frames(nid, GAF_BOOT, order)`，`GAF_BOOT` 含 `Gaf::Zero`；NUMA 下节点 3 的内存就在顶端，
   于是某个 unit 拿到了 `0x3fe9b000`，顺手清零——把还在用的叶级页表擦了。

`pmm.hpp` 里 `alloc_page_table()` 的注释把这条不变量写反了：

> Before the buddy this comes from EarlyMem, which never gives it back:
> do_handoff() releases what bootmem still calls free, and an allocation is not free.

「分配过的就不是空闲的」只在 **`do_handoff()` 之前**分配才成立。`do_handoff()` 跑完之后，
bootmem 认为空闲的东西已经全给了 buddy，此刻再从 EarlyMem 拿一页，bootmem 里标已用也没用——
buddy 手上那份副本不会因此消失。`do_handoff()` 和 `set_init_stage(InitStage::Pmm)` 之间这段窗口里，
**每一次** `alloc_page_table()` 都是一次双重分配。单节点之所以躲过去，只是因为 buddy 从低端发页，
碰不到 EarlyMem TopDown 占的顶端。

**已经验证过的两条**（都不是修法，记下来省得别人再试一遍）：

- 把 `set_init_stage(InitStage::Pmm)` 提到 `do_handoff()` 后面：缺页没了，换成
  `#GP  rip=+0x37fd9`。因为 `SparseMemMap::Impl::allocate()` 也是按 `pmm_enabled()` 分支的，
  提前翻会让它在堆还不存在时走 `new`——`init_cache()` 里那句「It waits for the heap rather than
  for the page allocator」说的就是这件事。
- 只给 `alloc_page_table()` 换一个「buddy 已接管」的判据、让它这段窗口里走 buddy：缺页同样没了，
  也是同一个 `#GP  rip=+0x37fd9`，落在
  `PmNode::alloc_frames(Gaf, usize, BitSet<32> const&)` 里 `movq (%r14,%r13), %rdi`，
  `r14 = 0x22a840f00000004` 是非规范地址。这正是 `alloc_page_table()` 注释预告过的重入：
  「made an edit made *while* allocating from the buddy re-enter it」。所以页表页在这个窗口里
  不能从 buddy 拿。

**照谁改**：Linux 的 `alloc_low_pages()`（`arch/x86/mm/init.c`）按 `after_bootmem` 分支，和这里一样；
差别在于 Linux 的 `memblock_free_all()`（EarlyMem::do_handoff 的对应物）跑完之后
`after_bootmem` 就是真的——`mm_core_init()` 里 `memblock_free_all()` 紧跟着
`mem_init_print_info()`，而 `slab_is_available()` 之类的判据各管各的，不会像这里一个
`InitStage` 同时兼任「buddy 上了」和「堆上了」两件事。

方向有三个，选哪个是设计决定，不要顺手改：

1. 把 `InitStage` 拆开，或者给 `pmm_enabled()` 之外再加一个 `heap_enabled()`，让
   `alloc_page_table()` 看前者、`SparseMemMap::Impl::allocate()` 看后者。最贴近 Linux。
2. 让 `do_handoff()` 把交出去的区间在 bootmem 里标成已用（或者干脆让 EarlyMem 之后一律拒绝分配并
   断言），这样窗口里的分配变成一次可见的失败而不是静默踩内存。但这只把症状变响，`CpuLocal::init()`
   依然拿不到页表页，还得配合第 1 或第 3 条。
3. 把 `CpuLocal::init()` 挪到 `do_handoff()` 之前。它现在排在 buddy 之后是因为 unit 的
   frame 要按节点从 buddy 拿（`build_dense()` 里的 `alloc_frames(nid, ...)`），所以这条要先解决
   「按节点拿内存」在 EarlyMem 上怎么表达。

**严重程度**：`mm_numa = true` 的镜像根本起不来，比下面那条潜伏的锁问题急。

## 2. VMO 的页仓库由两把不同的锁保护：缺页走地址空间的 SpinLock，读写与提交走 VMO 的 Mutex

**现象**：潜伏。一个 VMO 只被一个地址空间映射、且不被 `read()`/`write()`/`commit_range()` 同时访问时触发不到。

**原因**：`VmCowPages::m_pages`（一棵 B 树）被两条路径改写：

- 缺页：`VmAspace::page_fault_internal()` 持 `VmAspace::m_lock`（`sync::SpinLock`，`vm_aspace.cpp:165`），
  经 `VmMapping::fault()` 调 `paged->make_cursor()` 和 `require_page()`（`vm_mapping.cpp:485-495`），分配并插入页；
  `VmMapping::map_paged()` 同样直接用 cursor（`vm_mapping.cpp:191`）。
- 提交、读、写：`VmObjectPaged::commit_range_internal()` 和 `read_write()` 持 `VmObjectPaged::m_mutex`。

两把锁互不相干，所以：同一个 VMO 被两个地址空间映射时，两边同时缺页会并发插入同一棵树；一边缺页、一边
`write()`，同理。文件系统计划里第一个会碰上的是 K16（所有进程共用一个 vDSO VMO），其次是 K17/K21
（launcher `vmo_write` 一个 VMO 再映射它）。

**不能直接在缺页路径里拿 VMO 的 Mutex**：缺页持的是 SpinLock，在 SpinLock 下拿会睡眠的 Mutex 是错的。

**照谁改**：Zircon 的缺页在碰页之前先拿 VMO 的锁：`VmMapping::PageFault()`
（`zircon/kernel/vm/vm_mapping.cc:1376`）对分页 VMO 取 `Guard<CriticalMutex>{paged->lock()}`，再进
`PageFaultLockedObject()`。地址空间的锁和 VmCowPages 的锁都是可睡眠的 `CriticalMutex`
（`zircon/kernel/vm/include/vm/vm_aspace.h:336`，`vm_cow_pages.h:176`）。所以要么把 `VmAspace::m_lock` 换成
可睡眠的锁（缺页本来就在可睡眠上下文里），缺页时再取 VMO 的锁；要么让 VmCowPages 自带一把自旋锁专管
m_pages。前者与 Zircon 一致，推荐。

**必须在 K16 之前修掉。**
