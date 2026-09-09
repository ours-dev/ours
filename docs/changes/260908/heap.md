# heap 改动记录

## kmalloc 与 vmalloc 合并到 kernel/lib/heap（2026-09-08）

**文件**
- `kernel/lib/heap/` — 新库：`kmalloc.{hpp,cpp}`、`new.{hpp,cpp}`、`vmalloc_arena.{hpp,cpp}`、`tests/`
- `kernel/lib/ktl/` — 交出 kmalloc / new，只剩 `memory_resource.cpp`
- `kernel/mm/` — 交出 `vmalloc_arena.*`
- 全树 23 个文件把 `<ktl/new.hpp>` 改成 `<heap/new.hpp>`

**症状**
无，是结构调整。两个分配器原先分居两处：kmalloc 在 `kernel/lib/ktl`，
`VmallocArena` 在 `kernel/mm`。

**原因**
它们是**一件事的两半**，而先前的位置把这件事切开了。

kmalloc 服务内核里绝大多数的小对象，从 2 的幂 size class 出，背后是 physmap 里物理连续
的 slab；vmalloc 服务少数大块，从专用地址窗口出，逐页从 buddy 拿——虚拟连续、物理离散。
Linux 也是这条线（`mm/slub.c` 对 `mm/vmalloc.c`）。

真正要求它们同处一库的是**路由**：`operator new` 要按大小在两者之间选，`operator delete`
要按地址路由回去。把这个选择放在两个实现旁边，是让它在一处就能被审阅的唯一办法——
Linux 的 `kvmalloc()`/`kvfree()` 出于同样理由紧挨着 slab 分配器。

放在 `ktl` 里也不对：ktl 是内核的 C++ 模板层（容器、`Name`、`Result`），分配器不是模板；
放在 `kernel/mm` 里同样不对：vmalloc 是 mm 的**客户**（它调 `alloc_frames`、`map_bulk`），
不是 mm 的一部分。

**改法**
`git mv` 保留历史。命名空间随位置走：

| 改前 | 改后 |
|---|---|
| `ktl::kmalloc` / `ktl::kfree` / `ktl::init_kmalloc` | `heap::` 同名 |
| `ours::mm::VmallocArena` | `heap::VmallocArena` |
| `<ktl/kmalloc.hpp>` / `<ktl/new.hpp>` / `<ours/mm/vmalloc_arena.hpp>` | `<heap/…>` |

`operator new(usize, Gaf)` 的签名从 `ktl::Gaf` 改写成 `ours::mm::Gaf`。**这不是行为改动**：
`ktl::Gaf` 本来就是 `ours::mm::Gaf` 的别名，mangled name 一字不差，调用点一处不用动。
改它是为了让 heap 不依赖 ktl——ktl 依赖 `//kernel/mm:headers`，而 heap 也要依赖 mm，
取那个别名等于把 ktl 无谓地塞进这条路径上。

**依赖方向上唯一有意思的一处**：`//kernel/mm:headers` 必须依赖 `//kernel/lib/heap:headers`
（每个说 `new (gaf)` 的 mm 源文件都要 `<heap/new.hpp>`），而 heap 又需要 mm 的 `Gaf` 和
`PAGE_SIZE`。两个方向都取 target 依赖就是 GN 拒绝的环。解法是 heap 只取 mm 的**包含路径
config** 而不是 target：

```gn
cpp_interface("headers") {
  public_configs = [ "//kernel/mm:headers.config" ]
  public_deps = [ "//system/lib/ustl:headers", "//system/public" ]
}
```

heap 的 source_set 仍然取完整的 `//kernel/mm:headers`——那是另一个 target，不成环。
`//kernel/mm/tests` 早就用同一个手法（它的注释解释过：`//kernel/phys:handoff` 在宿主
工具链下求值不了）。

**参照**
- `Linux mm/slub.c` 与 `mm/vmalloc.c`：同样的两类分配器；`kvmalloc()`/`kvfree()` 的路由
  与 slab 同处一文件。
- 本树 `kernel/lib/lz4`、`kernel/lib/prof`：一个 lib 一件事的既有粒度。

**验证**
`ninja` 通过；宿主单测 `//kernel/lib/heap/tests:vmalloc-arena-smoke` 8 组全过；QEMU 实测
两个镜像，`k:vmalloc` 窗口照常保留，userboot 照常 `hello from ring 3`（dbg_1 897 次
用户态时钟中断），均以 signal 15 结束。全树 23 个包含点的改动经 `git diff` 确认**只有
include 行**。

