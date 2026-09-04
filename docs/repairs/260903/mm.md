# kernel/mm — 待修

2026-09-03 对 kernel/mm 做的一次完成度审查。结论：**物理内存那一层是完整的，
虚拟内存那一层只有"预先映射"能用，按需分页整个不存在。**

能用的：伙伴分配器、zone/node、memmap、cpu-local、对象缓存与 kmalloc、
带 `VmMapOption::Commit` 的即时映射、VMA 树与预留、`ProtectionRanges`、
`VmaSet` 那套带增广的 B 树空隙查找。

下面按依赖顺序排，前面的不做后面的做不了。

---

## 1. `VmCowPages` 还不是页面仓库（按需分页的前提）

- `VmPageMap m_pages` 声明了，全树没有一处读或写。
- `VmCowPages::Cursor` 的构造函数把 `offset` 和 `size` 两个参数直接丢掉，`m_offset`
  从不初始化，也从不前进——它是个没有位置的游标。
- `is_type_page()` / `is_type_zero()` 只有声明没有定义（没人调用，所以还不是链接错误）。
- `require_owned_page()` 从不查"这个偏移已经有页了吗"，每次调用都新分配一页，然后谁也不记下来。

后果是同一个偏移要两次会得到两页，而且都不属于这个 VMO。现在不暴露，只因为所有映射都是
一次性 Commit 之后再不缺页。按需分页要求"第二次访问同一页拿到同一页"，这条现在不成立。

修法：游标带上偏移并能前进；`require_*_page` 先查 `m_pages`，命中即返回，未命中才分配并
`insert_page`；补上两个只有声明的判定函数。

## 2. `VmMapping::fault()` 是桩，按需分页整个不工作

```cpp
auto VmMapping::fault(VmFault *vmf) -> ustl::Pair<Status, usize> {
    m_canary.verify(MAPPING_MAGIC);
    return {Status::NotSupported, 0};
}
```

它上面那一整套——异常分发、`VmAspace::page_fault_internal()` 的重试循环、
`PageRequest`/`ShouldWait` 机制——全都是围绕一个不存在的操作写的。

同一条路径上另有两处会在第一次缺页就触发：

- `page_fault_internal()` 在 `m_last_fault` 还是空 `Arc` 时就调 `m_last_fault->check_range()`。
- 它给 `fault()` 传的是字面量 `0` 当 `VmFault *`。

## 3. PageSource / PageRequest：建议**暂时不做**

`PageRequest::wait()` 返回 `Unimplemented`，`PageSource` 是空类，
`VmCowPages::Cursor::create_read_request()` / `create_dirty_request()` 都是 `Unimplemented`。

看过 `docs/development/kernel/mm/fuchsia-vm-page-source-pager.md` 之后的判断：Fuchsia 那套协议
（重叠合并、early wake、`SwapAsyncRequest`、引用环）的全部复杂度，是"供页者在另一个进程里"
这条边界的造价，每一样都为摊平一次 IPC 往返。我们没有这条边界。

匿名缺页根本不需要它：`m_page_source` 为空时"什么都没有"的答案不是去问谁，而是就地分配一页
零页，同步、不阻塞、永不产生 `ShouldWait`。所以第 1、2 条做完，mm 就已经是真正的按需分页，
而 PageRequest 一行都不用写。

真要做的时候，**必须保留**的两样与进程边界无关：`PageRequest` 由调用方在栈上分配
（缺页路径不能分配内存，否则可能递归触发回收再触发缺页——这正是
`require_page(nr_pages, MultiPageRequest*)` 这个参数一路穿下来的原因），以及
`page_fault_internal()` 里 `ShouldWait` 加 do-while 重试的形状。

`AnonymousPageRequest` 现在是空类，看着像可以删，但它在 Zircon 里正是"匿名页分配需要等回收
或压缩"的等待凭证。我们的 `PageQueues` 还是骨架，所以它现在确实无事可做——那是脚手架，不是死代码。

## 4. VMA 接口的两处签名问题

- `map_at()` 的 `VirtAddr *virt_base` 同时是输入提示和输出结果，空指针表示"你挑地址"。
  这是把 LAPIC 的 MMIO 放到 physmap 起点那个 bug 的温床（见存档第 36 节）。
- 偏移与绝对地址混用：`unmap`/`protect`/`reserve`/`create_subvma` 收相对 `m_base` 的偏移，
  而 `map_at`/`map` 返回绝对地址、`contains`/`find_mapping_locked` 收绝对地址。调用方要反复
  换算，`init_vmm.cpp` 里就写着 `phys_vma.base - root_vma->base()`。

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

入队/出队已经做了（见 `docs/changes/260903/mm.md` 第 6 节），还剩两件：

- `rotate_reclaimable_queues()` 只递增 `m_mrugen`，`m_lrugen` 从不前进，环填满之后没有任何
  处理，代际方案等于只有一代。
- `LruRunner::run()` 写的是 `while (State::Shutdown == m_state.load(...)) {}`——条件像是反的，
  它在"已关闭"时空转；`m_thread` 标着 `[[maybe_unused]]`，没有任何东西启动它。
  `start_runners()` 只有声明没有定义。

没有回收意味着内存紧张时缺页只能直接失败，在 1、2 条做完之后这会是第一个暴露的短板。

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
