# kernel/mm — 待修

2026-09-03 对 kernel/mm 做的一次完成度审查。结论：**物理内存那一层是完整的，
虚拟内存那一层只有"预先映射"能用，按需分页整个不存在。**

能用的：伙伴分配器、zone/node、memmap、cpu-local、对象缓存与 kmalloc、
带 `VmMapOption::Commit` 的即时映射、VMA 树与预留、`ProtectionRanges`、
`VmaSet` 那套带增广的 B 树空隙查找。

下面按依赖顺序排，前面的不做后面的做不了。

---

## 1. 页面仓库做完之后新暴露的几件（原第 1 条已修，见 `docs/changes/260903/mm.md` 第 7 节）

`VmCowPages` 现在真的记页了，随之而来几件独立的小账，按严重程度排（commit 的 O(n²) 外层循环已修，见 `docs/changes/260925/mm.md`）：

- **`~VmCowPages()` 不释放自己拥有的页。** 改前页从没被记下来，谈不上释放；现在仓库知道
  自己有哪些页了，才第一次有条件在析构时还给伙伴分配器。要连带把 `VmObjectPaged::decommit()`
  一起想清楚（对应 `Zircon VmCowPages::DecommitRangeLocked()`）。不修就是每个 VMO 死掉时
  泄漏它的全部页。
- **`VmPageMap` 一页一个 B 树条目。** 1 GiB 的 VMO 就是 26 万个条目，顺序遍历每页一次
  O(log n) 查找。`Zircon VmPageListNode` 把 16 页并成一个树节点正是为了摊掉这个，
  键改成 `index >> 4`、值改成 16 槽数组即可，接口不用动。

## 1b. `ktl::xarray::XArray` 是一个编不出来的空壳（属 ktl，不属 mm）

`load()` / `store()` / `erase()` / `make_entry()` 四个方法**只有声明，从来没有定义**，
调用任何一个都是链接错误：

```
ld.lld: error: undefined hidden symbol:
  ktl::xarray::XArray<ktl::Allocator<char>, ...>::load(unsigned long)
```

本该定义它们的 `kernel/lib/ktl/include/ktl/xarray.tcc` 没有被任何文件 include，而且里面写的是
`Xarray`（小写 a）、`kMaxSlots`、`kSlotsBits`、`index_`、`owner_`、`node_` 这些当前
`xarray.hpp` 里不存在的名字，`Cursor` 也没声明过 `expand` / `create_slots` / `load` /
`descend` / `at_root` 中的任何一个——就算 include 进来也编不过。此外 `Entry::is_null()` 对
`BitFields` 做 `reinterpret_cast<usize>`，`cast_to<T>()` 把值当指针再解引用，`Node::set_entry()`
标着 `const` 却写 `slots`，`alloc_node()` 的形参个数两处不一致。

`VmPageMap` 已经改用 ustl 的 B 树，所以 mm 不再依赖它。剩下的选择是补完还是删掉，
属于 `kernel/lib/ktl` 那条线。**在补完之前，不要有新的调用方**——编译期没有任何信号，
只有链接时才会发现。

## 2. 缺页做通之后剩下的两道墙（原第 2 条已修，见 `docs/changes/260903/mm.md` 第 8 节）

`VmMapping::fault()` 实现了，`page_fault_internal()` 那两处（空 `Arc` 解引用、字面量 `0`
当 `VmFault *`）也修了。但**硬件缺页仍然到不了这里**，两处都不属于 mm：