**故意没做**
- **`operator new` 的路由还没接上**。这一步只搬家，`new` 仍然只走 kmalloc、>4 KiB 仍返回
  nullptr。路由是 vmalloc 本体到位之后的事。
- **`ktl::init_memory_resource()` 留在 ktl**。它是 ustl 容器的默认资源，属于 C++ 胶水，
  不是分配器；它调 `heap::kmalloc` 是正常的客户关系。

## vmalloc 本体：地址簿记接上物理页与页表（2026-09-08）

**文件**
- `kernel/lib/heap/include/heap/vmalloc.hpp` / `vmalloc.cpp` — 新增
- `kernel/lib/heap/BUILD.gn` — 登记
- `kernel/mm/init_vmm.cpp` — 空了很久的 `init_heap()` 终于有了内容

**症状**
无，新增。`init_heap()` 的注释从一开始就写着它在等什么：
"the VMM's own allocator: virtually contiguous, physically scattered"。

**原因与几处取舍**

**为什么不建在 VmObjectPaged 上。** 这是整个形状的决定性事实：`~VmCowPages` 是
`= default`，`VmPageMap` 没有释放页的析构，`decommit()` 返回 `Unimplemented`
（`vm_cow_pages.cpp:12`、`vm_object_paged.cpp:82`）。**VMO 死时它的页从不归还 PMM。**
建在上面的 vmalloc，`vfree` 只能拆 PTE 而漏光所有页——一个"能分配不能释放"的分配器比
没有更糟。所以它自持页：`alloc_frames` 拿散页，`ArchVmAspace::map` 装页表，
`free_frames` 还回去。`CpuLocal::Mapper::build_dense`（`cpu_local.cpp:334`）是树里同款
配方。

**为什么不记录物理页。** 每个活区间只记 `{base, nr_pages}`。物理页已经写在页表里了，
拆的时候用 `query()` 读回来，每页一次页表走查——换来的是不必为每个区间找地方存一份帧
列表，也不必维护"第二份真相"。Linux 之所以要 `struct vm_struct::pages`，是因为它的
unmap 是批量延迟的，页归还时页表早没了；这里的拆解是同步的，还读得到。

**拆解的顺序是反着 Linux 的，而且必须反。**

```cpp
for (每一页) {
    query(va, &pa);                  // 先读出帧
    unmap(va, 1);                    // 再断开翻译（含 invlpg）
    free_frames(phys_to_frame(pa));  // 最后才归还
}
```

Linux 是先还页、TLB 刷新推迟到 `__purge_vmap_area_lazy()`；它敢这样是因为 lazy purge
在刷新完成前一直扣着那段地址不放。这里没有这个兜底，所以一个在翻译失效之前就被归还的
帧，可能在别人手里的同时还能被陈旧 TLB 表项摸到。逐页做，等于每个帧只在**它自己的**
TLB 表项消失之后才被归还。

**guard page。** 每次多要一页地址、不映射它，越界立刻 #PF 而不是走进邻居。Linux 默认
如此（`VM_NO_GUARD` 是例外）。注意它**只计入地址预留**：映射和释放走的都是 `nr_pages`。
这正是 Linux 区分 `vm_struct::size` 与 `get_vm_area_size()` 的那条缝，两边写反就会多映射
或多释放一页。

**锁用 Mutex 不用 SpinLock。** 锁内的每一件事都可能阻塞：`alloc_frames` 要取当前线程的
resource lock 并可能走进 reclaim，页表走查要分配表页。用 Mutex 让"vmalloc 不可在中断
上下文调用"成为**设计的性质**，而不是一句注释里的警告。

**`READY` 标志不是防御性编程**：`operator new` 将来按大小路由到这里，而内核在
`init_vmm()` 之前就在分配（`SparseMemMap` 在 `InitLevel::Pmm` 就 `new` 数组）。太早调用
的人拿到的是和窗口耗尽一样的 nullptr——那条失败路径他本来就有。

**失败路径**逐个回滚：VA 拿不到→无副作用；帧拿不到→退 VA；映射到一半失败→归还刚弹出
的那个帧、归还链表里剩下的、把已装的页交给和 `vfree` 同一个拆解函数、再退 VA。
`map()` 逐页做而不是 `map_bulk`，是因为帧是以链表形式来的，批量就得先有个物理地址数组
放它们——而"不需要额外存储"正是这套设计的立足点。

**一个容易踩的细节**：帧必须从链表里**弹出**再用，不能遍历完就丢掉链表头。
`PmFrame` 的钩子是 auto-unlink 的，链表头析构时仍挂在上面的节点会指向一个已死的头。