- **`Thread::Current::fault()` 挡掉了所有内核地址**（`kernel/kernel/task/thread.cpp:712`）：

  ```cpp
  if (mm::is_kernel_address(va)) {
      return Status::NotFound;
  }
  ```

  于是 `#PF` → `x86_handle_page_fault()` → `Thread::Current::page_fault()` 这条链，对内核
  地址一律在第一行就返回，`VmAspace::page_fault()` 根本不会被调到。当前没有用户地址空间，
  所以**没有任何一个真实缺页能走到 VMM**。第 8 节那次验证只能改用
  `VmAspace::soft_fault()` 直接驱动。

  这道挡板在"内核映射一律预先 Commit"的前提下是合理的（内核缺页即是 bug）；那个前提
  现在不再成立了。修法要么按地址空间分派（内核地址走 `VmAspace::kernel_aspace()`），
  要么保留挡板但明确写下"内核不做按需分页"这条约束——两者都要 task 那条线拍板。
  **不定这件事，按需分页就只有内部接口能用。**

- **`PageTable::map_pages()` 忽略 `MapControl`**（`kernel/arch/x86/mm/page_table.cpp:86`）：
  `(void)control;`，注释写着 "engine always rejects a page that is already mapped"。
  于是没有 overwrite / skip 语义，对一个已装好的表项再 `map()` 一次必得 `AlreadyExists`。
  `VmMapping::fault()` 现在靠先 `query()` 绕开它（缺页可能落在别的 CPU 刚装好的页上，
  或者本 CPU 的陈旧 TLB 上），但这属于替 arch 层打补丁。属 arch，不属 mm。

## 5. VMA 没有遍历能力

只有 `find_mapping_locked()` 能找叶子，没有找子区域的，也没有对外枚举。地址空间转储、
范围操作（commit/decommit/prefetch）、拆除时的记账都无从下手。
对应物是 `Zircon VmAddressRegion::EnumerateChildren()`。

## 6. VMO 的数据操作全缺

`VmObjectPaged` 的 `decommit`、`take_pages`、`supply_pages`、`read`、`write` 都是
`Unimplemented`；基类 `VmObject` 的 `dirty_pages`、`write_back_preamble/postamble` 也是。

## 7. vmalloc 不存在

`init_vmm.cpp` 的 `init_heap()` 是空函数。没有虚拟连续的内核分配器。

## 8. 回收只是骨架

入队/出队（第 6 节）、代际老化（第 7 节）与 runner 的状态机（第 8 节）都做了。
剩下的都是"要等别的东西先存在"的，按依赖排：

**8.1 没有回收方，所以没有一页能被真的释放。**
`advance_lru_locked()` 在环满时把最老的一代整体折进下一代，而不是挑页扔掉——因为没有地方可扔：
没有交换分区、没有供页者、没有压缩。这不是遗漏，是当前系统的边界。内存紧张时缺页仍然只能
直接失败，在第 1、2 条（按需分页）做完之后这会是第一个暴露的短板。

**8.2 没有人往队列里填页。** 三个 setter 与 `remove()` 都好了，但要等 `VmCowPages` 变成真正的
页仓库（第 1 条）才有调用方。`mark_accessed()` 同理，它的输入是页表的 accessed 位。

**8.3 runner 线程没有启动，`run()` 的循环体从未运行过。**
`start_runners()` 有定义但故意什么都不启动（理由写在函数体的注释里：一个线程今天只会老化一个
永远空着的环）。因此 `run()` 里 `sleep_until` + `rotate_reclaimable_queues` 那一段只有静态
可读性，没有运行时证据。打开它的前置条件是 8.1 与 8.2 都成立——有页可排序，且有回收方读这个
排序。打开的时候一并要做的两件小事：

- 关停改为等在 `sync::Event` 上而不是可中断的 `sleep_until` 上，让关停立刻生效而不是最坏等
  一个轮转周期。注意 `sync::Event` 没有构造函数，`gktl::Canary` 与 `Flags` 都不会被初始化，
  默认构造一个能不能用需要先确认。
- 老化循环按 Zircon 的分工搬到 `MruRunner`，`LruRunner` 拿"回收方要求看到更老的页时退休最老
  一代"那一半。
- `m_min_rotate_time` / `m_max_rotate_time` 现在取的是 Zircon 的 10 秒 / 60 秒，是占位值，
  该按实际回收行为重新定。

## 8b. `PageQueues` 上不了 host 单元测试

`PageQueues` 的每个入口都取 `sync::SpinLock`，而这把锁的 `acquire()/release()` 是 out-of-line
定义在 `kernel/kernel/sync/spin_lock.cpp` 里的，依赖 `mm::CpuLocal::current_cpu()` 和
`GKTL_COUNTER`；`Guard<SpinLock, IrqSave>` 还要求 `arch::AutoInterruptDisabler` 是完整类型。
开发机是 arm64：带 `-DARCH_X86` 时 x86 的 `cli/pushfq` 内联汇编汇编不过，不带时
`arch/interrupt.hpp` 的兜底分支里 `ArchCpuState` 只有声明，`AutoInterruptDisabler` 的成员
定义不出来。

于是这个类目前只能靠临时的内核内探针验证，探针提交前就删掉，回归就无从谈起。真要修，
方向是把队列的簿记与锁解耦（让 `*_locked` 那一层不引用锁类型），或者给 host 工具链补一组
`arch` 的空实现，两者都不属于回收这条线。

## 10. `ustl` 的 `BTree::erase(iterator)` 名不副实（属 ustl，不属 mm）

签名是 `auto erase(iterator pos) -> iterator`，和 `std::map` 一样，读起来像是 O(1) 摊还的
"删除并交回后继"。实现却是（`system/lib/ustl/include/ustl/collections/detail/btree.hpp:1376`）：

```cpp
auto next = pos; ++next;
next_key.emplace(key_of(*next));
erase(key);                                                        // O(log n)
return next_key.has_value() ? lower_bound_node(*next_key) : end();  // O(log n)
```

即"记下后继的键 → 按键删 → 重新查回去"。功能正确，但**成本是 O(log n) 而不是摊还 O(1)**，
所以任何边遍历边删的循环都是 O(k log n)，而标准库同样的写法是 O(k + log n)。

`erase(iterator, iterator)` 同理，它就是循环调 `erase(first)`。

要做成真的，需要节点算法在重平衡过程中维持一个有效的后继位置，而不是事后按键找回来。
这是共享容器里的精细改动，应当由 ustl 那条线认领，不要在 mm 里顺手改。

## 11. 零散

- `VmAspace::clone()`、`MemCache::alias()` 是 `Unimplemented`，属于尚未需要的功能。
- `VmMapping::m_policy`（`MemoryAllocationPolicy`）存了但没有任何代码读，文件里已有 TODO。
- `VmArea::reserve()` 的文档注释是 `protect()` 的复制品（"Update rights of the subset of the
  area"），但它做的是别的事。

## 10. 对象缓存：`m_node_caches` 为空是个二义状态，缺失的初始化不会被发现

严重度低（不影响当前功能），但它是一类缺陷的放大器，值得单独一条。

`MemCache::allocate(Gaf, NnId, CpuCache *)` 开头是：

```cpp
auto *const node_cache = m_node_caches[nid];
if (!node_cache) [[unlikely]] {
    // This cache keeps no slabs on that node.
    return nullptr;
}
```

这个 nullptr 同时表示两件完全不同的事：

1. **合法**：这个缓存按 `SLAB_NODES` 只在部分节点上放 slab，问到别的节点上就该说没有。
2. **缺陷**：`NodeCache::on_mem_cache_init()` 根本没跑，整个 `m_node_caches` 还是构造函数
   留下的一排零，这个缓存彻底不可用。

两者在这一行上完全同形，于是第二种情况既不断言也不报错，只是让该缓存的每一次分配都返回
nullptr，把错误推迟到第一个真正检查分配结果的调用方——中间隔了几个初始化阶段，现场离成因
很远。2026-09-03 那次 `NodeCache::on_mem_cache_init()` 被复制粘贴吃掉的缺陷正是这样表现的：
`MemCache::init()` 返回 `Ok`，`init_kmalloc()` 把八个 size class 全部"建成功"且
`DEBUG_ASSERT` 一声不吭，内核堆整个是死的，而第一条错误日志说的是 ThreadCache 建不出来。
详见 `docs/changes/260903/mm.md`。