**参照**
- `Linux __vmalloc_node_range()` / `vfree()` / `is_vmalloc_addr()` / `struct vm_struct`。
- 本树 `CpuLocal::Mapper::build_dense()`：预留窗口 + 自持帧 + arch map 的既有配方。

**验证**
`ninja` 通过；`clang-tidy` 只剩文件作用域静态量的 SCREAMING 命名告警（与
`KERNEL_ASPACE`、`SYSCALL_TABLE` 等既有写法一致）。QEMU 实测 `kernel.dev.dbg_1.obi`，
`init_heap()` 现在打出：

```
vmalloc: window [FFFFFF9000000000, FFFFFF9100000000), 1048576 pages
```

userboot 照常 `hello from ring 3`，900 次用户态时钟中断，无回归。

**这一步还没有真正分配过任何东西**——没有调用者。下一条改动加启动期自检，那才是第一次
执行 `vmalloc()`/`vfree()`。

**故意没做**
- **`operator new` 的路由**。等自检证明 vmalloc 自己是对的之后再接。
- **多核 TLB shootdown**。`unmap` 只做本地 `invlpg`，交回 arch（见
  `docs/repairs/260908/x86.md` 之外另记）。
- **`vzalloc`**。调用方传 `Gaf::Zero` 即可，不另开入口。
- **`vmap()`（映射调用方给的页）**、大页、per-cpu 缓存、lazy purge：Linux 有，这里都不
  需要，`.agents/tasks/vmalloc/DESIGN.md` 里逐条列了哪些是必需哪些是优化。

## vmalloc 的启动期自检（2026-09-08）

**文件**
- `kernel/lib/heap/vmalloc.cpp` / `include/heap/vmalloc.hpp` — 新增 `vmalloc_self_test()`
- `kernel/mm/init_vmm.cpp` — `init_heap()` 里紧接着调它

**症状**
无，新增。这是 `vmalloc()` / `vfree()` **第一次被执行**。

**原因**
旁边的宿主单测覆盖不到这里。地址簿记（`VmallocArena`）是纯算术，脱离内核可测；而帧、
页表、guard page 和拆解顺序，只有在有 MMU 的机器上才有意义。这段自检是能同时碰到这四样
东西的最小程序。

**最要紧的是最后一条检查**：其余每一条失败都会响——写不进去就是 #PF，越界请求返回非空
就是断言。唯独**泄漏不会响**，而泄漏恰恰是拆解顺序和 guard page 算术最容易写错的方向：
guard page 被误映射、帧被丢掉、地址没还回去，三者都表现为"启动照常、只是窗口少了几页"。
所以自检比较分配前后的空闲页数——这一条捕获全部三种。

**为什么在两个镜像里都跑，而不是只在 `KERNEL_DEBUG_LEVEL=1` 里**：dbg 镜像开着
`DEBUG_ASSERT`，release 不开。一个只在断言里成立的不变量，在 release 镜像里就是静默
损坏；而 vmalloc 将来要被 `operator new` 依赖，两个镜像都得证明。代价是启动期三次分配。

**用例**
- 一页，从头写到尾——映射短了或权限不对会直接 #PF；
- `2 * PAGE_SIZE + 1` 字节：既不是页整数倍也不是 2 的幂。向上取整是 off-by-one 的藏身处，
  而它替代的 `get_frames_n()` 会把这个尺寸**悄悄取整到 4 页**；
- 两个活区间之间的间隔 ≥ 一页，证明 guard page 真的隔开了它们；
- 一个窗口外的内核地址不会被 `is_vmalloc_addr()` 认成 vmalloc 的；
- 零尺寸、超过窗口的尺寸都被拒；
- `vfree(nullptr)` 无害；
- **分配前后空闲页数相等**。

**验证**
两个镜像实测均打出：

```
vmalloc: window [FFFFFF9000000000, FFFFFF9100000000), 1048576 pages
vmalloc: self test passed
```

userboot 照常进入 ring 3（dbg_1 920 次用户态时钟中断），均以 signal 15 结束。

**故意没做**
- **没有测多核**。TLB shootdown 缺失是已知的，单核测不出来。
- **没有测窗口耗尽后的行为**。要占掉 4 GiB 地址空间才能到那一步，启动期不值得；
  arena 那一侧的宿主单测已经覆盖了"耗尽返回 0"。
- **没有做成可反复运行的 kshell 命令**。等真有人怀疑它的时候再加。