修法：在 `MemCache::init()` 末尾（登记进 `GLOBAL_CACHES` 之前）加一条 `DEBUG_ASSERT`，
逐位比对 `SLAB_NODES` 与 `m_node_caches`——`SLAB_NODES` 里置位的每个 nid 都必须拿到一个
非空 NodeCache，没置位的必须是空。这样"初始化步骤漏掉"当场就响，而 `allocate()` 里那条
nullptr 快速返回也就重新只剩下第 1 种含义，注释才名副其实。

`create_boot()` 走的是同一个 `init()`，所以 kmalloc 的 size class 一并被覆盖。

一并可做（不值得单独一次构建—启动循环）：`MemCache::create()`（约 626 行）里还留着一处
`auto status = ...; if (Status::Ok != status)`，本文件其余地方已统一成单行
if-with-initializer，顺手扫掉。

## 11. 启动当前卡死在 `prof_init()` 的 `build_fde_index()`（交叉引用，不属 mm）

**这条不是 mm 的，记在这里是因为它挡住了 mm 剩余路径的运行时验证。**

实测定位（`kernel.dev.dbg_1.obi`，临时 `log::trace` 打点，打点未提交）：

1. `labour_routine()` 里 `init_platform()` 正常返回；
2. `set_init_level(gktl::InitLevel::Platform)` 开始跑该级别的 init hook，
   `free_bootstrap16_buffer()`（`kernel/arch/x86/kernel/bootstrap.cpp:130`）释放
   D000/E000/F000 三页后正常返回；
3. `prof_init()`（`kernel/commands/prof.cpp:88`）进入，打印 `eh size=28600`，
   随后**卡在 `build_fde_index()` 里不再返回**，没有断言、没有异常、QEMU 存活。

把 `free_bootstrap16_buffer()` 临时改成空操作，卡死点不变——所以与那三次释放无关。
嫌疑集中在 `unwind::FdeIndex::build()` 对 `.eh_frame` 的遍历：CFI 记录走不动
（长度字段为零、或某种记录形态没被处理）就是一个不推进的循环。`kernel/lib/prof` 树上
有另一会话的大量未提交改动，应由该会话认领。

**对 mm 的影响**：`EarlyMem::reclaim_init_area()` 这条路径当前不可达，
2026-09-03「`reclaim_init_area()` 和它的调用者搬出 `.init` 段」那条改动只有静态证据
（`llvm-nm` 的符号地址），没有运行时证据。这个卡死解掉之后应当补一次实测。

## 12. `free_bootstrap16_buffer()` 释放了页却没有解除映射

`kernel/arch/x86/kernel/bootstrap.cpp`：`create_bootstrap16()` 为 AP 启动建了两处恒等映射
（`BOOTSTRAP16_BUFFER_BASE` 起 `MIN_BOOTSTRAP16_BUFFER_SIZE`，以及必要时另拷贝的 GDT 页），
而 `free_bootstrap16_buffer()` 只把三页交还给伙伴分配器，没有 unmap。

页被重新分配出去之后，这两处恒等映射仍然可写地指着它们——一个跨所有者的可写别名，
而且带 `Discache`。当前不暴露（没人再走那些地址），但它和第 10 条是同一种病：
释放本身无声，代价由下一个使用者付。

同一处还有一点值得核对：缓冲区是 `mm::EarlyMem::allocate()`（bootmem）拿的，
却用 `mm::free_frames()` 还给伙伴分配器。如果这几页在 handoff 时是作为 reserved
而从未进入伙伴分配器，这就是在释放一个它从未拥有的页。已确认它不是当前的卡死点
（见第 11 条），但没有确认它是对的。

非 mm 所有，同样列在这里备查。
